#include "OverlayCommon.h"

#include "md/lyrics/LyricsClock.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>

namespace md::platform {

using winrt::com_ptr;

// ------------------------------------------------------------------ surface

bool LayeredSurface::ensure(ID2D1Factory* d2d, SIZE sz, UINT dpi) {
    if (!d2d || sz.cx <= 0 || sz.cy <= 0) return false;
    if (valid() && size_.cx == sz.cx && size_.cy == sz.cy && dpi_ == dpi) return true;
    release();
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = sz.cx;
    bi.bmiHeader.biHeight = -sz.cy;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    // A memory DC is enough: no screen DC (and no window) is involved.
    memDC_ = CreateCompatibleDC(nullptr);
    dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (!memDC_ || !dib_) {
        release();
        return false;
    }
    oldBmp_ = SelectObject(memDC_, dib_);
    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 0,
        0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
    if (FAILED(d2d->CreateDCRenderTarget(&props, rt_.put()))) {
        release();
        return false;
    }
    rt_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    size_ = sz;
    dpi_ = dpi;
    ++gen_;
    return true;
}

void LayeredSurface::release() {
    rt_ = nullptr;
    if (memDC_) {
        if (oldBmp_) SelectObject(memDC_, oldBmp_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    oldBmp_ = nullptr;
    if (dib_) {
        DeleteObject(dib_);
        dib_ = nullptr;
    }
    bits_ = nullptr;
    size_ = {0, 0};
}

void LayeredSurface::begin() {
    RECT rc{0, 0, size_.cx, size_.cy};
    rt_->BindDC(memDC_, &rc);
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
}

bool LayeredSurface::end() {
    HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        release();
        return false;
    }
    GdiFlush();
    return SUCCEEDED(hr);
}

bool LayeredSurface::present(HWND hwnd, const POINT* dst, BYTE alpha) const {
    if (!hwnd || !valid()) return false;
    POINT src{0, 0};
    SIZE sz = size_;
    BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    // hdcDst = nullptr: the default palette is used; no screen DC round trip needed.
    return UpdateLayeredWindow(hwnd, nullptr, const_cast<POINT*>(dst), &sz, memDC_, &src, 0, &bf, ULW_ALPHA) != FALSE;
}

bool LayeredSurface::copyPixels(std::vector<uint8_t>& out) const {
    if (!bits_ || size_.cx <= 0) return false;
    GdiFlush();
    size_t n = static_cast<size_t>(size_.cx) * size_.cy * 4;
    out.assign(static_cast<const uint8_t*>(bits_), static_cast<const uint8_t*>(bits_) + n);
    return true;
}

bool LayeredSurface::savePng(const std::wstring& path) const {
    if (!bits_ || size_.cx <= 0) return false;
    GdiFlush();
    return writePng(path, static_cast<const uint8_t*>(bits_), size_.cx, size_.cy);
}

bool writePng(const std::wstring& path, const uint8_t* bgra, int w, int h) {
    com_ptr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.put()))))
        return false;
    com_ptr<IWICBitmap> bmp;
    UINT stride = static_cast<UINT>(w) * 4;
    if (FAILED(f->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, stride, stride * h,
                                         const_cast<BYTE*>(bgra), bmp.put())))
        return false;
    com_ptr<IWICStream> stream;
    com_ptr<IWICBitmapEncoder> enc;
    com_ptr<IWICBitmapFrameEncode> frame;
    if (FAILED(f->CreateStream(stream.put())) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) ||
        FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(frame.put(), nullptr)) || FAILED(frame->Initialize(nullptr)))
        return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppPBGRA;
    frame->SetSize(w, h);
    frame->SetPixelFormat(&fmt);
    return SUCCEEDED(frame->WriteSource(bmp.get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

// ------------------------------------------------------------------ factories

ID2D1Factory* d2dFactory() {
    static com_ptr<ID2D1Factory> f = [] {
        com_ptr<ID2D1Factory> x;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, x.put());
        return x;
    }();
    return f.get();
}

IDWriteFactory* dwriteFactory() {
    static com_ptr<IDWriteFactory> f = [] {
        com_ptr<IDWriteFactory> x;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(x.put()));
        return x;
    }();
    return f.get();
}

com_ptr<IDWriteTextFormat> makeFormat(const std::wstring& family, float sizeDip, DWRITE_FONT_WEIGHT weight) {
    com_ptr<IDWriteTextFormat> fmt;
    auto* dw = dwriteFactory();
    if (!dw) return fmt;
    // CreateTextFormat succeeds for unknown families (fallback happens at layout
    // time), so check the system collection first.
    com_ptr<IDWriteFontCollection> sys;
    dw->GetSystemFontCollection(sys.put());
    UINT32 idx = 0;
    BOOL exists = FALSE;
    if (sys) sys->FindFamilyName(family.c_str(), &idx, &exists);
    const wchar_t* name = exists ? family.c_str() : L"Segoe UI";
    if (FAILED(dw->CreateTextFormat(name, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, sizeDip,
                                    L"", fmt.put())))
        return nullptr;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    return fmt;
}

const wchar_t* iconFontFamily() {
    static const wchar_t* name = [] {
        com_ptr<IDWriteFontCollection> sys;
        if (auto* dw = dwriteFactory()) dw->GetSystemFontCollection(sys.put());
        UINT32 idx = 0;
        BOOL exists = FALSE;
        if (sys) sys->FindFamilyName(L"Segoe Fluent Icons", &idx, &exists);
        return exists ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }();
    return name;
}

double steadyMs() { return lyrics::steadyNowMs(); }

// ------------------------------------------------------------------ text helpers

std::vector<float> caretPositions(IDWriteTextLayout* l, uint32_t len) {
    std::vector<float> out(len + 1, 0.f);
    if (!l) return out;
    // Leading edge of each character; the trailing edge of the last one for len.
    for (uint32_t i = 0; i <= len; ++i) {
        FLOAT x = 0, y = 0;
        DWRITE_HIT_TEST_METRICS m{};
        if (i < len) l->HitTestTextPosition(i, FALSE, &x, &y, &m);
        else if (len > 0) l->HitTestTextPosition(len - 1, TRUE, &x, &y, &m);
        out[i] = x;
    }
    // A low surrogate is not a caret stop: give it its pair's position.
    return out;
}

float fillX(const std::vector<float>& c, const lyrics::FillSpan& f) {
    if (c.empty()) return 0.f;
    size_t n = c.size() - 1;
    float x0 = c[std::min<size_t>(f.from, n)], x1 = c[std::min<size_t>(f.to, n)];
    return x0 + (x1 - x0) * static_cast<float>(std::clamp(f.frac, 0.0, 1.0));
}

namespace {

// Collects the glyph outlines of a layout into one geometry group.
struct OutlineRenderer : winrt::implements<OutlineRenderer, IDWriteTextRenderer> {
    ID2D1Factory* d2d;
    std::vector<com_ptr<ID2D1Geometry>> parts;
    explicit OutlineRenderer(ID2D1Factory* f) : d2d(f) {}

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* out) override {
        *out = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override {
        *m = DWRITE_MATRIX{1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* p) override {
        *p = 1.f;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        com_ptr<ID2D1PathGeometry> path;
        if (FAILED(d2d->CreatePathGeometry(path.put()))) return S_OK;
        com_ptr<ID2D1GeometrySink> sink;
        if (FAILED(path->Open(sink.put()))) return S_OK;
        sink->SetFillMode(D2D1_FILL_MODE_WINDING);
        run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances, run->glyphOffsets,
                                          run->glyphCount, run->isSideways, run->bidiLevel & 1, sink.get());
        sink->Close();
        com_ptr<ID2D1TransformedGeometry> moved;
        if (SUCCEEDED(d2d->CreateTransformedGeometry(path.get(), D2D1::Matrix3x2F::Translation(x, y), moved.put())))
            parts.push_back(moved);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override {
        return S_OK;
    }
};

} // namespace

com_ptr<ID2D1PathGeometry> textGeometry(ID2D1Factory* d2d, IDWriteTextLayout* layout) {
    com_ptr<ID2D1PathGeometry> out;
    if (!d2d || !layout) return out;
    auto r = winrt::make_self<OutlineRenderer>(d2d);
    if (FAILED(layout->Draw(nullptr, r.get(), 0, 0))) return out;
    // Flatten the runs into one path so stroke / fill / shadow are single draws.
    if (FAILED(d2d->CreatePathGeometry(out.put()))) return nullptr;
    com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(out->Open(sink.put()))) return nullptr;
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    if (!r->parts.empty()) {
        std::vector<ID2D1Geometry*> raw;
        for (auto& p : r->parts) raw.push_back(p.get());
        com_ptr<ID2D1GeometryGroup> group;
        if (SUCCEEDED(d2d->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, raw.data(), static_cast<UINT32>(raw.size()),
                                               group.put())))
            group->Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION_CUBICS_AND_LINES, nullptr, sink.get());
    }
    sink->Close();
    return out;
}

const TextEntry* TextCache::get(const std::wstring& text, const std::wstring& family, float sizeDip,
                                DWRITE_FONT_WEIGHT weight, bool withGeometry) {
    int sizeKey = static_cast<int>(std::lround(sizeDip * 16));
    Key key{text, family, sizeKey, static_cast<int>(weight)};
    auto it = entries_.find(key);
    if (it != entries_.end() && (!withGeometry || it->second.geometry)) return &it->second;
    if (it == entries_.end()) {
        if (entries_.size() > 48) entries_.clear();
        auto fk = std::make_tuple(family, sizeKey, static_cast<int>(weight));
        auto& fmt = formats_[fk];
        if (!fmt) fmt = makeFormat(family, sizeDip, weight);
        if (!fmt) return nullptr;
        if (formats_.size() > 16) {
            auto keep = fmt;
            formats_.clear();
            formats_[fk] = keep;
        }
        TextEntry e;
        auto* dw = dwriteFactory();
        if (!dw || FAILED(dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), formats_[fk].get(), 100000.f,
                                               sizeDip * 3, e.layout.put())))
            return nullptr;
        DWRITE_TEXT_METRICS m{};
        e.layout->GetMetrics(&m);
        e.width = m.widthIncludingTrailingWhitespace;
        e.height = m.height;
        e.carets = caretPositions(e.layout.get(), static_cast<uint32_t>(text.size()));
        it = entries_.emplace(key, std::move(e)).first;
    }
    if (withGeometry && !it->second.geometry) it->second.geometry = textGeometry(d2dFactory(), it->second.layout.get());
    return &it->second;
}

com_ptr<ID2D1Brush> pushFadeClip(ID2D1RenderTarget* rt, ID2D1Layer* layer, const D2D1_RECT_F& r, bool fadeLeft,
                                 bool fadeRight, float fade) {
    float w = std::max(1.f, r.right - r.left);
    float f = std::clamp(fade / w, 0.f, 0.45f);
    D2D1_GRADIENT_STOP stops[] = {{0.f, D2D1::ColorF(0, 0, 0, fadeLeft ? 0.f : 1.f)},
                                  {f, D2D1::ColorF(0, 0, 0, 1)},
                                  {1.f - f, D2D1::ColorF(0, 0, 0, 1)},
                                  {1.f, D2D1::ColorF(0, 0, 0, fadeRight ? 0.f : 1.f)}};
    com_ptr<ID2D1GradientStopCollection> coll;
    com_ptr<ID2D1LinearGradientBrush> brush;
    if (layer && (fadeLeft || fadeRight) &&
        SUCCEEDED(rt->CreateGradientStopCollection(stops, 4, coll.put())) &&
        SUCCEEDED(rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, 0), D2D1::Point2F(r.right, 0)), coll.get(),
            brush.put()))) {
        rt->PushLayer(D2D1::LayerParameters(r, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.f,
                                            brush.get()),
                      layer);
        return brush;
    }
    // No fade needed: a plain layer keeps Push/Pop symmetric for the caller.
    rt->PushLayer(D2D1::LayerParameters(r), layer);
    return nullptr;
}

} // namespace md::platform
