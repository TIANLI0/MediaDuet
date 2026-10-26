// Developer tool: renders the real desktop-lyrics overlay and taskbar widget
// offscreen into PNG frame sequences. No window is created or shown: both
// surfaces run in their offscreen test mode on a virtual clock, and each frame is
// composited onto a few synthetic backgrounds (checker, dark "wallpaper", light
// document, taskbar colours) so legibility can be checked. Not shipped.
//
//   MediaDuetOverlayDemo.exe [out-dir] [dpi]
// Writes <out-dir>\<scenario>_<background>_<ms>.png (default out-dir:
// overlay_frames next to the executable, dpi 144).

#include "overlay/LyricsOverlay.h"
#include "overlay/OverlayCommon.h"
#include "overlay/TaskbarWidget.h"

#include "md/lyrics/Lrc.h"
#include "md/lyrics/LyricsClock.h"
#include "md/util/ManualScheduler.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using md::platform::LyricsOverlay;
using md::platform::TaskbarWidget;

double g_now = 0; // virtual steady clock (ms)

// ------------------------------------------------------------------ backgrounds
enum class Bg { Checker, Dark, Light, TaskbarDark, TaskbarLight };
const wchar_t* bgName(Bg b) {
    switch (b) {
    case Bg::Checker: return L"checker";
    case Bg::Dark: return L"dark";
    case Bg::Light: return L"light";
    case Bg::TaskbarDark: return L"tbdark";
    default: return L"tblight";
    }
}

void background(Bg b, int w, int h, std::vector<uint8_t>& px) {
    px.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float r, g, bl;
            float u = static_cast<float>(x) / w, v = static_cast<float>(y) / h;
            switch (b) {
            case Bg::Checker: {
                bool c = ((x / 12) + (y / 12)) % 2;
                r = g = bl = c ? 0.80f : 0.62f;
                break;
            }
            case Bg::Dark: {
                // Wallpaper-like: deep blue to warm orange with soft blobs and a bright band.
                r = 0.08f + 0.55f * u * u + 0.25f * std::exp(-((u - 0.7f) * (u - 0.7f) + (v - 0.4f) * (v - 0.4f)) * 18);
                g = 0.10f + 0.25f * u + 0.35f * std::exp(-((u - 0.25f) * (u - 0.25f) + (v - 0.6f) * (v - 0.6f)) * 14);
                bl = 0.25f + 0.35f * (1 - u) * v + 0.4f * std::exp(-((u - 0.45f) * (u - 0.45f)) * 40) * 0.6f;
                break;
            }
            case Bg::Light: {
                // A white document with grey text lines.
                bool line = (y % 18) < 3 && x % 260 < 230 && x > 20;
                r = g = bl = line ? 0.55f : 0.97f;
                break;
            }
            case Bg::TaskbarDark: r = g = bl = 0.13f; break;
            default: r = 0.93f, g = 0.94f, bl = 0.96f; break;
            }
            auto* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>(std::clamp(bl, 0.f, 1.f) * 255);
            p[1] = static_cast<uint8_t>(std::clamp(g, 0.f, 1.f) * 255);
            p[2] = static_cast<uint8_t>(std::clamp(r, 0.f, 1.f) * 255);
            p[3] = 255;
        }
}

// Premultiplied src over opaque dst, with a global window opacity.
void compose(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src, float opacity) {
    for (size_t i = 0; i + 3 < src.size() && i + 3 < dst.size(); i += 4) {
        float a = src[i + 3] / 255.f * opacity;
        for (int c = 0; c < 3; ++c) {
            float v = src[i + c] / 255.f * opacity + dst[i + c] / 255.f * (1 - a);
            dst[i + c] = static_cast<uint8_t>(std::clamp(v, 0.f, 1.f) * 255 + 0.5f);
        }
        dst[i + 3] = 255;
    }
}

bool save(const std::wstring& dir, const std::wstring& name, Bg bg, const std::vector<uint8_t>& frame, SIZE sz,
          float opacity = 1.f) {
    std::vector<uint8_t> px;
    background(bg, sz.cx, sz.cy, px);
    compose(px, frame, opacity);
    std::wstring path = dir + L"\\" + name + L"_" + bgName(bg) + L".png";
    bool ok = md::platform::writePng(path, px.data(), sz.cx, sz.cy);
    wprintf(L"%s %s\n", ok ? L"wrote" : L"FAILED", path.c_str());
    return ok;
}

// ------------------------------------------------------------------ sample lyrics
// Showcase mode (website / README screenshots): lyrics and translation read from LRC files.
std::string g_showcaseLrc, g_showcaseTr;

std::string readFile(const std::wstring& path) {
    std::string out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

std::shared_ptr<md::lyrics::Lyrics> sampleLyrics(bool withTranslation) {
    if (!g_showcaseLrc.empty()) {
        auto parsed = md::lyrics::parseLrc(g_showcaseLrc);
        if (withTranslation && !g_showcaseTr.empty()) md::lyrics::mergeTranslation(parsed, md::lyrics::parseLrc(g_showcaseTr));
        return std::make_shared<md::lyrics::Lyrics>(std::move(parsed));
    }
    const char* lrc =
        "[00:00.00]<00:00.00>Sample <00:00.80>word-timed <00:02.00>karaoke <00:03.20>line<00:04.50>\n"
        // 逐行歌词也会平滑地填充
        "[00:05.00]\xE9\x80\x90\xE8\xA1\x8C\xE6\xAD\x8C\xE8\xAF\x8D\xE4\xB9\x9F\xE4\xBC\x9A\xE5\xB9\xB3\xE6\xBB\x91\xE5"
        "\x9C\xB0\xE5\xA1\xAB\xE5\x85\x85\n"
        "[00:10.00]This is a deliberately very long English lyric line that cannot possibly fit into the desktop lyrics "
        "window at this size\n"
        // 这是一句非常非常长的中文歌词，用来测试桌面歌词在窗口宽度不够时会不会平滑地横向滚动而不是被省略号截断
        "[00:18.00]\xE8\xBF\x99\xE6\x98\xAF\xE4\xB8\x80\xE5\x8F\xA5\xE9\x9D\x9E\xE5\xB8\xB8\xE9\x9D\x9E\xE5\xB8\xB8\xE9"
        "\x95\xBF\xE7\x9A\x84\xE4\xB8\xAD\xE6\x96\x87\xE6\xAD\x8C\xE8\xAF\x8D\xEF\xBC\x8C\xE7\x94\xA8\xE6\x9D\xA5\xE6"
        "\xB5\x8B\xE8\xAF\x95\xE6\xA1\x8C\xE9\x9D\xA2\xE6\xAD\x8C\xE8\xAF\x8D\xE5\x9C\xA8\xE7\xAA\x97\xE5\x8F\xA3\xE5"
        "\xAE\xBD\xE5\xBA\xA6\xE4\xB8\x8D\xE5\xA4\x9F\xE6\x97\xB6\xE4\xBC\x9A\xE4\xB8\x8D\xE4\xBC\x9A\xE5\xB9\xB3\xE6"
        "\xBB\x91\xE5\x9C\xB0\xE6\xA8\xAA\xE5\x90\x91\xE6\xBB\x9A\xE5\x8A\xA8\xE8\x80\x8C\xE4\xB8\x8D\xE6\x98\xAF\xE8"
        "\xA2\xAB\xE7\x9C\x81\xE7\x95\xA5\xE5\x8F\xB7\xE6\x88\xAA\xE6\x96\xAD\n"
        "[00:26.00]<00:26.00>  Multiple   <00:26.60>spaces <00:27.20>\xF0\x9F\x8E\xB5 <00:27.80>end  <00:28.60>\n"
        "[00:30.00]\n";
    auto parsed = md::lyrics::parseLrc(lrc);
    if (withTranslation)
        md::lyrics::mergeTranslation(
            parsed, md::lyrics::parseLrc(
                        // 示例逐字卡拉OK歌词 / Line-by-line lyrics fill smoothly too / 这是一句故意写得很长的英文歌词
                        "[00:00.00]\xE7\xA4\xBA\xE4\xBE\x8B\xE9\x80\x90\xE5\xAD\x97\xE5\x8D\xA1\xE6\x8B\x89OK\xE6\xAD\x8C"
                        "\xE8\xAF\x8D\n"
                        "[00:05.00]Line-by-line lyrics fill smoothly too\n"
                        "[00:10.00]\xE8\xBF\x99\xE6\x98\xAF\xE4\xB8\x80\xE5\x8F\xA5\xE6\x95\x85\xE6\x84\x8F\xE5\x86\x99"
                        "\xE5\xBE\x97\xE5\xBE\x88\xE9\x95\xBF\xE7\x9A\x84\xE8\x8B\xB1\xE6\x96\x87\xE6\xAD\x8C\xE8\xAF\x8D\n"
                        "[00:18.00]A very, very long Chinese lyric line that checks whether long lines scroll smoothly "
                        "instead of being cut off\n"));
    return std::make_shared<md::lyrics::Lyrics>(std::move(parsed));
}

LyricsOverlay::Strings strings() {
    LyricsOverlay::Strings s;
    s.lock = L"Lock";
    s.unlock = L"Unlock";
    s.hide = L"Hide";
    s.settings = L"Settings";
    s.earlier = L"Lyrics earlier 0.1 s";
    s.later = L"Lyrics later 0.1 s";
    s.resetOffset = L"Reset offset (now %s)";
    s.fontSize = L"Text size (now %s)";
    s.unlockTip = L"Unlock desktop lyrics";
    return s;
}

std::shared_ptr<md::lyrics::LyricsClock> playingClock() {
    auto c = std::make_shared<md::lyrics::LyricsClock>();
    md::lyrics::LyricsClock::Report r;
    r.positionMs = 0;
    r.atMs = 0;
    r.playing = true;
    r.trackId = "demo";
    c->update(r, 0);
    return c;
}

struct OverlayScenario {
    std::wstring name;
    nlohmann::json cfg;
    bool translation = false;
    std::vector<int> times;
    std::vector<Bg> bgs{Bg::Dark};
    bool hover = false;
};

void runOverlay(md::ManualScheduler& sched, const std::wstring& dir, UINT dpi, const OverlayScenario& sc) {
    LyricsOverlay o(sched, {}, {true, [] { return g_now; }, dpi});
    nlohmann::json cfg = {{"fontSize", 32}, {"opacity", 0.95}, {"karaoke", true}, {"twoLines", true},
                          {"autoHide", false}, {"translation", sc.translation}, {"width", 900}};
    for (auto& [k, v] : sc.cfg.items()) cfg[k] = v;
    g_now = 0;
    o.configure(cfg, true, false, strings());
    o.setEnabled(true);
    o.setLyrics(sampleLyrics(sc.translation), "demo");
    o.setTiming(playingClock(), 0, 300, true);
    std::vector<uint8_t> px;
    SIZE sz{};
    // Walk the clock in 16 ms frames so row changes and cross-fades happen as live.
    for (int t : sc.times) {
        while (g_now + 16 < t) {
            g_now += 16;
            o.renderOffscreen(sc.hover, px, sz);
        }
        g_now = t;
        if (!o.renderOffscreen(sc.hover, px, sz)) continue;
        wchar_t name[96];
        swprintf_s(name, L"%s_%05d", sc.name.c_str(), t);
        for (Bg b : sc.bgs) save(dir, name, b, px, sz, 0.95f);
    }
}

void runTaskbar(md::ManualScheduler& sched, const std::wstring& dir, UINT dpi, bool light,
                std::vector<int> times = {1400, 7000, 11000, 15000, 21000, 27000}, const char* align = "left") {
    TaskbarWidget tb(sched, {}, {true, [] { return g_now; }, dpi, light});
    tb.configure({{"enabled", true}, {"width", 360}, {"fontSize", 13}, {"translation", false}, {"align", align}}, true, false);
    bool right = std::string(align) == "right";
    md::media::MediaSessionInfo s;
    s.title = g_showcaseLrc.empty() ? "Sample Track" : "\xE6\x99\x9A\xE9\xA3\x8E\xE6\x9D\xA5\xE4\xBF\xA1"; // 晚风来信
    s.artist = g_showcaseLrc.empty() ? "MediaDuet Demo" : "\xE6\x9E\x97\xE5\xB2\xB8";                          // 林岸
    s.trackId = "demo";
    s.status = md::media::PlaybackStatus::Playing;
    tb.setTrack(&s);
    tb.setLyrics(sampleLyrics(false), "demo");
    tb.setTiming(playingClock(), 0);
    float k = dpi / 96.f;
    int w = static_cast<int>(360 * k), h = static_cast<int>(48 * k);
    std::vector<uint8_t> px;
    for (int t : times) {
        for (bool hover : {false, true}) {
            if (hover && t != 7000) continue;
            g_now = t;
            if (!tb.renderOffscreen(w, h, hover, px)) continue;
            wchar_t name[96];
            swprintf_s(name, L"taskbar%s%s_%05d%s", light ? L"_light" : L"", right ? L"_right" : L"", t, hover ? L"_hover" : L"");
            save(dir, name, light ? Bg::TaskbarLight : Bg::TaskbarDark, px, {w, h});
        }
    }
}

// Every 16 ms frame from `from` to `to` (as live: line changes roll), saving the listed times.
void runTaskbarFrames(md::ManualScheduler& sched, const std::wstring& dir, UINT dpi, const wchar_t* name, bool translation,
                      int from, int to, std::vector<int> save_) {
    TaskbarWidget tb(sched, {}, {true, [] { return g_now; }, dpi, false});
    tb.configure({{"enabled", true}, {"width", 360}, {"fontSize", 13}, {"translation", translation}, {"align", "left"}}, true, false);
    md::media::MediaSessionInfo s;
    s.title = "Sample Track";
    s.artist = "MediaDuet Demo";
    s.trackId = "demo";
    s.status = md::media::PlaybackStatus::Playing;
    tb.setTrack(&s);
    tb.setLyrics(sampleLyrics(translation), "demo");
    tb.setTiming(playingClock(), 0);
    float k = dpi / 96.f;
    int w = static_cast<int>(360 * k), h = static_cast<int>(48 * k);
    std::vector<uint8_t> px;
    for (g_now = from; g_now <= to; g_now += 16) {
        bool keep = std::find_if(save_.begin(), save_.end(), [](int t) { return t >= g_now && t < g_now + 16; }) != save_.end();
        if (!tb.renderOffscreen(w, h, false, px) || !keep) continue;
        wchar_t file[96];
        swprintf_s(file, L"%s_%05d", name, static_cast<int>(g_now));
        save(dir, file, Bg::TaskbarDark, px, {w, h});
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring dir;
    if (argc > 1) {
        dir = argv[1];
    } else {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        dir = exe;
        dir = dir.substr(0, dir.find_last_of(L"\\/")) + L"\\overlay_frames";
    }
    UINT dpi = argc > 2 ? static_cast<UINT>(_wtoi(argv[2])) : 144;
    CreateDirectoryW(dir.c_str(), nullptr);
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        _wfreopen_s(&f, L"CONOUT$", L"w", stdout);
    }
    md::ManualScheduler sched; // timers never fire: nothing runs behind our back

    // MediaDuetOverlayDemo <dir> <dpi> showcase <lyrics.lrc> [translation.lrc]: website screenshots.
    if (argc > 4 && std::wstring(argv[3]) == L"showcase") {
        g_showcaseLrc = readFile(argv[4]);
        if (argc > 5) g_showcaseTr = readFile(argv[5]);
        std::vector<OverlayScenario> show = {
            {L"showcase_center", {{"layout", "center"}}, true, {10200, 28000, 37000}, {Bg::Dark, Bg::Light}},
            {L"showcase_alternate", {{"layout", "alternate"}}, false, {10200, 28000}, {Bg::Dark}},
        };
        for (auto& sc : show) runOverlay(sched, dir, dpi, sc);
        runTaskbar(sched, dir, dpi, false, {10200, 28000});
        runTaskbar(sched, dir, dpi, true, {10200, 28000});
        return 0;
    }

    std::vector<OverlayScenario> scenarios = {
        {L"alternate", {{"layout", "alternate"}}, false,
         {1400, 4600, 5040, 5120, 5300, 7000, 11000, 14500, 17000, 19000, 22500, 25500, 27500},
         {Bg::Dark, Bg::Light, Bg::Checker}},
        {L"center", {{"layout", "center"}}, false, {1400, 7000, 14500, 22500}, {Bg::Dark, Bg::Light}},
        {L"translation", {{"layout", "alternate"}}, true, {1400, 7000, 12000}, {Bg::Dark, Bg::Light}},
        {L"hover", {{"layout", "alternate"}}, false, {7000}, {Bg::Dark, Bg::Light}, true},
        {L"colors", {{"colorActive", "#FF7FDBFF"}, {"colorInactive", "#E6FFFFFF"}}, false, {1400, 7000}, {Bg::Dark, Bg::Light}},
        {L"small", {{"fontSize", 22}, {"width", 520}}, false, {1400, 14500, 22500}, {Bg::Dark}},
    };
    for (auto& sc : scenarios) runOverlay(sched, dir, dpi, sc);

    {
        LyricsOverlay o(sched, {}, {true, [] { return g_now; }, dpi});
        o.configure({{"locked", true}}, true, false, strings());
        std::vector<uint8_t> px;
        SIZE sz{};
        if (o.renderUnlockButtonOffscreen(px, sz)) {
            save(dir, L"unlock_button", Bg::Dark, px, sz);
            save(dir, L"unlock_button", Bg::Light, px, sz);
        }
    }
    runTaskbar(sched, dir, dpi, false);
    runTaskbar(sched, dir, dpi, true);
    runTaskbar(sched, dir, dpi, false, {7000, 15000}, "right");
    // Line change at 5 s: the rows roll up; a long translation scrolls with its line (18 s).
    runTaskbarFrames(sched, dir, dpi, L"taskbar_roll", false, 4000, 5600, {4800, 5056, 5120, 5200, 5280, 5600});
    runTaskbarFrames(sched, dir, dpi, L"taskbar_tr", true, 17000, 26000, {17200, 19000, 21000, 23000, 25000});
    return 0;
}
