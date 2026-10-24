#include "pch.h"
#include "Ambient.h"
#include "RenderTest.h"

#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;

namespace mdui
{
    // State shared with in-flight renders (which may finish after Detach).
    struct AmbientBackground::Shared
    {
        bool alive{ true };
        std::mutex mutex;
        std::vector<uint8_t> spare;   // reused render buffer
    };

    namespace
    {
        constexpr int kFadeMs = 900;   // artwork crossfade (Android: tween(900))
        // Frames follow the display refresh (CompositionTarget::Rendering), at most ~60 fps: a
        // timer drifts against vsync (its 15.6 ms granularity turns 33 ms into 31 / 47 ms steps),
        // which shows as judder in such a smooth, slow motion.
        constexpr float kMinFrameSeconds = 0.014f;

        // Renders on the thread pool (a few ms) and presents on the UI thread.
        fire_and_forget RenderAsync(Microsoft::UI::Dispatching::DispatcherQueue queue, std::shared_ptr<ambient::Artwork const> art,
            float seconds, bool dark, ambient::Sizes sizes, uint32_t seed, std::vector<uint8_t> pixels,
            std::function<void(std::vector<uint8_t>&)> done)
        {
            co_await resume_background();
            ambient::Render(*art, seconds, dark, sizes, pixels, seed);
            co_await wil::resume_foreground(queue);
            done(pixels);
        }
    }

    void AmbientBackground::Attach(Grid const& host)
    {
        m_host = host;
        m_shared = std::make_shared<Shared>();
        for (int i = 0; i < 2; ++i)
        {
            Image image;
            image.Stretch(Stretch::Fill);
            image.IsHitTestVisible(false);
            image.Opacity(0);
            image.HorizontalAlignment(HorizontalAlignment::Stretch);
            image.VerticalAlignment(VerticalAlignment::Stretch);
            host.Children().Append(image);
            m_images[i] = image;
        }
        ApplyTransitions();
        auto queue = host.DispatcherQueue();
        m_debounce = queue.CreateTimer();
        m_debounce.IsRepeating(false);
        m_debounce.Interval(std::chrono::milliseconds(120));
        m_debounce.Tick([this, shared = m_shared](auto&&, auto&&) {
            if (shared->alive) Render();
        });
        m_release = queue.CreateTimer();
        m_release.IsRepeating(false);
        m_release.Interval(std::chrono::milliseconds(kFadeMs + 150));
        m_release.Tick([this, shared = m_shared](auto&&, auto&&) {
            if (!shared->alive) return;
            // Drop the bitmap of the slot that has faded out (memory: up to 4 bytes per pixel).
            for (int i = 0; i < 2; ++i)
            {
                if (i == m_front) continue;
                m_images[i].Source(nullptr);
                m_bitmaps[i] = nullptr;
            }
        });
        m_sizeToken = host.SizeChanged([this, shared = m_shared](auto&&, auto&&) {
            if (shared->alive) Schedule(true);
        });
        m_themeToken = host.ActualThemeChanged([this, shared = m_shared](auto&&, auto&&) {
            if (shared->alive) Schedule(false);
        });
        m_loadedToken = host.Loaded([this, shared = m_shared](auto&&, auto&&) {
            if (!shared->alive || m_xamlRoot) return;
            m_xamlRoot = m_host.XamlRoot();
            if (!m_xamlRoot) return;
            // DPI changes (moving to another monitor) need a new pixel size.
            m_rootChangedToken = m_xamlRoot.Changed([this, shared](auto&&, auto&&) {
                if (shared->alive && m_xamlRoot && std::abs(m_xamlRoot.RasterizationScale() - m_scale) > 1e-3) Schedule(true);
            });
            Schedule(false);
        });
    }

    void AmbientBackground::Detach()
    {
        if (!m_shared) return;
        m_shared->alive = false;
        if (m_debounce) m_debounce.Stop();
        if (m_release) m_release.Stop();
        SetFlowing(false);
        if (m_host)
        {
            m_host.SizeChanged(m_sizeToken);
            m_host.ActualThemeChanged(m_themeToken);
            m_host.Loaded(m_loadedToken);
        }
        if (m_xamlRoot) m_xamlRoot.Changed(m_rootChangedToken);
        m_xamlRoot = nullptr;
        for (int i = 0; i < 2; ++i)
        {
            if (m_images[i]) m_images[i].Source(nullptr);
            m_bitmaps[i] = nullptr;
        }
        m_front = -1;
    }

    void AmbientBackground::SetFlowing(bool flowing)
    {
        // The render test captures still frames.
        if (render::Active()) flowing = false;
        if (!m_shared || m_flowing == flowing || (flowing && !m_shared->alive)) return;
        m_flowing = flowing;
        if (!flowing)
        {
            CompositionTarget::Rendering(m_renderingToken);
            return;
        }
        m_lastFrame = std::chrono::steady_clock::now();
        m_renderingToken = CompositionTarget::Rendering([this, shared = m_shared](auto&&, auto&&) {
            if (!shared->alive || m_busy) return;   // the previous frame is still being drawn
            auto now = std::chrono::steady_clock::now();
            float step = std::chrono::duration<float>(now - m_lastFrame).count();
            if (step < kMinFrameSeconds) return;
            // Capped step: coming back from a stall never jumps the scene.
            m_seconds += std::min(step, 0.1f);
            m_lastFrame = now;
            Render();
        });
    }

    void AmbientBackground::SetAnimations(bool enabled)
    {
        if (m_animations == enabled) return;
        m_animations = enabled;
        ApplyTransitions();
    }

    void AmbientBackground::ApplyTransitions()
    {
        for (auto const& image : m_images)
        {
            if (!image) continue;
            if (m_animations)
            {
                ScalarTransition fade;
                fade.Duration(std::chrono::milliseconds(kFadeMs));
                image.OpacityTransition(fade);
            }
            else
            {
                image.OpacityTransition(nullptr);
            }
        }
    }

    void AmbientBackground::SetArtwork(std::shared_ptr<ambient::Artwork const> art)
    {
        if (!m_shared) return;
        if (art == m_art && (m_front >= 0 || !art)) return;
        m_art = std::move(art);
        m_artChanged = true;
        Schedule(false);
    }

    void AmbientBackground::Schedule(bool debounce)
    {
        m_debounce.Stop();
        if (debounce)
        {
            m_debounce.Start();
            return;
        }
        Render();
    }

    void AmbientBackground::FadeOut()
    {
        ++m_generation;   // a render still in flight is stale now
        if (m_front >= 0) m_images[m_front].Opacity(0);
        m_front = -1;
        m_release.Stop();
        m_release.Start();
    }

    void AmbientBackground::Render()
    {
        if (!m_shared || !m_host) return;
        if (!m_art)
        {
            m_artChanged = false;
            FadeOut();
            return;
        }
        if (m_busy)
        {
            m_pending = true;
            return;
        }
        if (auto root = m_host.XamlRoot()) m_scale = root.RasterizationScale();
        int width = static_cast<int>(std::lround(m_host.ActualWidth() * m_scale));
        int height = static_cast<int>(std::lround(m_host.ActualHeight() * m_scale));
        if (width <= 0 || height <= 0) return;   // SizeChanged renders once laid out
        bool dark = m_host.ActualTheme() == ElementTheme::Dark;
        bool crossfade = m_artChanged;
        float seconds = m_seconds;
        if (!crossfade && m_front >= 0 && width == m_renderedWidth && height == m_renderedHeight && dark == m_renderedDark &&
            seconds == m_renderedSeconds)
            return;
        m_artChanged = false;

        auto sizes = ambient::SizesFor(width, height);
        uint64_t generation = ++m_generation;
        auto shared = m_shared;
        std::vector<uint8_t> buffer;
        {
            std::lock_guard lock(shared->mutex);
            buffer = std::move(shared->spare);
        }
        auto done = [this, shared, generation, width, height, sizes, crossfade, dark, seconds](std::vector<uint8_t>& pixels) {
            if (shared->alive)
            {
                m_busy = false;
                if (generation == m_generation)
                {
                    Present(pixels, sizes.outW, sizes.outH, crossfade);
                    m_renderedWidth = width;
                    m_renderedHeight = height;
                    m_renderedDark = dark;
                    m_renderedSeconds = seconds;
                }
            }
            {
                std::lock_guard lock(shared->mutex);
                shared->spare = std::move(pixels);   // reuse the allocation next time
            }
            if (shared->alive && m_pending)
            {
                m_pending = false;
                Render();
            }
        };
        m_busy = true;
        m_pending = false;
        RenderAsync(m_host.DispatcherQueue(), m_art, seconds, dark, sizes, 0x9E3779B9u ^ static_cast<uint32_t>(width * 7919 + height),
            std::move(buffer), std::move(done));
    }

    void AmbientBackground::Present(std::vector<uint8_t> const& pixels, int width, int height, bool crossfade)
    {
        if (pixels.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4) return;
        // Crossfade into the other slot on an artwork change (or when nothing is shown yet); frames,
        // resizes and theme changes replace the current picture in place.
        int target = (crossfade || m_front < 0) ? (m_front < 0 ? 0 : 1 - m_front) : m_front;
        auto& bitmap = m_bitmaps[target];
        bool created = false;
        if (!bitmap || bitmap.PixelWidth() != width || bitmap.PixelHeight() != height)
        {
            bitmap = WriteableBitmap(width, height);
            created = true;
        }
        auto buffer = bitmap.PixelBuffer();
        if (buffer.Capacity() < pixels.size()) return;
        std::memcpy(buffer.data(), pixels.data(), pixels.size());
        bitmap.Invalidate();
        auto const& image = m_images[target];
        if (created || !image.Source()) image.Source(bitmap);
        if (target != m_front)
        {
            Canvas::SetZIndex(image, 1);
            Canvas::SetZIndex(m_images[1 - target], 0);
            if (m_front >= 0) m_images[m_front].Opacity(0);
            image.Opacity(1);
            m_front = target;
            m_release.Stop();
            m_release.Start();
        }
    }
}
