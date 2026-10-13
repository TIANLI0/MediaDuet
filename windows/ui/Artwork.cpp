#include "pch.h"
#include "Artwork.h"
#include "Model.h"

#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>

using namespace winrt;
using namespace winrt::Microsoft::UI::Dispatching;
using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Storage;
using winrt::Windows::UI::Color;

namespace mdui
{
    namespace
    {
        std::mutex g_colorMutex;
        std::unordered_map<std::string, std::vector<Color>> g_colors;

        // Normalises a colour to a pleasant mid-tone (HSL lightness 0.42..0.58, saturation >= 0.35
        // when chromatic).
        Color MidTone(double r, double g, double b)
        {
            double mx = std::max({ r, g, b }), mn = std::min({ r, g, b });
            double l = (mx + mn) / 2, s = 0, h = 0;
            if (mx != mn)
            {
                double d = mx - mn;
                s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
                if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
                else if (mx == g) h = (b - r) / d + 2;
                else h = (r - g) / d + 4;
                h /= 6;
                s = std::max(s, 0.35);
            }
            l = std::clamp(l, 0.42, 0.58);
            auto hue2rgb = [](double p, double q, double t) {
                if (t < 0) t += 1;
                if (t > 1) t -= 1;
                if (t < 1.0 / 6) return p + (q - p) * 6 * t;
                if (t < 1.0 / 2) return q;
                if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
                return p;
            };
            if (s == 0) { r = g = b = l; }
            else
            {
                double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
                r = hue2rgb(p, q, h + 1.0 / 3); g = hue2rgb(p, q, h); b = hue2rgb(p, q, h - 1.0 / 3);
            }
            auto to8 = [](double x) { return static_cast<uint8_t>(std::clamp(x, 0.0, 1.0) * 255.0 + 0.5); };
            return Color{ 255, to8(r), to8(g), to8(b) };
        }

        // Up to three dominant colours: the strongest hue bins (30 degrees each) that are not
        // neighbours of an already chosen one; the plain average when the artwork is grey.
        std::vector<Color> ComputePalette(std::vector<uint8_t> const& bgra)
        {
            struct Bin { double w = 0, r = 0, g = 0, b = 0; };
            std::array<Bin, 12> bins{};
            double ar = 0, ag = 0, ab = 0, an = 0;
            for (size_t i = 0; i + 3 < bgra.size(); i += 4)
            {
                double a = bgra[i + 3] / 255.0;
                if (a < 0.5) continue;
                double b = bgra[i] / 255.0 / a, g = bgra[i + 1] / 255.0 / a, r = bgra[i + 2] / 255.0 / a;
                double mx = std::max({ r, g, b }), mn = std::min({ r, g, b });
                double v = mx, s = mx > 0 ? (mx - mn) / mx : 0;
                ar += r; ag += g; ab += b; an += 1;
                if (s < 0.15 || v < 0.12) continue;
                double h = 0, d = mx - mn;
                if (mx == r) h = std::fmod((g - b) / d, 6.0);
                else if (mx == g) h = (b - r) / d + 2;
                else h = (r - g) / d + 4;
                if (h < 0) h += 6;
                auto idx = std::min<size_t>(static_cast<size_t>(h * 2), 11);
                double w = s * v;
                bins[idx].w += w; bins[idx].r += r * w; bins[idx].g += g * w; bins[idx].b += b * w;
            }
            std::vector<Color> palette;
            if (an == 0) return palette;
            std::array<size_t, 12> order{};
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return bins[x].w > bins[y].w; });
            std::vector<size_t> chosen;
            for (size_t idx : order)
            {
                if (chosen.size() >= 3 || bins[idx].w <= an * 0.02) break;
                bool neighbour = std::any_of(chosen.begin(), chosen.end(), [&](size_t c) {
                    size_t d = idx > c ? idx - c : c - idx;
                    return d <= 1 || d >= 11;
                });
                if (neighbour) continue;
                chosen.push_back(idx);
                auto const& bin = bins[idx];
                palette.push_back(MidTone(bin.r / bin.w, bin.g / bin.w, bin.b / bin.w));
            }
            if (palette.empty()) palette.push_back(MidTone(ar / an, ag / an, ab / an));
            return palette;
        }

        fire_and_forget LoadAsync(DispatcherQueue queue, std::string path, int pixels,
            std::shared_ptr<uint64_t> generation, uint64_t mine,
            ArtworkSlot::Applied applied, ArtworkSlot::ColorReady colorReady)
        {
            bool failed = false;
            std::vector<Color> dominant;
            Windows::Storage::Streams::IRandomAccessStream stream{ nullptr };
            try
            {
                co_await resume_background();
                auto file = co_await StorageFile::GetFileFromPathAsync(Widen(path));
                if (colorReady)
                {
                    bool cached = false;
                    {
                        std::lock_guard lock(g_colorMutex);
                        auto it = g_colors.find(path);
                        if (it != g_colors.end()) { dominant = it->second; cached = true; }
                    }
                    if (!cached)
                    {
                        auto colorStream = co_await file.OpenReadAsync();
                        auto decoder = co_await BitmapDecoder::CreateAsync(colorStream);
                        BitmapTransform transform;
                        transform.ScaledWidth(24);
                        transform.ScaledHeight(24);
                        transform.InterpolationMode(BitmapInterpolationMode::Fant);
                        auto data = co_await decoder.GetPixelDataAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied,
                            transform, ExifOrientationMode::IgnoreExifOrientation, ColorManagementMode::DoNotColorManage);
                        auto pixelsArr = data.DetachPixelData();
                        dominant = ComputePalette(std::vector<uint8_t>(pixelsArr.begin(), pixelsArr.end()));
                        std::lock_guard lock(g_colorMutex);
                        if (g_colors.size() > 256) g_colors.clear();
                        g_colors[path] = dominant;
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
                if (colorReady) colorReady({});
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
            if (colorReady) colorReady(decodeFailed ? std::vector<Color>{} : dominant);
        }
    }

    void ArtworkSlot::Request(DispatcherQueue const& queue, std::string const& path, int decodePixels,
        Applied applied, ColorReady color)
    {
        if (path == m_path) return;
        m_path = path;
        ++m_generation;
        if (!m_shared) m_shared = std::make_shared<uint64_t>(0);
        *m_shared = m_generation;
        if (path.empty())
        {
            if (applied) applied(nullptr);
            if (color) color({});
            return;
        }
        LoadAsync(queue, path, decodePixels, m_shared, m_generation, std::move(applied), std::move(color));
    }

    void ArtworkSlot::Clear()
    {
        m_path.clear();
        ++m_generation;
        if (m_shared) *m_shared = m_generation;
    }

    std::vector<Color> CachedPalette(std::string const& path)
    {
        std::lock_guard lock(g_colorMutex);
        auto it = g_colors.find(path);
        return it == g_colors.end() ? std::vector<Color>{} : it->second;
    }
}
