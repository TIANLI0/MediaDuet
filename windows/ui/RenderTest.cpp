#include "pch.h"
#include "RenderTest.h"

#include "Controller.h"
#include "Karaoke.h"
#include "MainWindow.xaml.h"
#include "WindowUtil.h"

#include <dbghelp.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include <chrono>
#include <exception>
#include <cstdio>
#include <fstream>
#include <vector>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
using namespace std::chrono_literals;

namespace mdui::render
{
    namespace
    {
        bool g_active = false;
        std::wstring g_outDir;
        HWND g_foreground = nullptr;
        std::vector<HWND> g_windows;
        std::wstring g_only;   // run only this section (empty = all)

        bool Section(wchar_t const* name)
        {
            return g_only.empty() || g_only == name;
        }

        void Log(std::wstring const& line)
        {
            std::wofstream out(g_outDir + L"\\render-test.log", std::ios::app);
            out << line << L"\n";
        }

        // Crashes during a render run: the faulting thread's stack, symbolized from the PDB next to the exe.
        LONG CALLBACK LogCrash(EXCEPTION_POINTERS* info)
        {
            if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
            static bool once = false;
            if (once) return EXCEPTION_CONTINUE_SEARCH;
            once = true;
            using SymInit = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
            using SymAddr = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, void*);
            using SymLine = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD, void*);
            HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
            auto init = dbg ? reinterpret_cast<SymInit>(GetProcAddress(dbg, "SymInitialize")) : nullptr;
            auto fromAddr = dbg ? reinterpret_cast<SymAddr>(GetProcAddress(dbg, "SymFromAddr")) : nullptr;
            auto lineFromAddr = dbg ? reinterpret_cast<SymLine>(GetProcAddress(dbg, "SymGetLineFromAddr64")) : nullptr;
            if (init) init(GetCurrentProcess(), nullptr, TRUE);
            void* frames[48];
            USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
            wchar_t head[128];
            swprintf_s(head, L"CRASH access violation at %p (%s %p)", info->ExceptionRecord->ExceptionAddress,
                       info->ExceptionRecord->ExceptionInformation[0] ? L"write" : L"read",
                       reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
            Log(head);
            for (USHORT i = 0; i < n; ++i) {
                auto addr = reinterpret_cast<DWORD64>(frames[i]);
                alignas(8) char buf[sizeof(SYMBOL_INFO) + 256]{};
                auto sym = reinterpret_cast<SYMBOL_INFO*>(buf);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                DWORD64 disp = 0;
                IMAGEHLP_LINE64 line{ sizeof(line) };
                DWORD lineDisp = 0;
                std::string text = (fromAddr && fromAddr(GetCurrentProcess(), addr, &disp, sym)) ? sym->Name : "?";
                if (lineFromAddr && lineFromAddr(GetCurrentProcess(), addr, &lineDisp, &line))
                    text += std::string("  ") + line.FileName + ":" + std::to_string(line.LineNumber);
                Log(L"  " + std::wstring(text.begin(), text.end()));
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }

        BOOL CALLBACK CollectMonitor(HMONITOR, HDC, LPRECT rect, LPARAM data)
        {
            reinterpret_cast<std::vector<RECT>*>(data)->push_back(*rect);
            return TRUE;
        }

        std::vector<RECT> Monitors()
        {
            std::vector<RECT> list;
            EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&list));
            return list;
        }

        [[noreturn]] void Abort(std::wstring const& why)
        {
            // Hide first (they are off-screen anyway), then leave without any further UI work.
            for (HWND h : g_windows) ShowWindow(h, SW_HIDE);
            Log(L"ABORT: " + why);
            ExitProcess(3);
        }

        bool IntersectsMonitor(RECT const& r)
        {
            for (auto const& m : Monitors())
            {
                RECT x{};
                if (IntersectRect(&x, &r, &m)) return true;
            }
            return MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr;
        }

        // The invariant: this process never takes the foreground (the user may be in a full-screen game).
        // The user switching between other apps meanwhile is fine: follow it instead of aborting.
        void ForegroundGuard(std::wstring const& where)
        {
            HWND fg = GetForegroundWindow();
            if (fg == g_foreground) return;
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            if (pid == GetCurrentProcessId())
            {
                wchar_t cls[128] = {};
                GetClassNameW(fg, cls, 128);
                Abort(L"this process took the foreground at " + where + L" (class " + cls + L")");
            }
            Log(L"foreground changed by the user at " + where + L"; continuing");
            g_foreground = fg;
        }

        void CheckForeground(wchar_t const* where)
        {
            ForegroundGuard(where);
            for (HWND h : g_windows)
            {
                RECT r{};
                if (GetWindowRect(h, &r) && IntersectsMonitor(r)) Abort(std::wstring(L"window on a monitor at ") + where);
            }
        }

        winrt::Windows::Foundation::IAsyncAction Delay(std::chrono::milliseconds ms)
        {
            auto queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
            co_await winrt::resume_after(ms);
            co_await wil::resume_foreground(queue);
        }

        // Saves the window content (RenderTargetBitmap) composited over the given backdrop colour
        // (the Mica backdrop itself is not part of the XAML tree).
        winrt::Windows::Foundation::IAsyncOperation<bool> Capture(Window window, std::wstring name, bool dark)
        {
            auto queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
            CheckForeground(name.c_str());
            try
            {
                RenderTargetBitmap rtb;
                co_await rtb.RenderAsync(window.Content());
                auto buffer = co_await rtb.GetPixelsAsync();
                int w = rtb.PixelWidth(), h = rtb.PixelHeight();
                std::vector<uint8_t> px(buffer.data(), buffer.data() + buffer.Length());
                uint8_t bg = dark ? 0x20 : 0xF3;
                for (size_t i = 0; i + 3 < px.size(); i += 4)
                {
                    double a = px[i + 3] / 255.0;
                    for (int k = 0; k < 3; ++k) px[i + k] = static_cast<uint8_t>(std::min(255.0, px[i + k] + (1.0 - a) * bg + 0.5));
                    px[i + 3] = 255;
                }
                auto folder = co_await winrt::Windows::Storage::StorageFolder::GetFolderFromPathAsync(g_outDir);
                auto file = co_await folder.CreateFileAsync(name + L".png", winrt::Windows::Storage::CreationCollisionOption::ReplaceExisting);
                auto stream = co_await file.OpenAsync(winrt::Windows::Storage::FileAccessMode::ReadWrite);
                auto encoder = co_await winrt::Windows::Graphics::Imaging::BitmapEncoder::CreateAsync(
                    winrt::Windows::Graphics::Imaging::BitmapEncoder::PngEncoderId(), stream);
                encoder.SetPixelData(winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,
                    winrt::Windows::Graphics::Imaging::BitmapAlphaMode::Ignore, static_cast<uint32_t>(w), static_cast<uint32_t>(h), 96, 96, px);
                co_await encoder.FlushAsync();
                stream.Close();
                co_await wil::resume_foreground(queue);
                Log(L"captured " + name + L".png " + std::to_wstring(w) + L"x" + std::to_wstring(h));
                CheckForeground(name.c_str());
                co_return true;
            }
            catch (winrt::hresult_error const& e)
            {
                Log(L"capture failed " + name + L": " + std::wstring(e.message()));
            }
            co_await wil::resume_foreground(queue);
            co_return false;
        }

        FrameworkElement FindNamed(DependencyObject const& node, std::wstring_view name)
        {
            if (auto fe = node.try_as<FrameworkElement>(); fe && fe.Name() == name) return fe;
            int n = Media::VisualTreeHelper::GetChildrenCount(node);
            for (int i = 0; i < n; ++i)
                if (auto hit = FindNamed(Media::VisualTreeHelper::GetChild(node, i), name)) return hit;
            return nullptr;
        }

        // Where the settings column (the "Pages" grid) and its visible page sit in the window.
        void LogSettingsColumn(Window const& window, std::wstring const& tag)
        {
            auto root = window.Content().as<UIElement>();
            auto pages = FindNamed(root, L"Pages");
            if (!pages) { Log(L"settings column " + tag + L": Pages not found"); return; }
            auto at = pages.TransformToVisual(root).TransformPoint({ 0, 0 });
            std::wstring line = L"settings column " + tag + L": x " + std::to_wstring(at.X) + L" w " +
                std::to_wstring(pages.ActualWidth()) + L" desired " + std::to_wstring(pages.DesiredSize().Width);
            auto panel = pages.as<Controls::Panel>();
            for (auto const& child : panel.Children())
            {
                auto fe = child.as<FrameworkElement>();
                if (fe.Visibility() != Visibility::Visible) continue;
                auto p = fe.TransformToVisual(root).TransformPoint({ 0, 0 });
                line += L"; page " + std::wstring(fe.Name()) + L" x " + std::to_wstring(p.X) + L" w " + std::to_wstring(fe.ActualWidth());
            }
            if (auto scroller = FindNamed(root, L"PageScroller"))
            {
                auto s = scroller.TransformToVisual(root).TransformPoint({ 0, 0 });
                line += L"; scroller x " + std::to_wstring(s.X) + L" w " + std::to_wstring(scroller.ActualWidth());
            }
            Log(line);
        }

        Session const* Focused()
        {
            return Controller::Get().Model().FocusedSession();
        }

        // Seeks the fake host's focused session and waits until its state arrived; then re-freezes
        // the test clock at that moment and steps it by `advance` ms.
        winrt::Windows::Foundation::IAsyncAction SeekTo(int64_t ms, double advance = 0)
        {
            // Freeze first: the seek's state then anchors exactly at the frozen moment (no time
            // passes for the lyrics clock), Advance() moves the frame to the wanted position, and
            // the real-time composition animations (scroll, emphasis) get time to settle.
            auto& c = Controller::Get();
            testclock::Freeze();
            if (auto s = Focused()) c.Media(s->side, s->key, "seek", ms);
            for (int i = 0; i < 100; ++i)
            {
                co_await Delay(30ms);
                auto s = Focused();
                if (s && std::llabs(s->positionMs - ms) < 5) break;
            }
            testclock::Advance(advance);
            co_await Delay(900ms);
        }

        winrt::Windows::Foundation::IAsyncAction ScenarioBody();

        winrt::fire_and_forget Scenario()
        {
            try
            {
                co_await ScenarioBody();
            }
            catch (winrt::hresult_error const& e)
            {
                Abort(L"exception 0x" + std::to_wstring(static_cast<uint32_t>(e.code())) + L": " + std::wstring(e.message()));
            }
            catch (std::exception const& e)
            {
                std::string w = e.what();
                Abort(L"std::exception: " + std::wstring(w.begin(), w.end()));
            }
            catch (...)
            {
                Abort(L"unknown exception");
            }
        }

        winrt::Windows::Foundation::IAsyncAction ScenarioBody()
        {
            auto& c = Controller::Get();
            Log(L"render test start");
            for (int i = 0; i < 400; ++i)
            {
                if (c.Link() == HostLink::Connected && c.Model().valid && c.CurrentLyrics().status == "found") break;
                co_await Delay(50ms);
            }
            if (!(c.Link() == HostLink::Connected && c.Model().valid)) Abort(L"fake host not reachable on the test pipe");
            g_foreground = GetForegroundWindow();
            Log(L"connected; lyrics " + std::wstring(c.CurrentLyrics().status.begin(), c.CurrentLyrics().status.end()));
            {
                // Lyrics typeface check: the bundled HarmonyOS Sans SC must actually be used (an
                // unresolvable family silently falls back, measuring like any unknown family).
                auto width = [](LyricWeight w, wchar_t const* family) {
                    Controls::TextBlock tb;
                    tb.FontSize(30);
                    if (family) {
                        tb.FontFamily(Media::FontFamily(family));
                        tb.FontWeight(winrt::Windows::UI::Text::FontWeight{ 700 });
                    } else {
                        ApplyLyricFont(tb, w);
                    }
                    tb.Text(L"Hello, world \x6674\x5929 \x4E03\x91CC\x9999 0123456789");
                    tb.Measure({ 1e6f, 1e6f });
                    return tb.DesiredSize().Width;
                };
                float fallback = width(LyricWeight::Bold, L"No Such Font MediaDuet");
                float bold = width(LyricWeight::Bold, nullptr), medium = width(LyricWeight::Medium, nullptr);
                Log(L"lyric font: files " + std::wstring(LyricFontAvailable() ? L"found" : L"MISSING") + L"; width bold " +
                    std::to_wstring(bold) + L" medium " + std::to_wstring(medium) +
                    L" unknown-family " + std::to_wstring(fallback) +
                    (std::abs(bold - fallback) > 0.5f ? L" -> HarmonyOS Sans SC in use" : L" -> NOT APPLIED (fallback font)"));
            }

            struct Size { int w, h; wchar_t const* name; };
            Size sizes[] = { { 1040, 760, L"1040x760" }, { 760, 560, L"760x560-min" }, { 1366, 768, L"1366x768" },
                { 1920, 1080, L"1920x1080-4k200" }, { 2560, 1440, L"2560x1440-4k150" }, { 3840, 2160, L"3840x2160-4k100" } };

            auto main = c.OpenOffscreen(WindowKind::Main, 1040, 760);
            Log(L"main window open");
            co_await Delay(1200ms);
            Log(L"main window settled");   // content loaded, artwork decoded, tint rendered
            // Dark, default size: the karaoke cases.
            if (Section(L"karaoke")) {
            co_await SeekTo(18200);
            co_await Capture(main, L"01-main-dark-1040x760-spaces", true);
            co_await SeekTo(17000, 300);
            co_await Capture(main, L"02-main-dark-line-start", true);
            co_await SeekTo(23400);
            co_await Capture(main, L"03-main-dark-cjk", true);
            co_await SeekTo(58800);
            co_await Capture(main, L"04-main-dark-chorus", true);
            co_await SeekTo(64000);
            co_await Capture(main, L"05-main-dark-interlude", true);
            co_await SeekTo(3000);
            co_await Capture(main, L"06-main-dark-intro", true);
            }

            // Responsive sizes (DIPs; 4K at 200 / 150 / 100 %).
            int n = 10;
            if (Section(L"sizes"))
            for (auto const& s : sizes)
            {
                PrepareOffscreen(GetHwnd(main), s.w, s.h);
                co_await Delay(700ms);
                co_await SeekTo(18200);
                co_await Capture(main, std::to_wstring(n++) + L"-main-dark-" + s.name, true);
            }

            // Light theme.
            if (Section(L"light")) {
            c.SetSetting("theme", "light");
            PrepareOffscreen(GetHwnd(main), 1040, 760);
            co_await Delay(900ms);
            co_await SeekTo(18200);
            co_await Capture(main, L"20-main-light-1040x760", false);
            PrepareOffscreen(GetHwnd(main), 3840, 2160);
            co_await Delay(900ms);
            co_await SeekTo(23400);
            co_await Capture(main, L"21-main-light-3840x2160", false);
            c.SetSetting("theme", "dark");
            PrepareOffscreen(GetHwnd(main), 1040, 760);
            co_await Delay(900ms);
            co_await SeekTo(18200);
            }

            // Lyrics -> no lyrics (a video without lyrics) and back, mid-animation frames (the
            // composition transitions run 8x slower here so frames can be taken during them).
            if (Section(L"transitions")) {
            SetSlowMotion(8);
            c.SetFocus(Side::Local, "chrome.exe", false);
            for (int i = 0; i < 60 && !(Focused() && Focused()->key == "chrome.exe"); ++i) co_await Delay(20ms);
            // Slow motion x8: pane fade 1.6 s, then (at ~1.8 s) the card glides for 3.4 s.
            co_await Capture(main, L"30-transition-off-0000", true);
            co_await Delay(700ms);
            co_await Capture(main, L"31-transition-off-0800", true);
            co_await Delay(1300ms);
            co_await Capture(main, L"32-transition-off-2300", true);
            co_await Delay(1000ms);
            co_await Capture(main, L"33-transition-off-3500", true);
            co_await Delay(3500ms);
            co_await Capture(main, L"34-transition-off-done", true);
            c.SetFocus(std::nullopt, {}, true);
            for (int i = 0; i < 60 && !(Focused() && Focused()->key != "chrome.exe"); ++i) co_await Delay(20ms);
            co_await Capture(main, L"35-transition-on-0000", true);
            co_await Delay(500ms);
            co_await Capture(main, L"36-transition-on-0700", true);
            co_await Delay(800ms);
            co_await Capture(main, L"37-transition-on-1700", true);
            co_await Delay(4500ms);
            co_await SeekTo(18200);
            co_await Capture(main, L"38-transition-on-done", true);

            // Large window transition (no lyrics <-> lyrics at 4K 100 %).
            PrepareOffscreen(GetHwnd(main), 3840, 2160);
            co_await Delay(800ms);
            c.SetFocus(Side::Local, "chrome.exe", false);
            for (int i = 0; i < 60 && !(Focused() && Focused()->key == "chrome.exe"); ++i) co_await Delay(20ms);
            co_await Delay(2200ms);
            co_await Capture(main, L"39-transition-off-3840-mid", true);
            co_await Delay(5000ms);
            co_await Capture(main, L"40-main-dark-3840-nolyrics", true);
            c.SetFocus(std::nullopt, {}, true);
            co_await Delay(800ms);
            co_await Capture(main, L"41-transition-on-3840-mid", true);
            co_await Delay(5000ms);
            SetSlowMotion(1);
            }

            // Next track while its lyrics are still loading (fake host answers "loading" first):
            // the layout holds, the old lines fade, then the new lines fade in line by line.
            if (Section(L"hold")) {
            PrepareOffscreen(GetHwnd(main), 1040, 760);
            co_await Delay(800ms);
            co_await SeekTo(18200);
            auto before = Focused() ? Focused()->trackId : std::string{};
            if (auto s = Focused()) c.Media(s->side, s->key, "next");
            for (int i = 0; i < 60 && Focused() && Focused()->trackId == before; ++i) co_await Delay(20ms);
            co_await Delay(250ms);
            co_await Capture(main, L"45-hold-while-loading", true);
            co_await Delay(1400ms);
            co_await Capture(main, L"46-hold-new-lyrics", true);
            }

            // Settings pages (About: developer card + notices; Devices: versions; General: work mode).
            if (Section(L"settings")) {
            PrepareOffscreen(GetHwnd(main), 1040, 900);
            co_await Delay(500ms);
            auto impl = winrt::get_self<winrt::MediaDuetUI::implementation::MainWindow>(main.as<winrt::MediaDuetUI::MainWindow>());
            co_await Capture(main, L"59-main-before-settings", true);   // first-run choice / version warning
            impl->ShowSettings(L"about", false);
            co_await Delay(1200ms);   // avatar decode
            co_await Capture(main, L"60-settings-about-dark", true);
            impl->ShowSettings(L"devices", false);
            co_await Delay(700ms);
            co_await Capture(main, L"61-settings-devices-dark", true);
            impl->ShowSettings(L"general", false);
            co_await Delay(700ms);
            co_await Capture(main, L"62-settings-general-dark", true);
            c.SetSetting("theme", "light");
            impl->ShowSettings(L"about", false);
            co_await Delay(900ms);
            co_await Capture(main, L"63-settings-about-light", false);
            c.SetSetting("theme", "dark");
            // Every page in a tall window, for checking the card layout as a whole.
            PrepareOffscreen(GetHwnd(main), 1040, 2600);
            co_await Delay(500ms);
            for (auto tag : {L"general", L"devices", L"unlock", L"connection", L"arbitration", L"lyrics", L"taskbar",
                             L"integration", L"compat", L"audio", L"diagnostics", L"about"}) {
                impl->ShowSettings(tag, false);
                co_await Delay(700ms);
                co_await Capture(main, std::wstring(L"64-settings-page-") + tag, true);
            }
            // Maximized-like wide windows: every page's column must sit at the same x.
            for (int w : { 1366, 1920, 2560 }) {
                PrepareOffscreen(GetHwnd(main), w, 1080);
                co_await Delay(500ms);
                for (auto tag : {L"general", L"devices", L"connection", L"arbitration", L"lyrics", L"taskbar",
                                 L"integration", L"compat", L"audio", L"diagnostics", L"about"}) {
                    impl->ShowSettings(tag, false);
                    co_await Delay(700ms);
                    LogSettingsColumn(main, std::to_wstring(w) + L" " + tag);
                    co_await Capture(main, L"65-settings-wide-" + std::to_wstring(w) + L"-" + tag, true);
                }
            }
            PrepareOffscreen(GetHwnd(main), 1040, 900);
            impl->ShowPlayer(false);
            co_await Delay(500ms);
            }

            // Compact window.
            Window compact{ nullptr };
            if (Section(L"compact")) {
            compact = c.OpenOffscreen(WindowKind::Compact, 340, 96);
            co_await Delay(900ms);
            co_await SeekTo(17900, 300);   // the line slide (240 ms) runs on the lyric clock
            co_await Capture(compact, L"50-compact-dark-spaces", true);
            co_await SeekTo(23100, 300);
            co_await Capture(compact, L"51-compact-dark-cjk", true);
            co_await SeekTo(58500, 300);
            co_await Capture(compact, L"52-compact-dark-long", true);
            }

            CheckForeground(L"end");
            Log(L"render test done");
            for (HWND h : g_windows) ShowWindow(h, SW_HIDE);
            if (compact) compact.Close();
            main.Close();
        }

        void Step(wchar_t const* what) { Log(std::wstring(L"step: ") + what); }
    }

    bool Active() { return g_active; }

    namespace
    {
        double g_slow = 1.0;
    }

    void SetSlowMotion(double factor) { g_slow = std::max(1.0, factor); }
    double SlowMotion() { return g_active ? g_slow : 1.0; }

    void LogLine(std::wstring const& line)
    {
        if (g_active) Log(line);
    }

    void SetActive(std::wstring outDir, std::wstring only)
    {
        g_only = std::move(only);
        std::set_terminate([] {
            std::wstring what = L"unknown";
            try
            {
                if (auto e = std::current_exception()) std::rethrow_exception(e);
            }
            catch (winrt::hresult_error const& e)
            {
                what = L"hresult 0x" + std::to_wstring(static_cast<uint32_t>(e.code())) + L" " + std::wstring(e.message());
            }
            catch (std::exception const& e)
            {
                std::string w = e.what();
                what = std::wstring(w.begin(), w.end());
            }
            catch (...)
            {
            }
            Log(L"TERMINATE: " + what);
            ExitProcess(4);
        });
        g_active = true;
        g_outDir = std::move(outDir);
        AddVectoredExceptionHandler(1, LogCrash);
        CreateDirectoryW(g_outDir.c_str(), nullptr);
        DeleteFileW((g_outDir + L"\\render-test.log").c_str());
    }

    void PrepareOffscreen(HWND hwnd, int widthDip, int heightDip)
    {
        if (!hwnd) Abort(L"no window handle");
        if (std::find(g_windows.begin(), g_windows.end(), hwnd) == g_windows.end()) g_windows.push_back(hwnd);
        // Never in the taskbar / Alt+Tab, never activated.
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        ex = (ex | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
        // Entirely beyond the union of all monitors.
        RECT all{};
        for (auto const& m : Monitors()) UnionRect(&all, &all, &m);
        int x = all.right + 3000, y = all.bottom + 3000;
        // Two passes: the window takes the DPI of the monitor nearest to its new place.
        for (int pass = 0; pass < 2; ++pass)
        {
            double scale = ScaleFor(hwnd);
            int w = static_cast<int>(widthDip * scale), h = static_cast<int>(heightDip * scale);
            RECT target{ x, y, x + w, y + h };
            if (IntersectsMonitor(target)) Abort(L"target rectangle intersects a monitor");
            SetWindowPos(hwnd, HWND_BOTTOM, x, y, w, h, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            RECT r{};
            GetWindowRect(hwnd, &r);
            if (IntersectsMonitor(r)) Abort(L"window rectangle intersects a monitor after moving");
        }
        if (!IsWindowVisible(hwnd))
        {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            RECT r{};
            GetWindowRect(hwnd, &r);
            if (IntersectsMonitor(r)) Abort(L"window rectangle intersects a monitor after showing");
        }
        if (g_foreground) ForegroundGuard(L"preparing");
    }

    void Run()
    {
        Scenario();
    }
}
