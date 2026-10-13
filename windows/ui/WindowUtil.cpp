#include "pch.h"
#include "WindowUtil.h"
#include "RenderTest.h"
#include "resource.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <microsoft.ui.xaml.window.h>

#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

#include <memory>

using namespace winrt;
using namespace winrt::Microsoft::UI;
using namespace winrt::Microsoft::UI::Windowing;
using namespace winrt::Microsoft::UI::Xaml;

namespace mdui
{
    HWND GetHwnd(Window const& window)
    {
        HWND hwnd = nullptr;
        if (auto native = window.try_as<::IWindowNative>())
            native->get_WindowHandle(&hwnd);
        return hwnd;
    }

    double ScaleFor(HWND hwnd)
    {
        UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;
        return (dpi ? dpi : 96) / 96.0;
    }

    namespace
    {
        constexpr UINT_PTR kSubclassId = 0x4D44;   // 'MD'

        LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
        {
            auto hook = reinterpret_cast<MessageHook*>(data);
            if (msg == WM_NCDESTROY)
            {
                RemoveWindowSubclass(hwnd, SubclassProc, id);
                delete hook;
                return DefSubclassProc(hwnd, msg, wp, lp);
            }
            if (hook && *hook)
            {
                if (auto result = (*hook)(hwnd, msg, wp, lp)) return *result;
            }
            return DefSubclassProc(hwnd, msg, wp, lp);
        }
    }

    void InstallMessageHook(HWND hwnd, MessageHook hook)
    {
        auto heap = new MessageHook(std::move(hook));
        if (!SetWindowSubclass(hwnd, SubclassProc, kSubclassId, reinterpret_cast<DWORD_PTR>(heap)))
            delete heap;
    }

    void ApplyBackdrop(Window const& window)
    {
        using namespace winrt::Microsoft::UI::Composition::SystemBackdrops;
        if (MicaController::IsSupported())
        {
            Media::MicaBackdrop mica;
            window.SystemBackdrop(mica);
        }
        else if (DesktopAcrylicController::IsSupported())
        {
            Media::DesktopAcrylicBackdrop acrylic;
            window.SystemBackdrop(acrylic);
        }
    }

    void ApplyCaptionColors(Window const& window, bool dark)
    {
        auto titleBar = window.AppWindow().TitleBar();
        using winrt::Windows::UI::Color;
        Color fg = dark ? Color{ 255, 255, 255, 255 } : Color{ 255, 0, 0, 0 };
        Color fgInactive = dark ? Color{ 255, 150, 150, 150 } : Color{ 255, 120, 120, 120 };
        Color hover = dark ? Color{ 24, 255, 255, 255 } : Color{ 18, 0, 0, 0 };
        Color pressed = dark ? Color{ 14, 255, 255, 255 } : Color{ 10, 0, 0, 0 };
        Color transparent{ 0, 0, 0, 0 };
        titleBar.ButtonBackgroundColor(transparent);
        titleBar.ButtonInactiveBackgroundColor(transparent);
        titleBar.ButtonForegroundColor(fg);
        titleBar.ButtonInactiveForegroundColor(fgInactive);
        titleBar.ButtonHoverBackgroundColor(hover);
        titleBar.ButtonHoverForegroundColor(fg);
        titleBar.ButtonPressedBackgroundColor(pressed);
        titleBar.ButtonPressedForegroundColor(fg);
    }

    static void SetDwmDark(HWND hwnd, bool dark)
    {
        // Keeps the system frame/border colour consistent with the XAML theme.
        BOOL useDark = dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDark, sizeof(useDark));
    }

    void ApplyWindowIcon(HWND hwnd)
    {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        auto bigIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
            GetSystemMetricsForDpi(SM_CXICON, GetDpiForWindow(hwnd)),
            GetSystemMetricsForDpi(SM_CYICON, GetDpiForWindow(hwnd)), LR_SHARED));
        auto smallIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
            GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(hwnd)),
            GetSystemMetricsForDpi(SM_CYSMICON, GetDpiForWindow(hwnd)), LR_SHARED));
        if (bigIcon) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
        if (smallIcon) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
    }

    void SetRoundedCorners(HWND hwnd)
    {
        DWORD pref = 2;   // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &pref, sizeof(pref));
    }

    void BringToFront(Window const& window)
    {
        if (render::Active()) return;   // the render test never activates anything
        if (auto presenter = window.AppWindow().Presenter().try_as<OverlappedPresenter>())
        {
            if (presenter.State() == OverlappedPresenterState::Minimized) presenter.Restore();
        }
        window.Activate();
        HWND hwnd = GetHwnd(window);
        if (hwnd) SetForegroundWindow(hwnd);
    }

    RECT WorkAreaFor(RECT const& r)
    {
        HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (mon && GetMonitorInfoW(mon, &mi)) return mi.rcWork;
        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        return wa;
    }

    bool IsRectOnAnyMonitor(RECT const& r)
    {
        // Requires a meaningful part (the top-left 48x48 px) to be on a monitor.
        RECT probe{ r.left, r.top, r.left + 48, r.top + 48 };
        return MonitorFromRect(&probe, MONITOR_DEFAULTTONULL) != nullptr;
    }

    RECT ClampToWorkArea(RECT const& r)
    {
        RECT wa = WorkAreaFor(r);
        LONG w = r.right - r.left, h = r.bottom - r.top;
        w = std::min(w, wa.right - wa.left);
        h = std::min(h, wa.bottom - wa.top);
        LONG x = std::clamp(r.left, wa.left, wa.right - w);
        LONG y = std::clamp(r.top, wa.top, wa.bottom - h);
        return RECT{ x, y, x + w, y + h };
    }

    std::wstring ExeDirectory()
    {
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (n < path.size())
            {
                path.resize(n);
                break;
            }
            path.resize(path.size() * 2);
        }
        auto slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash + 1);
    }

    bool SystemAnimationsEnabled()
    {
        BOOL enabled = TRUE;
        if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) return true;
        return enabled != FALSE;
    }

    namespace
    {
        winrt::Microsoft::UI::Composition::Visual PrepareVisual(UIElement const& element)
        {
            winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::SetIsTranslationEnabled(element, true);
            return winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(element);
        }
    }

    void AnimateVisualOpacity(UIElement const& element, float from, float to, int durationMs, int delayMs)
    {
        auto visual = PrepareVisual(element);
        auto compositor = visual.Compositor();
        auto ease = compositor.CreateCubicBezierEasingFunction({ 0.1f, 0.9f }, { 0.2f, 1.0f });
        auto anim = compositor.CreateScalarKeyFrameAnimation();
        anim.InsertKeyFrame(0.0f, from);
        anim.InsertKeyFrame(1.0f, to, ease);
        double slow = render::SlowMotion();
        anim.Duration(std::chrono::milliseconds(std::max(1, static_cast<int>(durationMs * slow))));
        if (delayMs > 0)
        {
            anim.DelayTime(std::chrono::milliseconds(static_cast<int>(delayMs * slow)));
            anim.DelayBehavior(winrt::Microsoft::UI::Composition::AnimationDelayBehavior::SetInitialValueBeforeDelay);
        }
        visual.StartAnimation(L"Opacity", anim);
    }

    void AnimateVisualTranslation(UIElement const& element, winrt::Windows::Foundation::Numerics::float3 from,
        winrt::Windows::Foundation::Numerics::float3 to, int durationMs, int delayMs)
    {
        auto visual = PrepareVisual(element);
        auto compositor = visual.Compositor();
        auto ease = compositor.CreateCubicBezierEasingFunction({ 0.1f, 0.9f }, { 0.2f, 1.0f });
        auto anim = compositor.CreateVector3KeyFrameAnimation();
        anim.InsertKeyFrame(0.0f, from);
        anim.InsertKeyFrame(1.0f, to, ease);
        double slow = render::SlowMotion();
        anim.Duration(std::chrono::milliseconds(std::max(1, static_cast<int>(durationMs * slow))));
        if (delayMs > 0)
        {
            anim.DelayTime(std::chrono::milliseconds(static_cast<int>(delayMs * slow)));
            anim.DelayBehavior(winrt::Microsoft::UI::Composition::AnimationDelayBehavior::SetInitialValueBeforeDelay);
        }
        visual.StartAnimation(L"Translation", anim);
    }

    void ResetVisual(UIElement const& element)
    {
        auto visual = PrepareVisual(element);
        visual.StopAnimation(L"Opacity");
        visual.StopAnimation(L"Translation");
        visual.Opacity(1.0f);
        visual.Properties().InsertVector3(L"Translation", { 0, 0, 0 });
    }

    // ---- WindowHost --------------------------------------------------------------------------

    void WindowHost::Attach(Window const& window, FrameworkElement const& root, SIZE minSizeDip, bool backdrop)
    {
        m_window = winrt::make_weak(window);
        m_root = winrt::make_weak(root);
        m_hwnd = GetHwnd(window);
        m_minDip = minSizeDip;
        m_customCaption = window.ExtendsContentIntoTitleBar();
        if (backdrop) ApplyBackdrop(window);
        ApplyWindowIcon(m_hwnd);
        SetRoundedCorners(m_hwnd);

        m_visibilityToken = window.VisibilityChanged([this, alive = m_alive](auto&&, WindowVisibilityChangedEventArgs const& e) {
            if (*alive) SetShowing(e.Visible(), m_minimized);
        });
        m_themeToken = root.ActualThemeChanged([this, alive = m_alive](auto&&, auto&&) {
            if (!*alive) return;
            SetDwmDark(m_hwnd, IsDark());
            if (auto w = m_window.get(); w && m_customCaption) ApplyCaptionColors(w, IsDark());
        });

        InstallMessageHook(m_hwnd, [this, alive = m_alive](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> std::optional<LRESULT> {
            if (!*alive) return std::nullopt;
            switch (msg)
            {
            case WM_GETMINMAXINFO:
            {
                auto info = reinterpret_cast<MINMAXINFO*>(lp);
                double scale = ScaleFor(hwnd);
                info->ptMinTrackSize.x = static_cast<LONG>(m_minDip.cx * scale);
                info->ptMinTrackSize.y = static_cast<LONG>(m_minDip.cy * scale);
                if (render::Active())
                {
                    // Off-screen 4K layouts may be larger than this machine's desktop.
                    info->ptMaxTrackSize.x = 16000;
                    info->ptMaxTrackSize.y = 16000;
                }
                break;
            }
            case WM_SIZE:
                if (wp == SIZE_MINIMIZED) SetShowing(m_visible, true);
                else if (wp == SIZE_RESTORED || wp == SIZE_MAXIMIZED) SetShowing(m_visible, false);
                break;
            default:
                break;
            }
            if (extraHook) return extraHook(hwnd, msg, wp, lp);
            return std::nullopt;
        });
    }

    void WindowHost::Detach()
    {
        *m_alive = false;
        if (auto w = m_window.get()) w.VisibilityChanged(m_visibilityToken);
        if (auto r = m_root.get()) r.ActualThemeChanged(m_themeToken);
        showingChanged = nullptr;
        extraHook = nullptr;
    }

    void WindowHost::SetShowing(bool visible, bool minimized)
    {
        bool before = IsShowing();
        m_visible = visible;
        m_minimized = minimized;
        if (before != IsShowing() && showingChanged) showingChanged();
    }

    bool WindowHost::IsDark() const
    {
        if (auto r = m_root.get()) return r.ActualTheme() == ElementTheme::Dark;
        return false;
    }

    void WindowHost::ApplyTheme(ElementTheme theme)
    {
        auto root = m_root.get();
        if (!root) return;
        if (root.RequestedTheme() != theme) root.RequestedTheme(theme);
        SetDwmDark(m_hwnd, IsDark());
        if (auto w = m_window.get(); w && m_customCaption) ApplyCaptionColors(w, IsDark());
    }
}
