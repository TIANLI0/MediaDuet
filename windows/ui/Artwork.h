// Album-art loading (decoded once per path at the size the caller needs) and its colour palette
// (up to three dominant colours, normalised to pleasant mid-tones) for the background tint.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.h>

namespace mdui
{
    class ArtworkSlot
    {
    public:
        using Applied = std::function<void(winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage const& image)>;
        // Dominant colours, most prominent first; empty when there is no artwork / it failed.
        using ColorReady = std::function<void(std::vector<winrt::Windows::UI::Color> const& palette)>;

        // Requests the image at `path` decoded to `decodePixels` (physical px, square-ish).
        // Does nothing when the same path is already loaded or loading. Empty path clears.
        void Request(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue, std::string const& path,
            int decodePixels, Applied applied, ColorReady color = nullptr);
        void Clear();
        std::string const& Path() const { return m_path; }

    private:
        std::string m_path;
        uint64_t m_generation{ 0 };
        std::shared_ptr<uint64_t> m_shared;   // read by in-flight loads to detect staleness
    };

    // Cached palettes (computed from a 24x24 downscale on a background thread).
    std::vector<winrt::Windows::UI::Color> CachedPalette(std::string const& path);
}
