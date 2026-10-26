#include "LyricsOverlay.h"

#include "OverlayCommon.h"

#include "md/lyrics/Karaoke.h"
#include "md/overlay/SurfacePolicy.h"
#include "md/util/Log.h"
#include "md/util/Text.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <map>

namespace md::platform {

using winrt::com_ptr;
namespace ov = md::overlay;
using lyrics::RowContent;
using lyrics::RowKind;

namespace {

constexpr wchar_t kClass[] = L"MediaDuet.LyricsOverlay";
constexpr wchar_t kUnlockClass[] = L"MediaDuet.LyricsUnlock";
constexpr double kFadeMs = 260;         // row cross-fade
constexpr int64_t kAutoHideMs = 4000;
constexpr int64_t kTopMs = 2000;        // topmost re-assert / full-screen re-check
constexpr int64_t kLockPollMs = 200;    // cursor poll while locked
constexpr int64_t kDwellMs = 600;       // rest over locked lyrics before the unlock button shows
constexpr int64_t kUnlockLingerMs = 1500;
constexpr int64_t kHoverPollMs = 120;
constexpr float kPad = 12.f;
constexpr float kToolbarH = 44.f;       // reserved strip at the top (toolbar / unlock button)
constexpr float kRowGap = 2.f;
constexpr float kLineH = 1.42f;         // row height / font size
constexpr int kFontStep = 2;            // toolbar A- / A+ and the menu (DIP)
constexpr int kMinFontSize = 12, kMaxFontSize = 96;   // settings lyrics.overlay.fontSize range
constexpr int kFontSizePresets[] = {24, 28, 32, 40, 48, 56, 64};
constexpr float kSecondaryTr = 0.72f;   // translation row size
constexpr float kSecondaryNext = 0.8f;  // "next line" row size (center layout)
constexpr float kUnlockDip = 36.f;

enum Btn : int { BPrev, BPlay, BNext, BSmaller, BLarger, BEarlier, BLater, BLock, BSettings, BClose, BCount };

D2D1_COLOR_F parseColor(const std::string& s, D2D1_COLOR_F fallback) {
    // #AARRGGBB or #RRGGBB
    if ((s.size() != 9 && s.size() != 7) || s[0] != '#') return fallback;
    for (size_t i = 1; i < s.size(); ++i)
        if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return fallback;
    unsigned long v = std::strtoul(s.c_str() + 1, nullptr, 16);
    float a = s.size() == 9 ? ((v >> 24) & 0xFF) / 255.f : 1.f;
    return {((v >> 16) & 0xFF) / 255.f, ((v >> 8) & 0xFF) / 255.f, (v & 0xFF) / 255.f, a};
}

float easeOut(float t) { return 1.f - (1.f - t) * (1.f - t) * (1.f - t); }
float luma(D2D1_COLOR_F c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }
D2D1_COLOR_F withAlpha(D2D1_COLOR_F c, float a) {
    c.a *= a;
    return c;
}

std::wstring formatOffset(int64_t ms) {
    wchar_t buf[32];
    swprintf_s(buf, L"%+.1f s", static_cast<double>(ms) / 1000.0);
    if (ms == 0) swprintf_s(buf, L"0.0 s");
    return buf;
}

} // namespace

struct LyricsOverlay::Impl {
    IScheduler& sched;
    Callbacks cb;
    TestOptions test;
    LyricsOverlay* owner;
    HWND hwnd = nullptr;
    HWND tip = nullptr;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    // configuration
    std::wstring fontFamily = L"Microsoft YaHei UI";
    float fontSize = 32.f;
    float opacity = 0.92f;
    bool karaoke = true, twoLines = true, locked = false, clickThrough = false, autoHide = true;
    bool topmost = true, hideInFullscreen = false;
    lyrics::RowLayout layout = lyrics::RowLayout::Alternate;
    bool translation = true;
    D2D1_COLOR_F active{1, 1, 1, 1}, inactive{1, 1, 1, 0.7f};
    std::optional<int> posX, posY;
    float widthDip = 900.f;
    bool animations = true, lowPower = false;
    Strings strings;

    // content
    std::shared_ptr<const lyrics::Lyrics> lyr;
    lyrics::LyricRows lyrRows; // of lyr (shared row model: gaps, end of lyrics)
    int mainLine = -1;         // the line in the main row (updateRows)
    bool trSong = false; // translations shown for this song
    std::string trackId;
    std::shared_ptr<const lyrics::LyricsClock> clock;
    int64_t delayMs = 0, manualOffset = 0;
    bool canOffset = false;
    bool playing = false;
    int64_t pausedAt = 0;
    std::map<int, lyrics::KaraokeLine> karas;

    // rows (what each row shows, what it showed before, when it changed)
    RowContent rowNow[2], rowPrev[2];
    bool centeredNow = false, centeredPrev[2] = {false, false};
    double rowChangedAt[2] = {-1e9, -1e9};

    // window state
    bool hover = false;
    int hoverBtn = -1;
    D2D1_RECT_F btn[BCount]{};
    bool fullscreenBlocked = false;
    bool shown = false;
    bool placementDirty = true;
    UINT dpi = 96;
    SIZE lastSize{0, 0};

    // unlock button (locked mode)
    HWND unlockHwnd = nullptr;
    HWND unlockTip = nullptr;
    LayeredSurface unlockSurface;
    bool unlockHover = false;
    int64_t dwellStart = -1, unlockLastNear = -1;

    // rendering resources
    LayeredSurface surface;
    TextCache texts;
    com_ptr<ID2D1Layer> layer;
    uint64_t layerGen = 0; // layers belong to one render target
    com_ptr<ID2D1StrokeStyle> roundStroke;

    ScopedTimer frameTimer, hideTimer, topTimer, lockTimer, hoverTimer;
    HWINEVENTHOOK fgHook = nullptr;

    Impl(IScheduler& s, Callbacks c, TestOptions t, LyricsOverlay* o)
        : sched(s), cb(std::move(c)), test(std::move(t)), owner(o), frameTimer(&s), hideTimer(&s), topTimer(&s),
          lockTimer(&s), hoverTimer(&s) {
        if (!test.now) test.now = [] { return lyrics::steadyNowMs(); };
        if (test.offscreen) dpi = test.dpi;
    }

    ~Impl() {
        *alive = false;
        if (fgHook) UnhookWinEvent(fgHook);
        if (hookTarget() == this) hookTarget() = nullptr;
        destroyUnlock();
        surface.release();
        if (hwnd) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            DestroyWindow(hwnd);
        }
    }

    double now() const { return test.now(); }
    float scale() const { return dpi / 96.f; }

    static Impl*& hookTarget() {
        static Impl* t = nullptr;
        return t;
    }

    static void CALLBACK onForeground(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
        if (auto* self = hookTarget()) {
            self->checkFullscreen();
            self->raise();
        }
    }

    // ------------------------------------------------------------ z-order / full screen
    void checkFullscreen() {
        if (test.offscreen) return;
        QUERY_USER_NOTIFICATION_STATE st{};
        bool blocked = SUCCEEDED(SHQueryUserNotificationState(&st)) &&
                       ov::overlayBlockedByFullscreen(static_cast<ov::NotifyState>(st), hideInFullscreen);
        if (blocked != fullscreenBlocked) {
            fullscreenBlocked = blocked;
            refresh();
        }
    }

    // Re-asserts "always on top" without activating anything.
    void raise() {
        if (!topmost || !shown || !hwnd) return;
        UINT f = SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER;
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, f);
        if (unlockHwnd && IsWindowVisible(unlockHwnd)) SetWindowPos(unlockHwnd, HWND_TOPMOST, 0, 0, 0, 0, f);
    }

    // Every 2 s while shown (z-order) or blocked by full screen (the foreground
    // event can fire before the state settles, which would leave us hidden).
    void armTopTimer() {
        if (test.offscreen) return;
        bool want = owner->enabled_ && (shown || fullscreenBlocked);
        if (!want) {
            topTimer.cancel();
            return;
        }
        if (!topTimer.active()) topTimer.start(kTopMs, [this] {
            checkFullscreen();
            raise();
            armTopTimer();
        });
    }

    // ------------------------------------------------------------ window
    bool ensureWindow() {
        if (test.offscreen) return false;
        if (hwnd && !IsWindow(hwnd)) hwnd = nullptr;
        if (hwnd) return true;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
        RegisterClassExW(&wc);
        DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (topmost ? WS_EX_TOPMOST : 0);
        hwnd = CreateWindowExW(ex, kClass, L"MediaDuet Lyrics", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance,
                               nullptr);
        if (!hwnd) return false;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        applyStyle();
        if (!fgHook) {
            hookTarget() = this;
            fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, onForeground, 0, 0,
                                     WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        }
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
        InitCommonControlsEx(&icc);
        tip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                              0, 0, 0, 0, hwnd, nullptr, wc.hInstance, nullptr);
        return true;
    }

    void applyStyle() {
        if (!hwnd) return;
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (locked && clickThrough) ex |= WS_EX_TRANSPARENT;
        else ex &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
        SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }

    bool hasSecondRow() const {
        if (trSong) return true;
        return twoLines;
    }
    float secondaryScale() const {
        if (trSong) return kSecondaryTr;
        return layout == lyrics::RowLayout::Alternate ? 1.f : kSecondaryNext;
    }
    float row0H() const { return fontSize * kLineH; }
    float row1H() const { return fontSize * secondaryScale() * kLineH; }
    float heightDip() const { return kToolbarH + kPad + row0H() + (hasSecondRow() ? kRowGap + row1H() : 0.f) + kPad; }

    void computePlacement(POINT& pt, SIZE& sz) {
        if (test.offscreen) {
            sz = {static_cast<LONG>(std::lround(widthDip * scale())), static_cast<LONG>(std::lround(heightDip() * scale()))};
            pt = {0, 0};
            return;
        }
        HMONITOR mon = nullptr;
        if (posX && posY) mon = MonitorFromPoint(POINT{*posX, *posY}, MONITOR_DEFAULTTONULL);
        if (!mon) {
            posX.reset();
            posY.reset();
            mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        }
        UINT dx = 96, dy = 96;
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        static auto getDpi = reinterpret_cast<GetDpiForMonitorFn>(
            GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor"));
        if (getDpi) getDpi(mon, 0, &dx, &dy);
        dpi = dx;
        float s = scale();
        sz = {static_cast<LONG>(std::lround(widthDip * s)), static_cast<LONG>(std::lround(heightDip() * s))};
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        auto wa = mi.rcWork;
        if (posX && posY) pt = {*posX, *posY};
        else pt = {wa.left + (wa.right - wa.left - sz.cx) / 2, wa.bottom - sz.cy - static_cast<LONG>(40 * s)};
        pt.x = std::clamp(pt.x, wa.left - sz.cx + 80, wa.right - 80);
        pt.y = std::clamp(pt.y, wa.top - static_cast<LONG>(kToolbarH * s), wa.bottom - 40);
    }

    // ------------------------------------------------------------ timing
    double position() const {
        if (!clock) return -1;
        auto p = clock->position(now());
        return p ? *p - static_cast<double>(delayMs) : -1;
    }

    bool shouldShow() const {
        if (!owner->enabled_ || fullscreenBlocked || !lyr || !lyr->synced || lyrRows.rows.empty()) return false;
        if (autoHide && !playing && sched.nowMs() - pausedAt > kAutoHideMs) return false;
        return true;
    }

    const lyrics::KaraokeLine& kara(int i) {
        auto it = karas.find(i);
        if (it != karas.end()) return it->second;
        if (karas.size() > 12) karas.clear();
        return karas.emplace(i, lyrics::buildKaraoke(lyr->lines[i])).first->second;
    }

    int64_t rowPosition() const {
        double p = position();
        return p < 0 ? -1 : static_cast<int64_t>(p);
    }

    // Updates which content each row shows; a changed row starts a cross-fade.
    void updateRows() {
        if (!lyr) return;
        auto d = lyrics::desktopRows(*lyr, lyrRows, rowPosition(), layout, twoLines, translation, &mainLine);
        double t = now();
        bool layoutChanged = d.centered != centeredNow;
        for (int r = 0; r < 2; ++r) {
            RowContent want = d.row[r];
            bool same = want.kind == rowNow[r].kind && want.line == rowNow[r].line;
            if (same && !layoutChanged) {
                rowNow[r].active = want.active;
                continue;
            }
            rowPrev[r] = rowNow[r];
            centeredPrev[r] = centeredNow;
            rowNow[r] = want;
            rowChangedAt[r] = (animations && !lowPower) ? t : -1e9;
        }
        centeredNow = d.centered;
    }

    float fadeT(int r) const {
        double el = now() - rowChangedAt[r];
        if (el >= kFadeMs) return 1.f;
        return easeOut(static_cast<float>(std::clamp(el / kFadeMs, 0.0, 1.0)));
    }

    void refresh() {
        frameTimer.cancel();
        if (test.offscreen) return;
        if (!shouldShow()) {
            if (shown && hwnd) ShowWindow(hwnd, SW_HIDE);
            shown = false;
            hideUnlock();
            lockTimer.cancel();
            hoverTimer.cancel();
            surface.release();
            armTopTimer();
            return;
        }
        if (!ensureWindow()) return;
        updateRows();
        render();
        if (!shown) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            shown = true;
            raise();
        }
        armTopTimer();
        armLockPoll();
        scheduleNext();
    }

    // Next frame: continuously while a fill moves / a fade runs, otherwise at the
    // next word or line start.
    void scheduleNext() {
        if (!shown || !lyr) return;
        double t = now();
        bool fading = t - rowChangedAt[0] < kFadeMs || t - rowChangedAt[1] < kFadeMs;
        bool moving = false;
        double next = -1;
        double p = position();
        double rate = clock ? std::max(0.1, clock->rate()) : 1.0;
        if (playing) {
            for (int r = 0; r < 2; ++r)
                if (rowNow[r].kind == RowKind::Line && rowNow[r].active && karaoke) {
                    double n = -1;
                    if (lyrics::fillMoving(kara(rowNow[r].line), p, &n)) moving = true;
                    else if (n >= 0) next = next < 0 ? n : std::min(next, n);
                }
            int64_t change = lyrRows.nextChange(rowPosition());
            if (change >= 0) next = next < 0 ? static_cast<double>(change) : std::min(next, static_cast<double>(change));
        }
        int64_t delay;
        if (fading || moving) {
            // Requests below the 15.6 ms timer tick round up to it: ~60 fps.
            delay = lowPower ? 100 : 10;
        } else if (playing && next >= 0) {
            delay = std::clamp<int64_t>(static_cast<int64_t>((next - p) / rate), 15, 60'000);
        } else {
            return;
        }
        frameTimer.start(delay, [this] { refresh(); });
    }

    // ------------------------------------------------------------ drawing
    D2D1_COLOR_F outlineColor(float alpha) const {
        bool darkText = luma(active) < 0.45f;
        return darkText ? D2D1::ColorF(1, 1, 1, 0.7f * alpha) : D2D1::ColorF(0.02f, 0.02f, 0.05f, 0.62f * alpha);
    }

    // Draws one text geometry at (x, y) scaled by k: shadow, (glow on the sung
    // part), outline, fill. fill: karaoke edge in layout units (nullptr = solid).
    void drawText(const TextEntry& e, float x, float y, float k, D2D1_COLOR_F color, float alpha, const float* fillX,
                  D2D1_COLOR_F unsung, float sizeDip) {
        auto* rt = surface.rt();
        if (!e.geometry || alpha <= 0.01f) return;
        D2D1_MATRIX_3X2_F old;
        rt->GetTransform(&old);
        auto at = [&](float dx, float dy) {
            rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(x + dx, y + dy) * old);
        };
        float stroke = std::max(1.3f, sizeDip * 0.07f) / k;
        // Soft shadow: two wide, faint strokes plus a filled copy, offset down.
        com_ptr<ID2D1SolidColorBrush> sh;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.16f * alpha), sh.put());
        at(0, sizeDip * 0.05f);
        rt->DrawGeometry(e.geometry.get(), sh.get(), stroke * 3.2f, roundStroke.get());
        rt->DrawGeometry(e.geometry.get(), sh.get(), stroke * 2.1f, roundStroke.get());
        at(0, 0);
        com_ptr<ID2D1Brush> fillBrush, glowBrush;
        if (fillX) {
            float feather = std::max(6.f, sizeDip * 0.5f) / k;
            D2D1_COLOR_F on = withAlpha(color, alpha), off = withAlpha(unsung, alpha);
            D2D1_GRADIENT_STOP stops[] = {{0.f, on}, {1.f, off}};
            com_ptr<ID2D1GradientStopCollection> coll;
            com_ptr<ID2D1LinearGradientBrush> gb;
            auto props = D2D1::LinearGradientBrushProperties(D2D1::Point2F(*fillX - feather / 2, 0),
                                                             D2D1::Point2F(*fillX + feather / 2, 0));
            if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, coll.put())) &&
                SUCCEEDED(rt->CreateLinearGradientBrush(props, coll.get(), gb.put())))
                fillBrush = gb;
            // A faint glow of the active colour around what has been sung.
            D2D1_GRADIENT_STOP gstops[] = {{0.f, withAlpha(color, 0.28f * alpha)}, {1.f, withAlpha(color, 0.f)}};
            com_ptr<ID2D1GradientStopCollection> gcoll;
            com_ptr<ID2D1LinearGradientBrush> glow;
            if (luma(color) > 0.3f &&
                SUCCEEDED(rt->CreateGradientStopCollection(gstops, 2, gcoll.put())) &&
                SUCCEEDED(rt->CreateLinearGradientBrush(props, gcoll.get(), glow.put())))
                glowBrush = glow;
        }
        if (!fillBrush) {
            com_ptr<ID2D1SolidColorBrush> sb;
            rt->CreateSolidColorBrush(withAlpha(color, alpha), sb.put());
            fillBrush = sb;
        }
        if (glowBrush) rt->DrawGeometry(e.geometry.get(), glowBrush.get(), stroke * 4.f, roundStroke.get());
        com_ptr<ID2D1SolidColorBrush> ol;
        rt->CreateSolidColorBrush(outlineColor(alpha), ol.put());
        rt->DrawGeometry(e.geometry.get(), ol.get(), stroke * 2.f, roundStroke.get());
        rt->FillGeometry(e.geometry.get(), fillBrush.get());
        rt->SetTransform(old);
    }

    struct RowGeom {
        float top, height, size;
    };
    RowGeom rowGeom(int r) const {
        float y0 = kToolbarH + kPad;
        if (r == 0) return {y0, row0H(), fontSize};
        return {y0 + row0H() + kRowGap, row1H(), fontSize * secondaryScale()};
    }

    // One row's content at alpha. centered / alignRight: placement when it fits.
    void drawRow(int r, const RowContent& c, bool centered, float alpha, double p) {
        if (c.kind == RowKind::None || alpha <= 0.01f || !lyr || c.line < 0 ||
            c.line >= static_cast<int>(lyr->lines.size()))
            return;
        auto g = rowGeom(r);
        float size = c.kind == RowKind::Translation ? fontSize * kSecondaryTr
                     : (r == 0 || !centered || trSong) ? fontSize
                                                        : fontSize * kSecondaryNext;
        if (r == 1 && !centered) size = fontSize; // alternate rows share the size
        std::wstring s;
        const lyrics::KaraokeLine* k = nullptr;
        if (c.kind == RowKind::Translation) {
            s = text::toWide(text::trim(lyr->lines[c.line].translation));
        } else {
            k = &kara(c.line);
            s = k->text.empty() ? std::wstring(L"\x266A") : k->text;
        }
        if (s.empty()) return;
        auto weight = c.kind == RowKind::Translation ? DWRITE_FONT_WEIGHT_MEDIUM : DWRITE_FONT_WEIGHT_BOLD;
        const TextEntry* e = texts.get(s, fontFamily, size, weight, true);
        if (!e) return;
        float W = surface.size().cx / scale();
        float avail = W - 2 * kPad;
        float ks = lyrics::fitScale(e->width, avail, 0.85f);
        float tw = e->width * ks;
        bool isActive = c.kind == RowKind::Line && c.active;
        bool past = c.kind == RowKind::Line && !c.active && p >= 0 && c.line < mainLine; // fading out after it was sung
        std::optional<float> fx;
        if (isActive && karaoke && k && !k->text.empty()) fx = fillX(e->carets, lyrics::fillAt(*k, p));
        float x;
        float off = 0.f;
        bool overflow = tw > avail + 0.5f;
        if (overflow) {
            off = fx ? lyrics::scrollOffset(tw, avail, *fx * ks) : 0.f;
            x = kPad - off;
        } else if (centered) {
            x = (W - tw) / 2;
        } else {
            x = r == 1 ? W - kPad - tw : kPad;
        }
        float y = g.top + (g.height - e->height * ks) / 2;
        com_ptr<ID2D1Brush> mask;
        float bleed = size * 0.3f;
        if (overflow)
            mask = pushFadeClip(surface.rt(), layer.get(), D2D1::RectF(kPad - 2, g.top - bleed, W - kPad + 2, g.top + g.height + bleed),
                                off > 0.5f, off < tw - avail - 0.5f, std::min(48.f, avail * 0.08f));
        D2D1_COLOR_F color = c.kind == RowKind::Translation ? withAlpha(active, 0.92f)
                             : (isActive && !karaoke) || past ? active
                                                              : inactive;
        if (fx) drawText(*e, x, y, ks, active, alpha, &*fx, inactive, size);
        else drawText(*e, x, y, ks, color, alpha, nullptr, inactive, size);
        if (overflow) surface.rt()->PopLayer();
    }

    void drawToolbar(float W) {
        auto* rt = surface.rt();
        int n = BCount;
        float bw = 30.f, sep = 9.f;
        float labelW = manualOffset != 0 ? 44.f : 0.f;
        float total = n * bw + 3 * sep + labelW + 12.f;
        float k = std::min(1.f, (W - 16.f) / total);
        bw *= k;
        sep *= k;
        labelW *= k;
        total *= k;
        float h = 34.f * k;
        float x0 = (W - total) / 2, y0 = (kToolbarH - h) / 2;
        auto pill = D2D1::RoundedRect(D2D1::RectF(x0, y0, x0 + total, y0 + h), h / 2, h / 2);
        com_ptr<ID2D1SolidColorBrush> bg, border, fg, hi;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.07f, 0.07f, 0.09f, 0.72f), bg.put());
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.16f), border.put());
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.95f), fg.put());
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.14f), hi.put());
        rt->FillRoundedRectangle(pill, bg.get());
        // Glass sheen on the upper half.
        D2D1_GRADIENT_STOP st[] = {{0.f, D2D1::ColorF(1, 1, 1, 0.10f)}, {1.f, D2D1::ColorF(1, 1, 1, 0.f)}};
        com_ptr<ID2D1GradientStopCollection> coll;
        com_ptr<ID2D1LinearGradientBrush> sheen;
        if (SUCCEEDED(rt->CreateGradientStopCollection(st, 2, coll.put())) &&
            SUCCEEDED(rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, y0), D2D1::Point2F(0, y0 + h * 0.55f)), coll.get(),
                sheen.put())))
            rt->FillRoundedRectangle(pill, sheen.get());
        rt->DrawRoundedRectangle(pill, border.get(), 1.f);
        float x = x0 + 6.f * k;
        auto place = [&](int b) {
            btn[b] = D2D1::RectF(x, y0, x + bw, y0 + h);
            x += bw;
        };
        auto separator = [&] {
            rt->DrawLine(D2D1::Point2F(x + sep / 2, y0 + h * 0.28f), D2D1::Point2F(x + sep / 2, y0 + h * 0.72f), border.get(), 1.f);
            x += sep;
        };
        place(BPrev);
        place(BPlay);
        place(BNext);
        separator();
        place(BSmaller);
        place(BLarger);
        separator();
        place(BEarlier);
        D2D1_RECT_F label = D2D1::RectF(x, y0, x + labelW, y0 + h);
        x += labelW;
        place(BLater);
        separator();
        place(BLock);
        place(BSettings);
        place(BClose);
        auto glyph = [&](int b, const wchar_t* g, float size, const std::wstring& family) {
            if (b == hoverBtn) {
                auto r = btn[b];
                float pad = 3.f * k;
                rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + pad, r.top + pad, r.right - pad, r.bottom - pad),
                                                           (h - 2 * pad) / 2, (h - 2 * pad) / 2),
                                         hi.get());
            }
            auto* e = texts.get(g, family, size * k, family == iconFontFamily() ? DWRITE_FONT_WEIGHT_NORMAL
                                                                                 : DWRITE_FONT_WEIGHT_SEMI_BOLD);
            if (!e) return;
            auto r = btn[b];
            bool off = (b == BEarlier || b == BLater) && !canOffset;
            com_ptr<ID2D1SolidColorBrush> c;
            rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, off ? 0.35f : 0.95f), c.put());
            rt->DrawTextLayout(D2D1::Point2F((r.left + r.right - e->width) / 2, (r.top + r.bottom - e->height) / 2),
                               e->layout.get(), c.get());
        };
        std::wstring icons = iconFontFamily();
        glyph(BPrev, L"\xE892", 13, icons);
        glyph(BPlay, playing ? L"\xE769" : L"\xE768", 14, icons);
        glyph(BNext, L"\xE893", 13, icons);
        glyph(BSmaller, L"\xE8E7", 14, icons);
        glyph(BLarger, L"\xE8E8", 14, icons);
        glyph(BEarlier, L"\x2212" L"0.1", 10.5f, L"Segoe UI");
        glyph(BLater, L"+0.1", 10.5f, L"Segoe UI");
        glyph(BLock, L"\xE72E", 13, icons);
        glyph(BSettings, L"\xE713", 13, icons);
        glyph(BClose, L"\xE711", 12, icons);
        if (labelW > 0) {
            if (auto* e = texts.get(formatOffset(manualOffset), L"Segoe UI", 10.f * k, DWRITE_FONT_WEIGHT_NORMAL)) {
                com_ptr<ID2D1SolidColorBrush> c;
                rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.7f), c.put());
                rt->DrawTextLayout(D2D1::Point2F((label.left + label.right - e->width) / 2, (label.top + label.bottom - e->height) / 2),
                                   e->layout.get(), c.get());
            }
        }
    }

    bool ensureResources(SIZE sz) {
        if (!surface.ensure(d2dFactory(), sz, dpi)) return false;
        if (!roundStroke) {
            auto props = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                     D2D1_LINE_JOIN_ROUND);
            d2dFactory()->CreateStrokeStyle(props, nullptr, 0, roundStroke.put());
        }
        return true;
    }

    void draw(SIZE sz) {
        surface.begin();
        auto* rt = surface.rt();
        if (!layer || layerGen != surface.generation()) {
            layer = nullptr;
            rt->CreateLayer(nullptr, layer.put());
            layerGen = surface.generation();
        }
        float W = sz.cx / scale(), H = sz.cy / scale();
        bool showToolbar = hover && !locked;
        if (showToolbar) {
            // Glass panel behind the lyrics while hovering (and hit-testable everywhere).
            com_ptr<ID2D1SolidColorBrush> bg, border;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.05f, 0.05f, 0.07f, 0.34f), bg.put());
            rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.14f), border.put());
            auto rr = D2D1::RoundedRect(D2D1::RectF(1, kToolbarH * 0.5f, W - 1, H - 1), 14, 14);
            rt->FillRoundedRectangle(rr, bg.get());
            rt->DrawRoundedRectangle(rr, border.get(), 1.f);
        }
        double p = position();
        for (int r = 0; r < 2; ++r) {
            float t = fadeT(r);
            if (t < 1.f) drawRow(r, rowPrev[r], centeredPrev[r], 1.f - t, p);
            drawRow(r, rowNow[r], centeredNow, t, p);
        }
        if (showToolbar) drawToolbar(W);
        else
            for (auto& b : btn) b = {};
    }

    void render() {
        POINT pt;
        SIZE sz;
        computePlacement(pt, sz);
        if (!ensureResources(sz)) return;
        if (sz.cx != lastSize.cx || sz.cy != lastSize.cy) {
            lastSize = sz;
            placementDirty = true;
        }
        draw(sz);
        if (!surface.end()) return;
        if (test.offscreen || !hwnd) return;
        BYTE alpha = static_cast<BYTE>(std::clamp(opacity, 0.2f, 1.f) * 255);
        // Keep the current position unless placement changed, so dragging is not fought.
        surface.present(hwnd, placementDirty || !shown ? &pt : nullptr, alpha);
        if (placementDirty) updateTooltips();
        placementDirty = false;
    }

    // ------------------------------------------------------------ tooltips
    void updateTooltips() {
        if (!tip || !hwnd) return;
        int count = static_cast<int>(SendMessageW(tip, TTM_GETTOOLCOUNT, 0, 0));
        for (int i = count - 1; i >= 0; --i) {
            TTTOOLINFOW ti{sizeof(ti)};
            if (SendMessageW(tip, TTM_ENUMTOOLSW, i, reinterpret_cast<LPARAM>(&ti))) SendMessageW(tip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        }
        if (locked) return;
        // Same layout as drawToolbar at full scale (rects are refreshed on hover).
        const std::wstring* text[BCount] = {&strings.previous, &strings.playPause, &strings.next,  &strings.smaller,
                                            &strings.larger,   &strings.earlier,   &strings.later, &strings.lock,
                                            &strings.settings, &strings.close};
        for (int b = 0; b < BCount; ++b) {
            TTTOOLINFOW ti{sizeof(ti)};
            ti.uFlags = TTF_SUBCLASS;
            ti.hwnd = hwnd;
            ti.uId = static_cast<UINT_PTR>(b + 1);
            ti.lpszText = const_cast<wchar_t*>(text[b]->c_str());
            ti.rect = {0, 0, 0, 0};
            SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        }
    }

    void syncTooltipRects() {
        if (!tip || !hwnd) return;
        float s = scale();
        for (int b = 0; b < BCount; ++b) {
            TTTOOLINFOW ti{sizeof(ti)};
            ti.hwnd = hwnd;
            ti.uId = static_cast<UINT_PTR>(b + 1);
            ti.rect = {static_cast<LONG>(btn[b].left * s), static_cast<LONG>(btn[b].top * s),
                       static_cast<LONG>(btn[b].right * s), static_cast<LONG>(btn[b].bottom * s)};
            SendMessageW(tip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
        }
    }

    // ------------------------------------------------------------ input
    int buttonAt(float x, float y) const {
        for (int b = 0; b < BCount; ++b)
            if (x >= btn[b].left && x <= btn[b].right && y >= btn[b].top && y <= btn[b].bottom) return b;
        return -1;
    }

    void setHover(bool h) {
        if (h == hover) return;
        hover = h;
        if (h && !hoverTimer.active()) armHoverPoll();
        if (shown) render();
        if (h) syncTooltipRects();
    }

    // While hovering: notice when the cursor leaves (client and caption areas
    // alternate, so TrackMouseEvent alone is not reliable here).
    void armHoverPoll() {
        hoverTimer.start(kHoverPollMs, [this] {
            if (!hwnd || !shown) {
                hover = false;
                return;
            }
            POINT c;
            RECT r;
            GetCursorPos(&c);
            GetWindowRect(hwnd, &r);
            if (!PtInRect(&r, c)) {
                hoverBtn = -1;
                setHover(false);
                return;
            }
            armHoverPoll();
        });
    }

    void onButton(int b) {
        switch (b) {
        case BPrev: if (cb.onCommand) cb.onCommand(media::CommandAction::Previous); break;
        case BPlay: if (cb.onCommand) cb.onCommand(media::CommandAction::Toggle); break;
        case BNext: if (cb.onCommand) cb.onCommand(media::CommandAction::Next); break;
        case BSmaller: if (cb.onFontSize) cb.onFontSize(-kFontStep); break;
        case BLarger: if (cb.onFontSize) cb.onFontSize(kFontStep); break;
        case BEarlier: if (cb.onOffset && canOffset) cb.onOffset(-100, false); break;
        case BLater: if (cb.onOffset && canOffset) cb.onOffset(100, false); break;
        case BLock: if (cb.onLockChanged) cb.onLockChanged(true); break;
        case BSettings: if (cb.onOpenSettings) cb.onOpenSettings(); break;
        case BClose: if (cb.onHideRequested) cb.onHideRequested(); break;
        default: break;
        }
    }

    void showMenu(POINT at) {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, locked ? strings.unlock.c_str() : strings.lock.c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        UINT off = canOffset ? MF_STRING : MF_STRING | MF_GRAYED;
        AppendMenuW(m, off, 4, strings.earlier.c_str());
        AppendMenuW(m, off, 5, strings.later.c_str());
        std::wstring reset = strings.resetOffset;
        if (auto pos = reset.find(L"%s"); pos != std::wstring::npos) reset.replace(pos, 2, formatOffset(manualOffset));
        AppendMenuW(m, canOffset && manualOffset != 0 ? MF_STRING : MF_STRING | MF_GRAYED, 6, reset.c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        // Text size: a step either way, or one of the common sizes (ids 100 + size).
        int size = static_cast<int>(std::lround(fontSize));
        HMENU sizes = CreatePopupMenu();
        AppendMenuW(sizes, size < kMaxFontSize ? MF_STRING : MF_STRING | MF_GRAYED, 7, strings.larger.c_str());
        AppendMenuW(sizes, size > kMinFontSize ? MF_STRING : MF_STRING | MF_GRAYED, 8, strings.smaller.c_str());
        AppendMenuW(sizes, MF_SEPARATOR, 0, nullptr);
        for (int preset : kFontSizePresets)
            AppendMenuW(sizes, MF_STRING | (preset == size ? MF_CHECKED : 0), 100 + preset, std::to_wstring(preset).c_str());
        std::wstring sizeTitle = strings.fontSize;
        if (auto pos = sizeTitle.find(L"%s"); pos != std::wstring::npos) sizeTitle.replace(pos, 2, std::to_wstring(size));
        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(sizes), sizeTitle.c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 2, strings.hide.c_str());
        AppendMenuW(m, MF_STRING, 3, strings.settings.c_str());
        SetForegroundWindow(hwnd);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, 0, hwnd, nullptr);
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(m);
        if (cmd == 1 && cb.onLockChanged) cb.onLockChanged(!locked);
        if (cmd == 2 && cb.onHideRequested) cb.onHideRequested();
        if (cmd == 3 && cb.onOpenSettings) cb.onOpenSettings();
        if (cmd == 4 && cb.onOffset) cb.onOffset(-100, false);
        if (cmd == 5 && cb.onOffset) cb.onOffset(100, false);
        if (cmd == 6 && cb.onOffset) cb.onOffset(0, true);
        if (cmd == 7 && cb.onFontSize) cb.onFontSize(kFontStep);
        if (cmd == 8 && cb.onFontSize) cb.onFontSize(-kFontStep);
        if (cmd > 100 && cmd <= 100 + kMaxFontSize && cb.onFontSize && cmd - 100 != size) cb.onFontSize(cmd - 100 - size);
    }

    // ------------------------------------------------------------ locked: unlock button
    void armLockPoll() {
        if (test.offscreen || !locked || !shown) {
            lockTimer.cancel();
            hideUnlock();
            return;
        }
        if (!lockTimer.active()) lockTimer.start(kLockPollMs, [this] { pollLocked(); });
    }

    RECT lyricsScreenRect() const {
        RECT r{};
        GetWindowRect(hwnd, &r);
        r.top += static_cast<LONG>(kToolbarH * scale());
        return r;
    }

    void pollLocked() {
        if (!locked || !shown || !hwnd) return hideUnlock();
        POINT c;
        GetCursorPos(&c);
        RECT lr = lyricsScreenRect();
        RECT br{};
        bool unlockVisible = unlockHwnd && IsWindowVisible(unlockHwnd);
        if (unlockVisible) GetWindowRect(unlockHwnd, &br);
        bool overLyrics = PtInRect(&lr, c) != FALSE;
        bool overButton = unlockVisible && PtInRect(&br, c);
        int64_t t = sched.nowMs();
        if (overLyrics || overButton) {
            unlockLastNear = t;
            if (dwellStart < 0) dwellStart = t;
            if (!unlockVisible && t - dwellStart >= kDwellMs) showUnlock();
        } else {
            dwellStart = -1;
            if (unlockVisible && t - unlockLastNear > kUnlockLingerMs) hideUnlock();
        }
        lockTimer.start(kLockPollMs, [this] { pollLocked(); });
    }

    void drawUnlock(LayeredSurface& s) {
        s.begin();
        auto* rt = s.rt();
        float d = kUnlockDip;
        float c = d / 2 + 2;
        com_ptr<ID2D1SolidColorBrush> shadow, bg, border, fg;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.25f), shadow.put());
        rt->CreateSolidColorBrush(unlockHover ? D2D1::ColorF(0.16f, 0.16f, 0.2f, 0.95f) : D2D1::ColorF(0.08f, 0.08f, 0.1f, 0.88f),
                                  bg.put());
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.22f), border.put());
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.96f), fg.put());
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c + 1.5f), d / 2 + 1, d / 2 + 1), shadow.get());
        auto el = D2D1::Ellipse(D2D1::Point2F(c, c), d / 2, d / 2);
        rt->FillEllipse(el, bg.get());
        rt->DrawEllipse(el, border.get(), 1.f);
        if (auto* e = texts.get(L"\xE785", iconFontFamily(), 15.f, DWRITE_FONT_WEIGHT_NORMAL))
            rt->DrawTextLayout(D2D1::Point2F(c - e->width / 2, c - e->height / 2), e->layout.get(), fg.get());
    }

    SIZE unlockSize() const {
        LONG px = static_cast<LONG>(std::lround((kUnlockDip + 4) * scale()));
        return {px, px};
    }

    void showUnlock() {
        if (test.offscreen || !hwnd) return;
        if (!unlockHwnd) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = UnlockProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kUnlockClass;
            wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
            RegisterClassExW(&wc);
            unlockHwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kUnlockClass,
                                         L"MediaDuet Unlock Lyrics", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance,
                                         nullptr);
            if (!unlockHwnd) return;
            SetWindowLongPtrW(unlockHwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            unlockTip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr,
                                        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, unlockHwnd, nullptr,
                                        wc.hInstance, nullptr);
        }
        SIZE sz = unlockSize();
        if (!unlockSurface.ensure(d2dFactory(), sz, dpi)) return;
        drawUnlock(unlockSurface);
        if (!unlockSurface.end()) return;
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        POINT pt{(wr.left + wr.right - sz.cx) / 2,
                 wr.top + std::max<LONG>(0, static_cast<LONG>(kToolbarH * scale()) - sz.cy) / 2};
        unlockSurface.present(unlockHwnd, &pt, 255);
        if (unlockTip) {
            TTTOOLINFOW ti{sizeof(ti)};
            ti.uFlags = TTF_SUBCLASS;
            ti.hwnd = unlockHwnd;
            ti.uId = 1;
            ti.rect = {0, 0, sz.cx, sz.cy};
            ti.lpszText = const_cast<wchar_t*>(strings.unlockTip.c_str());
            SendMessageW(unlockTip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
            SendMessageW(unlockTip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        }
        ShowWindow(unlockHwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(unlockHwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER);
    }

    void hideUnlock() {
        dwellStart = -1;
        if (unlockHwnd && IsWindowVisible(unlockHwnd)) ShowWindow(unlockHwnd, SW_HIDE);
        unlockHover = false;
    }

    void destroyUnlock() {
        unlockSurface.release();
        if (unlockHwnd) {
            SetWindowLongPtrW(unlockHwnd, GWLP_USERDATA, 0);
            DestroyWindow(unlockHwnd);
            unlockHwnd = nullptr;
            unlockTip = nullptr; // owned: destroyed with it
        }
    }

    static LRESULT CALLBACK UnlockProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self) return DefWindowProcW(h, msg, w, l);
        switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_MOUSEMOVE:
            if (!self->unlockHover) {
                self->unlockHover = true;
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                self->showUnlock();
            }
            return 0;
        case WM_MOUSELEAVE:
            self->unlockHover = false;
            if (IsWindowVisible(h)) self->showUnlock();
            return 0;
        case WM_LBUTTONUP:
            self->hideUnlock();
            if (self->cb.onLockChanged) self->cb.onLockChanged(false);
            return 0;
        default: break;
        }
        return DefWindowProcW(h, msg, w, l);
    }

    // ------------------------------------------------------------ main window proc
    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self) return DefWindowProcW(h, msg, w, l);
        switch (msg) {
        case WM_NCHITTEST: {
            if (self->locked) return HTCLIENT;
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(h, &p);
            float s = self->scale();
            return self->hover && self->buttonAt(p.x / s, p.y / s) >= 0 ? HTCLIENT : HTCAPTION;
        }
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_NCMOUSEMOVE:
        case WM_MOUSEMOVE: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (msg == WM_NCMOUSEMOVE) ScreenToClient(h, &p);
            float s = self->scale();
            int b = self->buttonAt(p.x / s, p.y / s);
            if (!self->hover) {
                self->setHover(true);
                b = self->buttonAt(p.x / s, p.y / s);
            }
            if (b != self->hoverBtn) {
                self->hoverBtn = b;
                if (self->shown) self->render();
            }
            break;
        }
        case WM_LBUTTONUP: {
            if (self->locked) break;
            float s = self->scale();
            int b = self->buttonAt(GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s);
            if (b >= 0) self->onButton(b);
            return 0;
        }
        case WM_EXITSIZEMOVE: {
            RECT r;
            GetWindowRect(h, &r);
            self->posX = r.left;
            self->posY = r.top;
            if (self->cb.onMoved) self->cb.onMoved(r.left, r.top);
            return 0;
        }
        case WM_NCRBUTTONUP:
        case WM_RBUTTONUP: {
            POINT p;
            GetCursorPos(&p);
            self->showMenu(p);
            return 0;
        }
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
            self->placementDirty = true;
            self->surface.release();
            self->texts.clear();
            if (self->shown) self->render();
            return 0;
        default: break;
        }
        return DefWindowProcW(h, msg, w, l);
    }
};

LyricsOverlay::LyricsOverlay(IScheduler& sched, Callbacks cb, TestOptions test)
    : impl_(std::make_unique<Impl>(sched, std::move(cb), std::move(test), this)) {}

LyricsOverlay::~LyricsOverlay() = default;

void LyricsOverlay::configure(const nlohmann::json& o, bool animations, bool lowPower, const Strings& strings) {
    auto& i = *impl_;
    i.fontFamily = text::toWide(o.value("fontFamily", std::string("Microsoft YaHei UI")));
    i.fontSize = static_cast<float>(o.value("fontSize", 32.0));
    i.opacity = static_cast<float>(o.value("opacity", 0.92));
    i.karaoke = o.value("karaoke", true);
    i.twoLines = o.value("twoLines", true);
    i.translation = o.value("translation", true);
    bool wasLocked = i.locked;
    i.locked = o.value("locked", false);
    i.clickThrough = o.value("clickThrough", false);
    i.autoHide = o.value("autoHide", true);
    i.topmost = o.value("topmost", true);
    i.hideInFullscreen = o.value("hideInFullscreen", false);
    i.layout = o.value("layout", std::string("alternate")) == "center" ? lyrics::RowLayout::Center
                                                                       : lyrics::RowLayout::Alternate;
    i.active = parseColor(o.value("colorActive", std::string("#FFFFFFFF")), {1, 1, 1, 1});
    i.inactive = parseColor(o.value("colorInactive", std::string("#B3FFFFFF")), {1, 1, 1, 0.7f});
    i.widthDip = static_cast<float>(o.value("width", 900.0));
    i.posX = o.contains("x") && o["x"].is_number() ? std::optional<int>(o["x"].get<int>()) : std::nullopt;
    i.posY = o.contains("y") && o["y"].is_number() ? std::optional<int>(o["y"].get<int>()) : std::nullopt;
    i.animations = animations;
    i.lowPower = lowPower;
    i.strings = strings;
    i.trSong = i.translation && i.lyr && lyrics::hasTranslation(*i.lyr);
    if (i.locked != wasLocked) {
        i.hover = false;
        i.hideUnlock();
    }
    i.applyStyle();
    i.placementDirty = true;
    i.surface.release();
    i.texts.clear();
    if (enabled_) i.checkFullscreen();
    i.refresh();
}

void LyricsOverlay::setEnabled(bool enabled) {
    enabled_ = enabled;
    if (enabled) impl_->checkFullscreen();
    impl_->refresh();
}

void LyricsOverlay::setLyrics(std::shared_ptr<const lyrics::Lyrics> l, const std::string& trackId) {
    auto& i = *impl_;
    if (trackId == i.trackId && l == i.lyr) return;
    bool rows = i.hasSecondRow();
    float second = i.secondaryScale();
    i.lyr = std::move(l);
    i.lyrRows = i.lyr ? lyrics::buildRows(*i.lyr) : lyrics::LyricRows{};
    i.mainLine = -1;
    i.trSong = i.translation && i.lyr && lyrics::hasTranslation(*i.lyr);
    if (rows != i.hasSecondRow() || second != i.secondaryScale()) i.placementDirty = true; // height changes
    i.trackId = trackId;
    i.karas.clear();
    i.texts.clear();
    for (int r = 0; r < 2; ++r) {
        i.rowNow[r] = i.rowPrev[r] = {};
        i.rowChangedAt[r] = -1e9;
    }
    i.refresh();
}

void LyricsOverlay::setTiming(std::shared_ptr<const lyrics::LyricsClock> clock, int64_t delayMs, int64_t manualOffsetMs,
                              bool canOffset) {
    auto& i = *impl_;
    bool playing = clock && clock->valid() && clock->playing();
    if (i.playing && !playing) i.pausedAt = i.sched.nowMs();
    i.clock = std::move(clock);
    i.delayMs = delayMs;
    i.manualOffset = manualOffsetMs;
    i.canOffset = canOffset;
    i.playing = playing;
    i.hideTimer.cancel();
    if (!playing && i.autoHide && !i.test.offscreen) i.hideTimer.start(kAutoHideMs + 50, [this] { impl_->refresh(); });
    i.refresh();
}

bool LyricsOverlay::windowVisible() const { return impl_->shown; }

bool LyricsOverlay::snapshot(const std::wstring& path) const { return impl_->surface.savePng(path); }

bool LyricsOverlay::renderOffscreen(bool hover, std::vector<uint8_t>& bgra, SIZE& size) {
    auto& i = *impl_;
    if (!i.lyr) return false;
    i.hover = hover;
    i.updateRows();
    POINT pt;
    i.computePlacement(pt, size);
    if (!i.ensureResources(size)) return false;
    i.draw(size);
    if (!i.surface.end()) return false;
    return i.surface.copyPixels(bgra);
}

bool LyricsOverlay::renderUnlockButtonOffscreen(std::vector<uint8_t>& bgra, SIZE& size) {
    auto& i = *impl_;
    size = i.unlockSize();
    LayeredSurface s;
    if (!s.ensure(d2dFactory(), size, i.dpi)) return false;
    i.drawUnlock(s);
    if (!s.end()) return false;
    return s.copyPixels(bgra);
}

} // namespace md::platform
