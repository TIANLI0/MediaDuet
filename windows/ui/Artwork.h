// Album-art loading (decoded once per path at the size the caller needs) and the small prepared
// copy that the flowing window background is drawn from (AmbientRender.h).
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

#include "AmbientRender.h"

namespace mdui
{
    class ArtworkSlot
    {
    public:
        using Applied = std::function<void(winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage const& image)>;
        // Prepared background copy of the artwork; null when there is no artwork / it failed.
        using BackdropReady = std::function<void(std::shared_ptr<ambient::Artwork const> const& art)>;

        // Requests the image at `path` decoded to `decodePixels` (physical px, square-ish).
        // Does nothing when the same path is already loaded or loading. Empty path clears.
        void Request(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue, std::string const& path,
            int decodePixels, Applied applied, BackdropReady backdrop = nullptr);
        void Clear();
        std::string const& Path() const { return m_path; }

    private:
        std::string m_path;
        uint64_t m_generation{ 0 };
        std::shared_ptr<uint64_t> m_shared;   // read by in-flight loads to detect staleness
    };
}
