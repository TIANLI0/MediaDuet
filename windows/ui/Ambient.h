// Artwork-tinted window background without colour banding.
//
// Replaces the former low-alpha LinearGradientBrush (8-bit steps were visible across a large
// window): the tint and soft colour fields are rendered by AmbientRender.h in float with
// triangular-PDF dither into a WriteableBitmap at the host element's physical pixel size and shown
// 1:1 by an Image (Stretch=Fill only absorbs sub-pixel rounding). Two Image slots crossfade when the
// palette changes. Re-rendered (off the UI thread) on palette, theme, size (debounced 120 ms) and
// DPI changes; nothing runs while nothing changes.
#pragma once

#include <memory>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.h>

#include "AmbientRender.h"

namespace mdui
{
    class AmbientBackground
    {
    public:
        void Attach(winrt::Microsoft::UI::Xaml::Controls::Grid const& host, ambient::Shape shape);
        void Detach();
        // Dominant artwork colours (most prominent first); empty fades the tint out.
        void SetPalette(std::vector<winrt::Windows::UI::Color> const& palette);
        void SetAnimations(bool enabled);

    private:
        struct Shared;
        void Schedule(bool debounce);
        void Render();
        void Present(std::vector<uint8_t> const& pixels, int width, int height, bool crossfade);
        void FadeOut();
        void ApplyTransitions();

        winrt::Microsoft::UI::Xaml::Controls::Grid m_host{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Image m_images[2]{ nullptr, nullptr };
        winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap m_bitmaps[2]{ nullptr, nullptr };
        int m_front{ -1 };
        ambient::Shape m_shape{ ambient::Shape::Main };
        std::vector<winrt::Windows::UI::Color> m_palette;
        bool m_paletteChanged{ false };
        bool m_animations{ true };
        double m_scale{ 1.0 };
        bool m_renderedDark{ false };
        int m_renderedWidth{ 0 };
        int m_renderedHeight{ 0 };
        uint64_t m_generation{ 0 };
        std::shared_ptr<Shared> m_shared;
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_debounce{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_release{ nullptr };
        winrt::event_token m_sizeToken{};
        winrt::event_token m_themeToken{};
        winrt::event_token m_loadedToken{};
        winrt::event_token m_rootChangedToken{};
        winrt::Microsoft::UI::Xaml::XamlRoot m_xamlRoot{ nullptr };
    };
}
