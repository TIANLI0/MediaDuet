// Software renderer for the artwork-tinted window background (plain C++, no WinRT: unit-tested
// headless, see tests/).
//
// An 8-bit linear gradient that fades a low-alpha colour over ~1500 px has only a few dozen distinct
// values, so the steps between them are visible as bands ("colour banding"). The field is therefore
// computed in float and quantised with triangular-PDF dither noise (+-1 LSB) per channel: the
// average of every area is exact and the remaining error is fine-grained noise instead of steps.
// The result is premultiplied BGRA at the element's physical pixel size, shown 1:1 (any rescaling
// would re-introduce bands).
//
// The field is evaluated on a coarse grid (every kCell px) and interpolated bilinearly: the shapes
// are very smooth, so this is exact to far below 1 LSB and keeps a 4K render at a few tens of ms.
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

    enum class Shape
    {
        Main,      // diagonal wash from the top-left + soft colour fields (Apple-Music-like, subtle)
        Compact,   // horizontal wash from the left + one soft field at the right
    };

    struct Params
    {
        Shape shape{ Shape::Main };
        Rgb colors[3]{};    // primary, secondary, tertiary (dominant artwork colours)
        int colorCount{ 0 };
        float alpha{ 0.5f };   // strength of the primary wash
    };

    constexpr int kCell = 8;

    inline float SmoothFalloff(float s)   // 1 at s <= 0, 0 at s >= 1, zero slope at both ends
    {
        s = std::clamp(s, 0.0f, 1.0f);
        return 1.0f - s * s * (3.0f - 2.0f * s);
    }

    // Premultiplied RGBA (float) of the field at normalised coordinates (u, v in 0..1); aspect = w/h.
    inline void Field(Params const& p, float u, float v, float aspect, float out[4])
    {
        float r = 0, g = 0, b = 0, a = 0;
        auto over = [&](Rgb const& c, float alpha) {
            alpha = std::clamp(alpha, 0.0f, 1.0f);
            r = c.r * alpha + r * (1 - alpha);
            g = c.g * alpha + g * (1 - alpha);
            b = c.b * alpha + b * (1 - alpha);
            a = alpha + a * (1 - alpha);
        };
        // Gaussian blob in a space where distances are measured relative to the longer side.
        auto blob = [&](float cu, float cv, float radius) {
            float du = (u - cu), dv = (v - cv);
            if (aspect >= 1) dv /= aspect;
            else du *= aspect;
            float d2 = (du * du + dv * dv) / (radius * radius);
            return std::exp(-d2);
        };
        if (p.colorCount <= 0) return void(out[0] = out[1] = out[2] = out[3] = 0);
        if (p.shape == Shape::Main)
        {
            if (p.colorCount >= 3) over(p.colors[2], p.alpha * 0.32f * blob(0.62f, 0.0f, 0.42f));
            if (p.colorCount >= 2) over(p.colors[1], p.alpha * 0.46f * blob(0.96f, 1.0f, 0.62f));
            over(p.colors[0], p.alpha * SmoothFalloff(((u + v) * 0.5f) / 0.85f));
        }
        else
        {
            if (p.colorCount >= 2) over(p.colors[1], p.alpha * 0.40f * blob(1.0f, 1.0f, 0.55f));
            over(p.colors[0], p.alpha * SmoothFalloff(u / 1.0f));
        }
        out[0] = r;
        out[1] = g;
        out[2] = b;
        out[3] = a;
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

    // Renders w x h premultiplied BGRA8 into out (resized as needed; capacity is reused).
    inline void Render(Params const& p, int w, int h, std::vector<uint8_t>& out, uint32_t seed = 0x9E3779B9u)
    {
        if (w <= 0 || h <= 0)
        {
            out.clear();
            return;
        }
        out.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        // Coarse grid: gw x gh samples covering [0, w] x [0, h].
        int gw = w / kCell + 2, gh = h / kCell + 2;
        std::vector<float> grid(static_cast<size_t>(gw) * gh * 4);
        float aspect = static_cast<float>(w) / static_cast<float>(h);
        for (int gy = 0; gy < gh; ++gy)
        {
            float v = std::min(1.0f, (gy * kCell + 0.5f) / static_cast<float>(h));
            for (int gx = 0; gx < gw; ++gx)
            {
                float u = std::min(1.0f, (gx * kCell + 0.5f) / static_cast<float>(w));
                Field(p, u, v, aspect, &grid[(static_cast<size_t>(gy) * gw + gx) * 4]);
            }
        }
        Noise noise{ seed ? seed : 1u };
        std::vector<float> row(static_cast<size_t>(gw) * 4);
        for (int y = 0; y < h; ++y)
        {
            int gy = y / kCell;
            float fy = static_cast<float>(y % kCell) / kCell;
            float const* r0 = &grid[static_cast<size_t>(gy) * gw * 4];
            float const* r1 = &grid[static_cast<size_t>(std::min(gy + 1, gh - 1)) * gw * 4];
            for (int i = 0; i < gw * 4; ++i) row[i] = (r0[i] + (r1[i] - r0[i]) * fy) * 255.0f;
            uint8_t* dst = &out[static_cast<size_t>(y) * w * 4];
            for (int x = 0; x < w; ++x)
            {
                int gx = x / kCell;
                float fx = static_cast<float>(x % kCell) / kCell;
                float const* c0 = &row[static_cast<size_t>(gx) * 4];
                float const* c1 = &row[static_cast<size_t>(std::min(gx + 1, gw - 1)) * 4];
                float val[4];
                for (int k = 0; k < 4; ++k) val[k] = c0[k] + (c1[k] - c0[k]) * fx;
                // Triangular PDF noise in (-1, 1) LSB: the sum of two uniform variables.
                uint32_t n0 = noise.Next(), n1 = noise.Next();
                auto tpdf = [](uint32_t bits) {
                    return (static_cast<float>(bits & 0xFF) + static_cast<float>((bits >> 8) & 0xFF)) * (1.0f / 255.0f) - 1.0f;
                };
                float d[4] = { tpdf(n0), tpdf(n0 >> 16), tpdf(n1), tpdf(n1 >> 16) };
                int q[4];
                for (int k = 0; k < 4; ++k)
                    q[k] = val[k] <= 0.0f ? 0 : std::clamp(static_cast<int>(std::floor(val[k] + 0.5f + d[k])), 0, 255);
                int a = q[3];
                // Premultiplied: colour <= alpha. BGRA order.
                dst[0] = static_cast<uint8_t>(std::min(q[2], a));
                dst[1] = static_cast<uint8_t>(std::min(q[1], a));
                dst[2] = static_cast<uint8_t>(std::min(q[0], a));
                dst[3] = static_cast<uint8_t>(a);
                dst += 4;
            }
        }
    }
}
