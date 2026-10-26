// Window background: the current artwork as a slowly flowing, heavily blurred colour field under a
// flat scrim, like the Android app (AmbientRender.h draws it); the window backdrop (Mica) shows
// while there is no artwork.
//
// Frames are rendered off the UI thread into a WriteableBitmap (dithered, so no colour banding) and
// shown by an Image stretched over the host element. The field flows with the display refresh (at
// most ~60 fps) while SetFlowing is on (music playing, window visible, animations enabled) and holds
// still otherwise, when only artwork, theme, size (debounced 120 ms) and DPI changes render a frame.
// Two Image slots crossfade when the artwork changes.
#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

#include "AmbientRender.h"

namespace mdui
{
    class AmbientBackground
    {
    public:
        void Attach(winrt::Microsoft::UI::Xaml::Controls::Grid const& host);
        void Detach();
        // Prepared artwork (ArtworkSlot); null fades back to the window backdrop.
        void SetArtwork(std::shared_ptr<ambient::Artwork const> art);
        void SetFlowing(bool flowing);
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
        std::shared_ptr<ambient::Artwork const> m_art;
        bool m_artChanged{ false };
        bool m_animations{ true };
        bool m_flowing{ false };
        float m_seconds{ ambient::kStartSeconds };
        std::optional<winrt::Windows::Foundation::TimeSpan> m_lastFrame;   // RenderingTime of the last frame
        bool m_busy{ false };      // a render is in flight (never more than one)
        bool m_pending{ false };   // render again when it lands
        double m_scale{ 1.0 };
        bool m_renderedDark{ false };
        int m_renderedWidth{ 0 };
        int m_renderedHeight{ 0 };
        float m_renderedSeconds{ -1 };
        uint64_t m_generation{ 0 };
        std::shared_ptr<Shared> m_shared;
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_debounce{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_release{ nullptr };
        winrt::event_token m_renderingToken{};
        winrt::event_token m_sizeToken{};
        winrt::event_token m_themeToken{};
        winrt::event_token m_loadedToken{};
        winrt::event_token m_rootChangedToken{};
        winrt::Microsoft::UI::Xaml::XamlRoot m_xamlRoot{ nullptr };
    };
}
