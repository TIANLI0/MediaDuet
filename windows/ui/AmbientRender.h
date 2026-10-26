// Software renderer for the window background (plain C++, no WinRT: unit-tested headless, see
// tests/).
//
// The desktop twin of the Android FlowingArtwork: four copies of the artwork, each turning slowly
// around its own drifting centre at its own pace, saturated and blurred into soft colour fields,
// under a flat scrim (same blobs, pace, saturation, blur radius and scrim colours as Android).
//
// Like Android, the scene is drawn into a canvas at 1/kDownscale of the window size and blurred
// there (cheap: a few thousand pixels). It is then interpolated bilinearly to the output size and
// quantised with triangular-PDF dither noise (+-1 LSB) per channel: an 8-bit copy of such a smooth
// field would show steps ("colour banding"); dithered, the average of every area is exact and the
// remaining error is fine-grained noise. Output is opaque premultiplied BGRA.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mdui::ambient
{
    struct Rgb
    {
        float r{ 0 }, g{ 0 }, b{ 0 };   // 0..1, sRGB
    };

    constexpr int kArtSide = 48;          // the artwork is decoded to this square size
    constexpr int kDownscale = 8;         // scene canvas = window size / kDownscale ...
    constexpr int kMaxSceneSide = 240;    // ... at most this long (the blur makes the rest invisible)
    constexpr float kSaturation = 1.45f;
    constexpr float kStartSeconds = 12.0f;   // not 0: the first (and the paused) frame is already an interesting arrangement
    constexpr int kMaxOutputPixels = 400000;  // output is shown stretched beyond this (the field is very smooth)

    // Prepared artwork: a small square copy (saturated, unpremultiplied RGBA) and its average colour.
    struct Artwork
    {
        int side{ 0 };
        std::vector<float> rgba;
        Rgb ground;   // the ground under the turning copies (never a gap at the edges)
    };

    // From premultiplied BGRA8 (side x side).
    inline Artwork PrepareArtwork(uint8_t const* bgra, int side)
    {
        Artwork art;
        art.side = side;
        art.rgba.resize(static_cast<size_t>(side) * side * 4);
        double sr = 0, sg = 0, sb = 0, sa = 0;
        for (size_t i = 0; i < static_cast<size_t>(side) * side; ++i)
        {
            float a = bgra[i * 4 + 3] / 255.0f;
            float r = bgra[i * 4 + 2] / 255.0f, g = bgra[i * 4 + 1] / 255.0f, b = bgra[i * 4] / 255.0f;
            sr += r; sg += g; sb += b; sa += a;
            if (a > 0) { r /= a; g /= a; b /= a; }
            // ColorMatrix.setToSaturation (Rec. 709 luminance weights), clamped like a colour filter.
            float lum = 0.213f * r + 0.715f * g + 0.072f * b;
            float* dst = &art.rgba[i * 4];
            dst[0] = std::clamp(lum + (r - lum) * kSaturation, 0.0f, 1.0f);
            dst[1] = std::clamp(lum + (g - lum) * kSaturation, 0.0f, 1.0f);
            dst[2] = std::clamp(lum + (b - lum) * kSaturation, 0.0f, 1.0f);
            dst[3] = a;
        }
        if (sa > 0) art.ground = { static_cast<float>(sr / sa), static_cast<float>(sg / sa), static_cast<float>(sb / sa) };
        return art;
    }

    // One turning copy: position (fraction of the canvas), size (of the longer side), pace, phase.
    struct Blob
    {
        float x, y, scale, turnSeconds, orbitSeconds, drift, phase, alpha;
    };

    inline constexpr Blob kBlobs[] = {
        { 0.22f, 0.18f, 1.25f, 70.0f, 41.0f, 0.08f, 0.0f, 1.0f },
        { 0.82f, 0.32f, 1.05f, -55.0f, 37.0f, 0.10f, 1.7f, 0.9f },
        { 0.25f, 0.78f, 1.15f, 85.0f, 47.0f, 0.09f, 3.1f, 0.9f },
        { 0.78f, 0.84f, 0.95f, -64.0f, 33.0f, 0.07f, 4.6f, 0.85f },
    };

    // The flat scrim over the field (Android GlassColors.scrim).
    inline Rgb ScrimColor(bool dark) { return dark ? Rgb{ 0, 0, 0 } : Rgb{ 0xF5 / 255.0f, 0xF5 / 255.0f, 0xF7 / 255.0f }; }
    inline float ScrimAlpha(bool dark) { return dark ? 0.5f : 0.55f; }

    struct Sizes
    {
        int sceneW, sceneH, outW, outH;
    };

    // Scene canvas and output bitmap sizes for a host of w x h physical pixels.
    inline Sizes SizesFor(int w, int h)
    {
        Sizes s{};
        int down = std::max(kDownscale, (std::max(w, h) + kMaxSceneSide - 1) / kMaxSceneSide);
        s.sceneW = std::max(4, (w + down - 1) / down);
        s.sceneH = std::max(4, (h + down - 1) / down);
        double area = static_cast<double>(w) * h;
        int factor = area > kMaxOutputPixels ? static_cast<int>(std::ceil(std::sqrt(area / kMaxOutputPixels))) : 1;
        s.outW = std::max(1, (w + factor - 1) / factor);
        s.outH = std::max(1, (h + factor - 1) / factor);
        return s;
    }

    // The turning copies over the ground, w x h, at time `seconds`.
    inline void DrawScene(Artwork const& art, float seconds, int w, int h, std::vector<Rgb>& out)
    {
        constexpr float kPi = 3.14159265f;
        out.assign(static_cast<size_t>(w) * h, art.ground);
        if (art.side <= 0) return;
        float extent = static_cast<float>(std::max(w, h));
        int n = art.side;
        for (auto const& b : kBlobs)
        {
            float orbit = 2.0f * kPi * seconds / b.orbitSeconds + b.phase;
            float cx = w * b.x + extent * b.drift * std::cos(orbit);
            float cy = h * b.y + extent * b.drift * std::sin(orbit);
            // Sub-pixel placement: the canvas is ~1/8 of the window, so snapping a copy to whole
            // canvas pixels (as Android's IntOffset does at full size) jumps it by ~8 screen pixels.
            float side = std::max(1.0f, extent * b.scale);
            float left = cx - side / 2, top = cy - side / 2;
            float angle = (b.phase * 57.3f + 360.0f * seconds / b.turnSeconds) * kPi / 180.0f;
            float c = std::cos(angle), s = std::sin(angle);
            float reach = side * 0.7072f + 1;
            int y0 = std::max(0, static_cast<int>(cy - reach)), y1 = std::min(h, static_cast<int>(cy + reach) + 1);
            int x0 = std::max(0, static_cast<int>(cx - reach)), x1 = std::min(w, static_cast<int>(cx + reach) + 1);
            float texel = n / side;
            for (int y = y0; y < y1; ++y)
            {
                float dy = y + 0.5f - cy;
                for (int x = x0; x < x1; ++x)
                {
                    // Undo the (clockwise, y down) rotation around the centre.
                    float dx = x + 0.5f - cx;
                    float lx = c * dx + s * dy + cx - left, ly = -s * dx + c * dy + cy - top;
                    if (lx < 0 || ly < 0 || lx >= side || ly >= side) continue;
                    // Bilinear sample, clamped at the edges.
                    float tx = std::clamp(lx * texel - 0.5f, 0.0f, n - 1.0f), ty = std::clamp(ly * texel - 0.5f, 0.0f, n - 1.0f);
                    int ix = static_cast<int>(tx), iy = static_cast<int>(ty);
                    int jx = std::min(ix + 1, n - 1), jy = std::min(iy + 1, n - 1);
                    float fx = tx - ix, fy = ty - iy;
                    float const* p00 = &art.rgba[(static_cast<size_t>(iy) * n + ix) * 4];
                    float const* p01 = &art.rgba[(static_cast<size_t>(iy) * n + jx) * 4];
                    float const* p10 = &art.rgba[(static_cast<size_t>(jy) * n + ix) * 4];
                    float const* p11 = &art.rgba[(static_cast<size_t>(jy) * n + jx) * 4];
                    float v[4];
                    for (int k = 0; k < 4; ++k)
                    {
                        float upper = p00[k] + (p01[k] - p00[k]) * fx, lower = p10[k] + (p11[k] - p10[k]) * fx;
                        v[k] = upper + (lower - upper) * fy;
                    }
                    float a = b.alpha * v[3];
                    Rgb& d = out[static_cast<size_t>(y) * w + x];
                    d.r = v[0] * a + d.r * (1 - a);
                    d.g = v[1] * a + d.g * (1 - a);
                    d.b = v[2] * a + d.b * (1 - a);
                }
            }
        }
    }

    // One box-blur pass along rows (stride 1) or columns (stride w), clamped at the edges.
    inline void BoxPass(std::vector<Rgb> const& src, std::vector<Rgb>& dst, int w, int h, int radius, bool horizontal)
    {
        int lines = horizontal ? h : w, len = horizontal ? w : h;
        size_t step = horizontal ? 1 : static_cast<size_t>(w);
        float scale = 1.0f / (2 * radius + 1);
        for (int line = 0; line < lines; ++line)
        {
            size_t base = horizontal ? static_cast<size_t>(line) * w : static_cast<size_t>(line);
            auto at = [&](int i) -> Rgb const& { return src[base + static_cast<size_t>(std::clamp(i, 0, len - 1)) * step]; };
            float r = 0, g = 0, b = 0;
            for (int i = -radius; i <= radius; ++i)
            {
                r += at(i).r; g += at(i).g; b += at(i).b;
            }
            for (int i = 0; i < len; ++i)
            {
                dst[base + static_cast<size_t>(i) * step] = { r * scale, g * scale, b * scale };
                Rgb const& in = at(i + radius + 1);
                Rgb const& out = at(i - radius);
                r += in.r - out.r; g += in.g - out.g; b += in.b - out.b;
            }
        }
    }

    // Gaussian blur (three box passes per axis), clamped at the edges.
    inline void Blur(std::vector<Rgb>& px, int w, int h, float sigma)
    {
        // Box sizes whose sum of variances matches sigma (W. Jarosz, "Fast image convolutions").
        constexpr int kPasses = 3;
        float ideal = std::sqrt(12 * sigma * sigma / kPasses + 1);
        int lower = static_cast<int>(std::floor(ideal));
        if (lower % 2 == 0) --lower;
        lower = std::max(lower, 1);
        int upper = lower + 2;
        int m = static_cast<int>(std::lround((12 * sigma * sigma - kPasses * lower * lower - 4 * kPasses * lower - 3 * kPasses) / (-4.0f * lower - 4)));
        std::vector<Rgb> tmp(px.size());
        for (int pass = 0; pass < kPasses; ++pass)
        {
            int radius = ((pass < m ? lower : upper) - 1) / 2;
            if (radius <= 0) continue;
            BoxPass(px, tmp, w, h, radius, true);
            BoxPass(tmp, px, w, h, radius, false);
        }
    }

    // xorshift32: cheap, good enough for dither noise.
    struct Noise
    {
        uint32_t s;
        uint32_t Next()
        {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            return s;
        }
    };

    // Bilinear upsampling of the scene (sw x sh) to w x h opaque BGRA8 with triangular-PDF dither.
    inline void Upsample(std::vector<Rgb> const& scene, int sw, int sh, int w, int h, std::vector<uint8_t>& out, uint32_t seed)
    {
        out.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        struct Tap { int i0, i1; float f; };
        auto taps = [](int n, int sn) {
            std::vector<Tap> t(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i)
            {
                float s = std::clamp((i + 0.5f) * sn / n - 0.5f, 0.0f, sn - 1.0f);
                int i0 = static_cast<int>(s);
                t[i] = { i0, std::min(i0 + 1, sn - 1), s - i0 };
            }
            return t;
        };
        auto xs = taps(w, sw), ys = taps(h, sh);
        Noise noise{ seed ? seed : 1u };
        std::vector<Rgb> row(static_cast<size_t>(sw));
        for (int y = 0; y < h; ++y)
        {
            Tap const& ty = ys[y];
            Rgb const* r0 = &scene[static_cast<size_t>(ty.i0) * sw];
            Rgb const* r1 = &scene[static_cast<size_t>(ty.i1) * sw];
            for (int x = 0; x < sw; ++x)
                row[x] = { (r0[x].r + (r1[x].r - r0[x].r) * ty.f) * 255.0f, (r0[x].g + (r1[x].g - r0[x].g) * ty.f) * 255.0f,
                    (r0[x].b + (r1[x].b - r0[x].b) * ty.f) * 255.0f };
            uint8_t* dst = &out[static_cast<size_t>(y) * w * 4];
            for (int x = 0; x < w; ++x)
            {
                Tap const& tx = xs[x];
                Rgb const& c0 = row[tx.i0];
                Rgb const& c1 = row[tx.i1];
                float val[3] = { c0.b + (c1.b - c0.b) * tx.f, c0.g + (c1.g - c0.g) * tx.f, c0.r + (c1.r - c0.r) * tx.f };
                // Triangular PDF noise in (-1, 1) LSB: the sum of two uniform variables.
                uint32_t n0 = noise.Next(), n1 = noise.Next();
                auto tpdf = [](uint32_t bits) {
                    return (static_cast<float>(bits & 0xFF) + static_cast<float>((bits >> 8) & 0xFF)) * (1.0f / 255.0f) - 1.0f;
                };
                float d[3] = { tpdf(n0), tpdf(n0 >> 16), tpdf(n1) };
                for (int k = 0; k < 3; ++k)
                    dst[k] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::floor(val[k] + 0.5f + d[k])), 0, 255));
                dst[3] = 255;
                dst += 4;
            }
        }
    }

    // Renders the frame at `seconds` for a host of the given sizes (see SizesFor) into out
    // (outW x outH opaque BGRA8; capacity is reused).
    inline void Render(Artwork const& art, float seconds, bool dark, Sizes const& sizes, std::vector<uint8_t>& out,
        uint32_t seed = 0x9E3779B9u)
    {
        if (sizes.sceneW <= 0 || sizes.sceneH <= 0 || sizes.outW <= 0 || sizes.outH <= 0)
        {
            out.clear();
            return;
        }
        std::vector<Rgb> scene;
        DrawScene(art, seconds, sizes.sceneW, sizes.sceneH, scene);
        // Android BlurEffect radius (0.16 x the shorter side) as Skia converts it to a sigma.
        float radius = std::min(sizes.sceneW, sizes.sceneH) * 0.16f;
        Blur(scene, sizes.sceneW, sizes.sceneH, 0.57735f * radius + 0.5f);
        Rgb scrim = ScrimColor(dark);
        float a = ScrimAlpha(dark);
        for (auto& p : scene)
        {
            p.r = scrim.r * a + p.r * (1 - a);
            p.g = scrim.g * a + p.g * (1 - a);
            p.b = scrim.b * a + p.b * (1 - a);
        }
        Upsample(scene, sizes.sceneW, sizes.sceneH, sizes.outW, sizes.outH, out, seed);
    }
}
