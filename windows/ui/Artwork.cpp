#include "pch.h"
#include "Artwork.h"
#include "Model.h"

#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>

using namespace winrt;
using namespace winrt::Microsoft::UI::Dispatching;
using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Storage;

namespace mdui
{
    namespace
    {
        std::mutex g_backdropMutex;
        std::unordered_map<std::string, std::shared_ptr<ambient::Artwork const>> g_backdrops;

        fire_and_forget LoadAsync(DispatcherQueue queue, std::string path, int pixels,
            std::shared_ptr<uint64_t> generation, uint64_t mine,
            ArtworkSlot::Applied applied, ArtworkSlot::BackdropReady backdropReady)
        {
            bool failed = false;
            std::shared_ptr<ambient::Artwork const> backdrop;
            Windows::Storage::Streams::IRandomAccessStream stream{ nullptr };
            try
            {
                co_await resume_background();
                auto file = co_await StorageFile::GetFileFromPathAsync(Widen(path));
                if (backdropReady)
                {
                    {
                        std::lock_guard lock(g_backdropMutex);
                        auto it = g_backdrops.find(path);
                        if (it != g_backdrops.end()) backdrop = it->second;
                    }
                    if (!backdrop)
                    {
                        auto smallStream = co_await file.OpenReadAsync();
                        auto decoder = co_await BitmapDecoder::CreateAsync(smallStream);
                        BitmapTransform transform;
                        transform.ScaledWidth(ambient::kArtSide);
                        transform.ScaledHeight(ambient::kArtSide);
                        transform.InterpolationMode(BitmapInterpolationMode::Fant);
                        auto data = co_await decoder.GetPixelDataAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied,
                            transform, ExifOrientationMode::IgnoreExifOrientation, ColorManagementMode::DoNotColorManage);
                        auto pixelsArr = data.DetachPixelData();
                        if (pixelsArr.size() < static_cast<size_t>(ambient::kArtSide) * ambient::kArtSide * 4) throw hresult_error(E_FAIL);
                        backdrop = std::make_shared<ambient::Artwork const>(ambient::PrepareArtwork(pixelsArr.data(), ambient::kArtSide));
                        std::lock_guard lock(g_backdropMutex);
                        if (g_backdrops.size() > 256) g_backdrops.clear();
                        g_backdrops[path] = backdrop;
                    }
                }
                stream = co_await file.OpenReadAsync();
            }
            catch (...)
            {
                failed = true;
            }

            co_await wil::resume_foreground(queue);
            if (*generation != mine) co_return;
            if (failed)
            {
                if (applied) applied(nullptr);
                if (backdropReady) backdropReady(nullptr);
                co_return;
            }

            BitmapImage bitmap;
            bitmap.DecodePixelType(DecodePixelType::Physical);
            bitmap.DecodePixelWidth(pixels);
            bool decodeFailed = false;
            try
            {
                co_await bitmap.SetSourceAsync(stream);
            }
            catch (...)
            {
                decodeFailed = true;
            }
            co_await wil::resume_foreground(queue);
            if (*generation != mine) co_return;
            if (applied) applied(decodeFailed ? nullptr : bitmap);
            if (backdropReady) backdropReady(decodeFailed ? nullptr : backdrop);
        }
    }

    void ArtworkSlot::Request(DispatcherQueue const& queue, std::string const& path, int decodePixels,
        Applied applied, BackdropReady backdrop)
    {
        if (path == m_path) return;
        m_path = path;
        ++m_generation;
        if (!m_shared) m_shared = std::make_shared<uint64_t>(0);
        *m_shared = m_generation;
        if (path.empty())
        {
            if (applied) applied(nullptr);
            if (backdrop) backdrop(nullptr);
            return;
        }
        LoadAsync(queue, path, decodePixels, m_shared, m_generation, std::move(applied), std::move(backdrop));
    }

    void ArtworkSlot::Clear()
    {
        m_path.clear();
        ++m_generation;
        if (m_shared) *m_shared = m_generation;
    }
}
