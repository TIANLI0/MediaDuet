// Win32 / AppWindow helpers shared by the windows.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Windows.Foundation.Numerics.h>

namespace mdui
{
    HWND GetHwnd(winrt::Microsoft::UI::Xaml::Window const& window);
    double ScaleFor(HWND hwnd);   // DPI / 96

    // Window message hook (comctl32 subclass). Return a value to swallow the message.
    using MessageHook = std::function<std::optional<LRESULT>(HWND, UINT, WPARAM, LPARAM)>;
    void InstallMessageHook(HWND hwnd, MessageHook hook);

    // Mica (or Desktop Acrylic when Mica is not supported).
    void ApplyBackdrop(winrt::Microsoft::UI::Xaml::Window const& window);
    // Caption button colours for an ExtendsContentIntoTitleBar window.
    void ApplyCaptionColors(winrt::Microsoft::UI::Xaml::Window const& window, bool dark);
    void ApplyWindowIcon(HWND hwnd);
    void SetRoundedCorners(HWND hwnd);

    // Brings a window to the foreground (restoring it if minimised).
    void BringToFront(winrt::Microsoft::UI::Xaml::Window const& window);

    // Moves/resizes so the window is fully inside the work area of the monitor it mostly overlaps
    // (or the primary monitor if it is not on any monitor any more). Physical pixels.
    RECT ClampToWorkArea(RECT const& r);
    bool IsRectOnAnyMonitor(RECT const& r);
    RECT WorkAreaFor(RECT const& r);

    // Path of the directory containing MediaDuetUI.exe (with trailing backslash).
    std::wstring ExeDirectory();

    // Windows' "Animation effects" setting (Settings > Accessibility > Visual effects).
    bool SystemAnimationsEnabled();
    // Composition animations on an element's hand-off visual (never mixed with the element's own
    // Opacity / Translation facade properties). Decelerating ease; delayMs keeps the start value.
    void AnimateVisualOpacity(winrt::Microsoft::UI::Xaml::UIElement const& element, float from, float to, int durationMs, int delayMs = 0);
    void AnimateVisualTranslation(winrt::Microsoft::UI::Xaml::UIElement const& element, winrt::Windows::Foundation::Numerics::float3 from,
        winrt::Windows::Foundation::Numerics::float3 to, int durationMs, int delayMs = 0);
    // Stops those animations and puts the visual back to opacity 1 / no translation.
    void ResetVisual(winrt::Microsoft::UI::Xaml::UIElement const& element);

    // A theme resource (App.xaml theme dictionaries, then WinUI's own) as `element`'s *actual* theme
    // resolves it. Resources().Lookup() from code follows the application's theme, which differs
    // from the window's when MediaDuet's theme is not the Windows one. nullptr when not found.
    winrt::Windows::Foundation::IInspectable LookupThemeResource(winrt::Microsoft::UI::Xaml::FrameworkElement const& element,
        std::wstring_view key);
    winrt::Microsoft::UI::Xaml::Media::Brush ThemeBrush(winrt::Microsoft::UI::Xaml::FrameworkElement const& element, std::wstring_view key);
    // Colour resources (and solid brushes, by their colour); `fallback` when not found.
    winrt::Windows::UI::Color ThemeColor(winrt::Microsoft::UI::Xaml::FrameworkElement const& element, std::wstring_view key,
        winrt::Windows::UI::Color fallback);

    // Common per-window plumbing: backdrop, icon, theme + caption colours, minimum size and
    // "is the window actually showing" tracking (visible and not minimised).
    class WindowHost
    {
    public:
        void Attach(winrt::Microsoft::UI::Xaml::Window const& window,
            winrt::Microsoft::UI::Xaml::FrameworkElement const& root, SIZE minSizeDip, bool backdrop = true);
        void Detach();
        bool IsShowing() const { return m_visible && !m_minimized; }
        HWND Hwnd() const { return m_hwnd; }
        double Scale() const { return ScaleFor(m_hwnd); }
        void ApplyTheme(winrt::Microsoft::UI::Xaml::ElementTheme theme);
        bool IsDark() const;

        std::function<void()> showingChanged;
        // The root's ActualTheme changed (MediaDuet's theme or, with "system", Windows' own).
        std::function<void()> themeChanged;
        MessageHook extraHook;

    private:
        void SetShowing(bool visible, bool minimized);

        winrt::weak_ref<winrt::Microsoft::UI::Xaml::Window> m_window;
        winrt::weak_ref<winrt::Microsoft::UI::Xaml::FrameworkElement> m_root;
        HWND m_hwnd{ nullptr };
        SIZE m_minDip{};
        bool m_visible{ true };
        bool m_minimized{ false };
        bool m_customCaption{ false };
        winrt::event_token m_visibilityToken{};
        winrt::event_token m_themeToken{};
        std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    };
}
