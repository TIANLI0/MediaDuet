#include "Artwork.h"

#include "md/crypto/Crypto.h"
#include "md/util/Log.h"

#include <windows.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <winrt/base.h>

#include <algorithm>
#include <fstream>
#include <vector>

namespace md::platform {

namespace fs = std::filesystem;
using winrt::com_ptr;

namespace {
constexpr size_t kMaxInput = 8 * 1024 * 1024;

struct ComInit {
    HRESULT hr;
    ComInit() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComInit() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

com_ptr<IWICImagingFactory> wicFactory() {
    com_ptr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.put()))))
        return nullptr;
    return f;
}

com_ptr<IWICBitmapFrameDecode> decodeFrame(IWICImagingFactory* f, ByteView raw, UINT& w, UINT& h) {
    com_ptr<IStream> in;
    in.attach(SHCreateMemStream(raw.data(), static_cast<UINT>(raw.size())));
    if (!in) return nullptr;
    com_ptr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(in.get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.put())))
        return nullptr;
    com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, frame.put()))) return nullptr;
    w = h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0 || w > 20000 || h > 20000) return nullptr;
    return frame;
}

std::optional<Bytes> encodeJpeg(IWICImagingFactory* f, IWICBitmapSource* src, UINT w, UINT h, float quality) {
    com_ptr<IStream> out;
    out.attach(SHCreateMemStream(nullptr, 0));
    if (!out) return std::nullopt;
    com_ptr<IWICBitmapEncoder> enc;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, enc.put())) ||
        FAILED(enc->Initialize(out.get(), WICBitmapEncoderNoCache)))
        return std::nullopt;
    com_ptr<IWICBitmapFrameEncode> fe;
    com_ptr<IPropertyBag2> bag;
    if (FAILED(enc->CreateNewFrame(fe.put(), bag.put()))) return std::nullopt;
    PROPBAG2 opt{};
    opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_R4;
    v.fltVal = quality;
    bag->Write(1, &opt, &v);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(fe->Initialize(bag.get())) || FAILED(fe->SetSize(w, h)) || FAILED(fe->SetPixelFormat(&fmt)) ||
        FAILED(fe->WriteSource(src, nullptr)) || FAILED(fe->Commit()) || FAILED(enc->Commit()))
        return std::nullopt;
    STATSTG st{};
    if (FAILED(out->Stat(&st, STATFLAG_NONAME))) return std::nullopt;
    Bytes data(static_cast<size_t>(st.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    out->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    out->Read(data.data(), static_cast<ULONG>(data.size()), &read);
    data.resize(read);
    return data;
}

struct Transcoded {
    Bytes jpeg;
    media::ImageSize source, stored;
};

// Decodes, scales the longer side to at most maxSide and encodes as JPEG within the
// protocol's artwork size limit (stepping the quality down if needed).
std::optional<Transcoded> transcode(ByteView raw, uint32_t maxSide) {
    ComInit com;
    auto f = wicFactory();
    if (!f) return std::nullopt;
    UINT w = 0, h = 0;
    auto frame = decodeFrame(f.get(), raw, w, h);
    if (!frame) return std::nullopt;
    com_ptr<IWICBitmapSource> src = frame;
    double scale = std::min(1.0, static_cast<double>(std::max<uint32_t>(maxSide, 1)) / std::max(w, h));
    UINT tw = std::max(1u, static_cast<UINT>(w * scale)), th = std::max(1u, static_cast<UINT>(h * scale));
    if (scale < 1.0) {
        com_ptr<IWICBitmapScaler> sc;
        if (FAILED(f->CreateBitmapScaler(sc.put())) ||
            FAILED(sc->Initialize(src.get(), tw, th, WICBitmapInterpolationModeHighQualityCubic)))
            return std::nullopt;
        src = sc;
    }
    com_ptr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(src.get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return std::nullopt;
    // Materialise once so a quality retry does not decode and scale again.
    com_ptr<IWICBitmap> pixels;
    if (FAILED(f->CreateBitmapFromSource(conv.get(), WICBitmapCacheOnLoad, pixels.put()))) return std::nullopt;
    for (float q : {0.86f, 0.75f, 0.6f, 0.45f}) {
        auto data = encodeJpeg(f.get(), pixels.get(), tw, th, q);
        if (!data || data->empty()) return std::nullopt;
        if (data->size() <= media::artwork::kMaxBytes)
            return Transcoded{std::move(*data), {w, h}, {tw, th}};
    }
    return std::nullopt;
}

bool writeFile(const fs::path& p, ByteView data) {
    auto tmp = p;
    tmp += L".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!o) return false;
    }
    return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

} // namespace

ArtworkCache::ArtworkCache(fs::path dir) : dir_(std::move(dir)) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
}

fs::path ArtworkCache::pathFor(const std::string& hash) const { return dir_ / (std::wstring(hash.begin(), hash.end()) + L".jpg"); }

bool ArtworkCache::has(const std::string& hash) const {
    std::error_code ec;
    return hash.size() == 64 && fs::exists(pathFor(hash), ec);
}

std::optional<Bytes> ArtworkCache::read(const std::string& hash) const {
    if (hash.size() != 64) return std::nullopt;
    std::ifstream in(pathFor(hash), std::ios::binary);
    if (!in) return std::nullopt;
    Bytes b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return b;
}

std::optional<media::ArtworkRef> ArtworkCache::ingest(ByteView raw) {
    auto r = ingestImage(raw);
    if (!r) return std::nullopt;
    return r->ref;
}

std::optional<ArtworkCache::Ingested> ArtworkCache::ingestImage(ByteView raw, uint32_t maxSide) {
    if (raw.empty() || raw.size() > kMaxInput) return std::nullopt;
    std::string rawKey = hexEncode(crypto::sha256(raw)) + ":" + std::to_string(maxSide);
    {
        std::lock_guard lock(m_);
        if (auto it = rawToRef_.find(rawKey); it != rawToRef_.end() && has(it->second.ref.hash)) return it->second;
    }
    auto t = transcode(raw, maxSide);
    if (!t || t->jpeg.empty() || t->jpeg.size() > media::artwork::kMaxBytes) return std::nullopt;
    Ingested out{{hexEncode(crypto::sha256(t->jpeg)), "image/jpeg", static_cast<uint32_t>(t->jpeg.size())},
                 t->source,
                 t->stored};
    if (!has(out.ref.hash) && !writeFile(pathFor(out.ref.hash), t->jpeg)) return std::nullopt;
    std::lock_guard lock(m_);
    if (rawToRef_.size() > 256) rawToRef_.clear();
    rawToRef_[rawKey] = out;
    return out;
}

std::optional<media::ImageSize> ArtworkCache::probe(ByteView raw) {
    if (raw.empty() || raw.size() > kMaxInput) return std::nullopt;
    ComInit com;
    auto f = wicFactory();
    if (!f) return std::nullopt;
    UINT w = 0, h = 0;
    if (!decodeFrame(f.get(), raw, w, h)) return std::nullopt;
    return media::ImageSize{w, h};
}

void ArtworkCache::touch(const std::string& hash) {
    if (hash.size() != 64) return;
    std::error_code ec;
    fs::last_write_time(pathFor(hash), fs::file_time_type::clock::now(), ec);
}

bool ArtworkCache::store(const std::string& hash, ByteView data) {
    if (hash.size() != 64 || data.size() > media::artwork::kMaxBytes) return false;
    if (hexEncode(crypto::sha256(data)) != hash) {
        log::warn("art", "received artwork failed hash verification");
        return false;
    }
    if (has(hash)) return true;
    return writeFile(pathFor(hash), data);
}

void ArtworkCache::prune(size_t maxFiles) {
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir_, ec))
        if (e.is_regular_file(ec)) files.push_back({e.last_write_time(ec), e.path()});
    if (files.size() <= maxFiles) return;
    std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t i = maxFiles; i < files.size(); ++i) fs::remove(files[i].second, ec);
}

} // namespace md::platform
