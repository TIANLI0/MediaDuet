#include "md/protocol/Frame.h"

#include <stdexcept>

namespace md::proto {

Bytes encodeFrame(FrameType type, ByteView body) {
    if (body.size() + 1 > kMaxFrameLength) throw std::length_error("frame too large");
    Bytes out(5 + body.size());
    putU32BE(out.data(), static_cast<uint32_t>(body.size() + 1));
    out[4] = static_cast<uint8_t>(type);
    std::copy(body.begin(), body.end(), out.begin() + 5);
    return out;
}

void FrameDecoder::feed(ByteView data) {
    if (failed_) return;
    // Compact consumed prefix before growing.
    if (pos_ > 0 && pos_ >= buf_.size() / 2) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(pos_));
        pos_ = 0;
    }
    buf_.insert(buf_.end(), data.begin(), data.end());
}

std::optional<Frame> FrameDecoder::next() {
    if (failed_) return std::nullopt;
    size_t avail = buf_.size() - pos_;
    if (avail < 4) return std::nullopt;
    uint32_t len = getU32BE(buf_.data() + pos_);
    if (len < 1 || len > max_) {
        fail("frame length out of range");
        return std::nullopt;
    }
    if (avail < 4 + static_cast<size_t>(len)) return std::nullopt;
    uint8_t t = buf_[pos_ + 4];
    if (t != 0x01 && t != 0x02 && t != 0x03) {
        fail("unknown frame type");
        return std::nullopt;
    }
    if (t == 0x01 && len - 1 > kMaxHandshakeBody) {
        fail("handshake frame too large");
        return std::nullopt;
    }
    Frame f{static_cast<FrameType>(t),
            Bytes(buf_.begin() + static_cast<ptrdiff_t>(pos_ + 5), buf_.begin() + static_cast<ptrdiff_t>(pos_ + 4 + len))};
    pos_ += 4 + len;
    if (pos_ == buf_.size()) {
        buf_.clear();
        pos_ = 0;
    }
    return f;
}

Bytes encodeBlobChunk(const BlobChunk& c) {
    if (c.hash.size() != 32 || c.data.size() > kBlobChunkMax) throw std::invalid_argument("bad blob chunk");
    Bytes out(kBlobHeader + c.data.size());
    std::copy(c.hash.begin(), c.hash.end(), out.begin());
    putU32BE(out.data() + 32, c.total);
    putU32BE(out.data() + 36, c.offset);
    std::copy(c.data.begin(), c.data.end(), out.begin() + kBlobHeader);
    return out;
}

std::optional<BlobChunk> decodeBlobChunk(ByteView d) {
    if (d.size() < kBlobHeader || d.size() - kBlobHeader > kBlobChunkMax) return std::nullopt;
    BlobChunk c;
    c.hash.assign(d.begin(), d.begin() + 32);
    c.total = getU32BE(d.data() + 32);
    c.offset = getU32BE(d.data() + 36);
    c.data.assign(d.begin() + kBlobHeader, d.end());
    if (c.total > kMaxArtworkBytes || c.offset > c.total || c.data.size() > c.total - c.offset) return std::nullopt;
    return c;
}

} // namespace md::proto
