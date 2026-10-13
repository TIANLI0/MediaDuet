#pragma once

// Shared plumbing of the host's Direct2D surfaces (desktop lyrics, taskbar widget):
// a premultiplied BGRA DIB with a D2D DC render target that can be rendered
// without any window (offscreen tests, PNG snapshots) and presented to a layered
// window with UpdateLayeredWindow.

#include "md/lyrics/Karaoke.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <winrt/base.h>

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace md::platform {

class LayeredSurface {
public:
    ~LayeredSurface() { release(); }
    // (Re)creates the DIB + render target when the size or DPI changed.
    bool ensure(ID2D1Factory* d2d, SIZE size, UINT dpi);
    void release();
    bool valid() const { return rt_ && memDC_; }
    ID2D1DCRenderTarget* rt() const { return rt_.get(); }
    SIZE size() const { return size_; }
    UINT dpi() const { return dpi_; }
    // Changes whenever the render target is recreated (RT-bound resources such as layers).
    uint64_t generation() const { return gen_; }
    // BindDC + BeginDraw + Clear(transparent).
    void begin();
    // EndDraw; false (surface released) if the target must be recreated.
    bool end();
    // UpdateLayeredWindow; dst = nullptr keeps the current window position.
    bool present(HWND hwnd, const POINT* dst, BYTE alpha) const;
    bool savePng(const std::wstring& path) const;
    // Copies the premultiplied BGRA pixels (top-down rows, stride = width * 4).
    bool copyPixels(std::vector<uint8_t>& out) const;

private:
    winrt::com_ptr<ID2D1DCRenderTarget> rt_;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    void* bits_ = nullptr;
    SIZE size_{0, 0};
    UINT dpi_ = 96;
    uint64_t gen_ = 0;
};

// Writes premultiplied BGRA pixels to a PNG (WIC).
bool writePng(const std::wstring& path, const uint8_t* bgra, int width, int height);

// Process-wide D2D / DWrite factories (single-threaded use on the host thread).
ID2D1Factory* d2dFactory();
IDWriteFactory* dwriteFactory();

// Caret x positions (DIP, layout space) for every UTF-16 position 0..len of a
// layout; the second half of a surrogate pair gets the position of the first.
std::vector<float> caretPositions(IDWriteTextLayout* layout, uint32_t len);
// x of a karaoke fill edge.
float fillX(const std::vector<float>& carets, const lyrics::FillSpan& f);

// Text formats for a family, falling back to Segoe UI when it is missing.
winrt::com_ptr<IDWriteTextFormat> makeFormat(const std::wstring& family, float sizeDip, DWRITE_FONT_WEIGHT weight);

// Outline geometry of a text layout (glyph runs → path) at origin (0,0).
winrt::com_ptr<ID2D1PathGeometry> textGeometry(ID2D1Factory* d2d, IDWriteTextLayout* layout);

// Icon glyphs: Segoe Fluent Icons, Segoe MDL2 Assets on Windows 10.
const wchar_t* iconFontFamily();

double steadyMs();

// One laid-out line of text (no wrapping), with its caret positions and, on
// request, its outline geometry. Layout space: origin at the top-left of the line.
struct TextEntry {
    winrt::com_ptr<IDWriteTextLayout> layout;
    std::vector<float> carets; // UTF-16 positions 0..len
    float width = 0, height = 0;
    winrt::com_ptr<ID2D1PathGeometry> geometry; // only when requested
};

// Caches text layouts (and formats) so per-frame rendering does no DirectWrite
// shaping. Bounded: cleared when it grows past a few dozen entries.
class TextCache {
public:
    const TextEntry* get(const std::wstring& text, const std::wstring& family, float sizeDip, DWRITE_FONT_WEIGHT weight,
                         bool withGeometry = false);
    void clear() { entries_.clear(); }

private:
    using Key = std::tuple<std::wstring, std::wstring, int, int>;
    std::map<Key, TextEntry> entries_;
    std::map<std::tuple<std::wstring, int, int>, winrt::com_ptr<IDWriteTextFormat>> formats_;
};

// Clip to `r` whose left/right edges fade out over `fade` DIPs; keep the returned
// brush alive until PopLayer().
winrt::com_ptr<ID2D1Brush> pushFadeClip(ID2D1RenderTarget* rt, ID2D1Layer* layer, const D2D1_RECT_F& r, bool fadeLeft,
                                        bool fadeRight, float fade);

} // namespace md::platform
