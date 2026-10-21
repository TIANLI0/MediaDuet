#include "ResumeButton.h"

#include "resources/resource.h"

#include "md/util/Log.h"

#include <objbase.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace md::app {

namespace {

constexpr wchar_t kClass[] = L"MediaDuetResume";
constexpr wchar_t kAumid[] = L"MediaDuet.Resume";
constexpr ULONGLONG kClickDebounceMs = 800;

ULONGLONG g_lastClick = 0;

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void destroyIcon(HICON& h) {
    if (h) DestroyIcon(h);
    h = nullptr;
}

// 32 bpp top-down DIB section; `bits` receives its pixels (BGRA).
HBITMAP makeDib(int w, int h, uint32_t** bits) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* p = nullptr;
    HBITMAP b = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    *bits = static_cast<uint32_t*>(p);
    return b;
}

COLORREF accentColor() {
    DWORD argb = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque)) && argb)
        return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
    return RGB(0x00, 0x78, 0xD4);
}

// Anti-aliased coverage (0..1) of an icon-font glyph centred in a size x size square.
std::vector<double> glyphCoverage(wchar_t glyph, int size, double em) {
    std::vector<double> cov(static_cast<size_t>(size) * size, 0.0);
    uint32_t* bits = nullptr;
    HBITMAP bmp = makeDib(size, size, &bits);
    if (!bmp) return cov;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    std::fill(bits, bits + size * size, 0u);
    HFONT font = nullptr;
    for (const wchar_t* face : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
        font = CreateFontW(-std::lround(size * em), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
        wchar_t got[LF_FACESIZE] = {};
        HGDIOBJ prev = SelectObject(dc, font);
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, prev);
        if (_wcsicmp(got, face) == 0) break;
        DeleteObject(font);
        font = nullptr;
    }
    if (font) {
        HGDIOBJ prev = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT r{0, 0, size, size};
        DrawTextW(dc, &glyph, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, prev);
        DeleteObject(font);
    }
    GdiFlush();
    for (size_t i = 0; i < cov.size(); ++i) cov[i] = ((bits[i] >> 8) & 0xFF) / 255.0; // grey AA text: any channel
    SelectObject(dc, oldBmp);
    DeleteDC(dc);
    DeleteObject(bmp);
    return cov;
}

// An icon from straight-alpha BGRA pixels.
HICON iconFromPixels(int size, const std::vector<uint32_t>& px) {
    uint32_t* bits = nullptr;
    HBITMAP color = makeDib(size, size, &bits);
    if (!color) return nullptr;
    std::copy(px.begin(), px.end(), bits);
    std::vector<uint8_t> zeros(((size + 15) / 16) * 2 * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros.data());
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

uint32_t argb(double a, double r, double g, double b) {
    auto c = [](double v) { return static_cast<uint32_t>(std::clamp(v, 0.0, 255.0) + 0.5); };
    return (c(a * 255.0) << 24) | (c(r) << 16) | (c(g) << 8) | c(b);
}

// The overlay badge: the phone glyph (as on MediaDuet's handoff button) on an accent circle.
HICON makeBadge() {
    int size = GetSystemMetrics(SM_CXSMICON);
    auto glyph = glyphCoverage(L'\xE8EA', size, 0.72);
    COLORREF accent = accentColor();
    const double cr = GetRValue(accent), cg = GetGValue(accent), cb = GetBValue(accent);
    const double radius = size / 2.0, cx = size / 2.0, cy = size / 2.0;
    std::vector<uint32_t> out(static_cast<size_t>(size) * size, 0);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int inside = 0; // 4x4 supersampled circle coverage
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    double dx = x + (sx + 0.5) / 4 - cx, dy = y + (sy + 0.5) / 4 - cy;
                    inside += dx * dx + dy * dy <= radius * radius;
                }
            double c = inside / 16.0;
            double g = glyph[y * size + x] * c;
            double a = g + c * (1 - g);
            if (a > 0)
                out[y * size + x] = argb(a, (255.0 * g + cr * c * (1 - g)) / a, (255.0 * g + cg * c * (1 - g)) / a,
                                         (255.0 * g + cb * c * (1 - g)) / a);
        }
    return iconFromPixels(size, out);
}

// A thumbnail toolbar glyph in the taskbar's text colour.
HICON makeGlyphIcon(wchar_t glyph, bool light) {
    int size = GetSystemMetrics(SM_CXSMICON);
    auto cov = glyphCoverage(glyph, size, 0.8);
    double v = light ? 0.0 : 255.0;
    std::vector<uint32_t> out(cov.size(), 0);
    for (size_t i = 0; i < cov.size(); ++i)
        if (cov[i] > 0) out[i] = argb(cov[i], v, v, v);
    return iconFromPixels(size, out);
}

// The taskbar (and its thumbnail flyouts) follows the system theme, not the apps' one.
bool systemLightTheme() {
    DWORD v = 0, size = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS &&
           v != 0;
}

// Premultiplied BGRA of an icon at its own size (for the thumbnail without a cover).
bool iconPixels(HICON icon, int& w, int& h, std::vector<uint32_t>& px) {
    ICONINFO ii{};
    if (!icon || !GetIconInfo(icon, &ii)) return false;
    BITMAP bm{};
    bool ok = false;
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof bm, &bm) && bm.bmBitsPixel == 32) {
        w = bm.bmWidth;
        h = bm.bmHeight;
        px.assign(static_cast<size_t>(w) * h, 0);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, ii.hbmColor, 0, h, px.data(), &bi, DIB_RGB_COLORS) == h;
        ReleaseDC(nullptr, dc);
        for (auto& p : px) {
            uint32_t a = p >> 24;
            auto pm = [&](int shift) { return (((p >> shift) & 0xFF) * a / 255) << shift; };
            p = (a << 24) | pm(16) | pm(8) | pm(0);
        }
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return ok;
}

// The cover scaled to side x side, premultiplied BGRA (WIC).
bool coverPixels(const std::filesystem::path& file, int side, std::vector<uint32_t>& px) {
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
              SUCCEEDED(f->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateBitmapScaler(&scaler)) &&
              SUCCEEDED(scaler->Initialize(frame, side, side, WICBitmapInterpolationModeHighQualityCubic)) &&
              SUCCEEDED(f->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeCustom));
    if (ok) {
        px.assign(static_cast<size_t>(side) * side, 0);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, side * 4, static_cast<UINT>(px.size() * 4), reinterpret_cast<BYTE*>(px.data())));
    }
    release(conv);
    release(scaler);
    release(frame);
    release(dec);
    release(f);
    return ok;
}

} // namespace

ResumeButton::ResumeButton(Callbacks cb) : cb_(std::move(cb)) {}

ResumeButton::~ResumeButton() {
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
    release(taskbar_);
    destroyIcon(big_);
    destroyIcon(small_);
    destroyIcon(badge_);
    for (auto& i : thumbIcons_) destroyIcon(i);
}

void ResumeButton::ensureWindow() {
    if (hwnd_) return;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    buttonCreated_ = RegisterWindowMessageW(L"TaskbarButtonCreated");
    // Off screen and only ever minimized: the window exists for its taskbar button.
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kClass, L"", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            -32000, -32000, 320, 200, nullptr, nullptr, inst, this);
    if (!hwnd_) return;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // Its own taskbar group (never with MediaDuet's windows), not pinnable.
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd_, IID_PPV_ARGS(&store)))) {
        PROPVARIANT pv;
        if (SUCCEEDED(InitPropVariantFromString(kAumid, &pv))) {
            store->SetValue(PKEY_AppUserModel_ID, pv);
            PropVariantClear(&pv);
        }
        if (SUCCEEDED(InitPropVariantFromBoolean(TRUE, &pv))) {
            store->SetValue(PKEY_AppUserModel_PreventPinning, pv);
            PropVariantClear(&pv);
        }
        store->Commit();
        store->Release();
    }
    // The thumbnail is drawn by us (cover art); no peek at an off-screen window.
    BOOL on = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_FORCE_ICONIC_REPRESENTATION, &on, sizeof on);
    DwmSetWindowAttribute(hwnd_, DWMWA_HAS_ICONIC_BITMAP, &on, sizeof on);
    DwmSetWindowAttribute(hwnd_, DWMWA_DISALLOW_PEEK, &on, sizeof on);
}

void ResumeButton::show(const Offer& offer) {
    ensureWindow();
    if (!hwnd_) return;
    bool changed = offer.id != offer_.id || offer.iconFile != offer_.iconFile || offer.artwork != offer_.artwork ||
                   offer.badgeTip != offer_.badgeTip;
    bool retitled = offer.title != offer_.title;
    bool controls = !(offer.controls == offer_.controls);
    offer_ = offer;
    if (controls) applyThumbBar();
    if (changed || !big_) applyIcons();
    if (retitled) SetWindowTextW(hwnd_, offer_.title.c_str());
    if (changed) DwmInvalidateIconicBitmaps(hwnd_);
    if (!visible_) {
        visible_ = true;
        ShowWindow(hwnd_, SW_SHOWMINNOACTIVE);
    }
}

void ResumeButton::hide() {
    if (!visible_) return;
    visible_ = false;
    buttonReady_ = false; // the next show creates a new button
    thumbAdded_ = false;
    state_ = State::Idle;
    ShowWindow(hwnd_, SW_HIDE);
}

void ResumeButton::setState(State s) {
    if (state_ == s) return;
    state_ = s;
    applyState();
}

void ResumeButton::applyIcons() {
    destroyIcon(big_);
    destroyIcon(small_);
    int bigSize = GetSystemMetrics(SM_CXICON), smallSize = GetSystemMetrics(SM_CXSMICON);
    if (!offer_.iconFile.empty())
        SHDefExtractIconW(offer_.iconFile.c_str(), 0, 0, &big_, &small_, MAKELONG(bigSize, smallSize));
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!big_) big_ = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, bigSize, bigSize, 0));
    if (!small_) small_ = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, smallSize, smallSize, 0));
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_));
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_));
    if (!badge_) badge_ = makeBadge();
    if (buttonReady_ && taskbar_) taskbar_->SetOverlayIcon(hwnd_, badge_, offer_.badgeTip.c_str());
}

void ResumeButton::applyState() {
    if (!buttonReady_ || !taskbar_) return;
    switch (state_) {
    case State::Idle: taskbar_->SetProgressState(hwnd_, TBPF_NOPROGRESS); break;
    case State::Working: taskbar_->SetProgressState(hwnd_, TBPF_INDETERMINATE); break;
    case State::Failed:
        taskbar_->SetProgressState(hwnd_, TBPF_ERROR);
        taskbar_->SetProgressValue(hwnd_, 100, 100);
        break;
    }
}

void ResumeButton::applyThumbBar() {
    if (!buttonReady_ || !taskbar_) return;
    bool light = systemLightTheme();
    if (!thumbIcons_[0] || light != thumbLight_) {
        for (auto& i : thumbIcons_) destroyIcon(i);
        thumbLight_ = light;
        const wchar_t glyphs[4] = {L'\xE892', L'\xE768', L'\xE769', L'\xE893'}; // previous, play, pause, next
        for (int i = 0; i < 4; ++i) thumbIcons_[i] = makeGlyphIcon(glyphs[i], light);
    }
    const auto& c = offer_.controls;
    THUMBBUTTON b[3] = {};
    auto set = [](THUMBBUTTON& t, UINT id, HICON icon, const std::wstring& tip, bool enabled) {
        t.dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
        t.iId = id;
        t.hIcon = icon;
        wcsncpy_s(t.szTip, tip.c_str(), _TRUNCATE);
        t.dwFlags = enabled ? THBF_ENABLED : THBF_DISABLED;
    };
    set(b[0], 1, thumbIcons_[0], c.previousTip, c.previous);
    set(b[1], 2, c.playing ? thumbIcons_[2] : thumbIcons_[1], c.playing ? c.pauseTip : c.playTip, c.playPause);
    set(b[2], 3, thumbIcons_[3], c.nextTip, c.next);
    if (!thumbAdded_) thumbAdded_ = SUCCEEDED(taskbar_->ThumbBarAddButtons(hwnd_, 3, b));
    else taskbar_->ThumbBarUpdateButtons(hwnd_, 3, b);
}

void ResumeButton::sendThumbnail(int maxWidth, int maxHeight) {
    int side = std::min(maxWidth, maxHeight);
    if (side <= 0) return;
    std::vector<uint32_t> px;
    int w = side, h = side;
    if (offer_.artwork.empty() || !coverPixels(offer_.artwork, side, px)) {
        // No cover: the app's icon, as large as Windows has it, centred.
        HICON large = nullptr;
        if (!offer_.iconFile.empty()) SHDefExtractIconW(offer_.iconFile.c_str(), 0, 0, &large, nullptr, MAKELONG(256, 0));
        int iw = 0, ih = 0;
        std::vector<uint32_t> ip;
        bool got = iconPixels(large ? large : big_, iw, ih, ip);
        destroyIcon(large);
        if (!got) return;
        w = std::min(iw, maxWidth);
        h = std::min(ih, maxHeight);
        px.assign(static_cast<size_t>(w) * h, 0);
        for (int y = 0; y < h; ++y) std::copy_n(ip.begin() + static_cast<size_t>(y) * iw, w, px.begin() + static_cast<size_t>(y) * w);
    }
    uint32_t* bits = nullptr;
    HBITMAP bmp = makeDib(w, h, &bits);
    if (!bmp) return;
    std::copy(px.begin(), px.end(), bits);
    DwmSetIconicThumbnail(hwnd_, bmp, 0);
    DeleteObject(bmp);
}

LRESULT CALLBACK ResumeButton::WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<ResumeButton*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, msg, w, l);
    return self->handle(msg, w, l);
}

LRESULT ResumeButton::handle(UINT msg, WPARAM w, LPARAM l) {
    auto click = [this] {
        ULONGLONG now = GetTickCount64();
        if (now - g_lastClick < kClickDebounceMs) return;
        g_lastClick = now;
        if (cb_.clicked) cb_.clicked();
    };
    if (msg == buttonCreated_ && buttonCreated_) {
        if (!taskbar_ && SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar_))) &&
            FAILED(taskbar_->HrInit()))
            release(taskbar_);
        buttonReady_ = taskbar_ != nullptr;
        if (buttonReady_) {
            taskbar_->SetOverlayIcon(hwnd_, badge_, offer_.badgeTip.c_str());
            applyState();
            thumbAdded_ = false; // a new button has no toolbar yet
            applyThumbBar();
        }
        return 0;
    }
    switch (msg) {
    case WM_QUERYOPEN: // the taskbar button (or Alt+Tab) restores the window: continue instead
        click();
        return FALSE;
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_RESTORE || (w & 0xFFF0) == SC_MAXIMIZE) {
            click();
            return 0;
        }
        break;
    case WM_SIZE:
        if (w != SIZE_MINIMIZED && visible_) ShowWindow(hwnd_, SW_SHOWMINNOACTIVE); // never shown restored
        return 0;
    case WM_CLOSE:
        hide();
        if (cb_.closed) cb_.closed();
        return 0;
    case WM_DWMSENDICONICTHUMBNAIL: sendThumbnail(HIWORD(l), LOWORD(l)); return 0;
    case WM_COMMAND:
        if (HIWORD(w) == THBN_CLICKED) {
            static constexpr Control kControls[] = {Control::Previous, Control::PlayPause, Control::Next};
            UINT id = LOWORD(w);
            if (id >= 1 && id <= 3 && cb_.control) cb_.control(kControls[id - 1]);
            return 0;
        }
        break;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        if (systemLightTheme() != thumbLight_) applyThumbBar(); // light / dark taskbar: redraw the glyphs
        break;
    case WM_DWMSENDICONICLIVEPREVIEWBITMAP: return 0;
    }
    return DefWindowProcW(hwnd_, msg, w, l);
}

} // namespace md::app
