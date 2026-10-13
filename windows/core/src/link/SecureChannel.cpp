#include "md/link/SecureChannel.h"

#include <stdexcept>

namespace md::link {

RecordCipher::RecordCipher(ByteView key32, ByteView iv4, proto::FrameType type)
    : aead_(key32), iv_(iv4.begin(), iv4.end()) {
    if (iv_.size() != 4) throw std::invalid_argument("iv must be 4 bytes");
    aad_ = {'M', 'D', 'v', '1', static_cast<uint8_t>(type)};
}

Bytes RecordCipher::nonce() const {
    Bytes n(12);
    std::copy(iv_.begin(), iv_.end(), n.begin());
    putU64BE(n.data() + 4, seq_);
    return n;
}

Bytes RecordCipher::seal(ByteView plaintext) {
    if (seq_ >= (uint64_t(1) << 63)) throw std::runtime_error("record sequence exhausted");
    Bytes out = aead_.seal(nonce(), aad_, plaintext);
    ++seq_;
    return out;
}

std::optional<Bytes> RecordCipher::open(ByteView body) {
    if (seq_ >= (uint64_t(1) << 63)) return std::nullopt;
    auto pt = aead_.open(nonce(), aad_, body);
    if (pt) ++seq_;
    return pt;
}

Bytes SecureChannel::sealFrame(proto::Channel ch, ByteView data) {
    Bytes pt;
    pt.reserve(1 + data.size());
    pt.push_back(static_cast<uint8_t>(ch));
    append(pt, data);
    return proto::encodeFrame(proto::FrameType::Secure, send_.seal(pt));
}

std::optional<Record> SecureChannel::openFrameBody(ByteView body) {
    auto pt = recv_.open(body);
    if (!pt || pt->empty()) return std::nullopt;
    uint8_t ch = (*pt)[0];
    if (ch != 0x01 && ch != 0x02) return std::nullopt;
    Record r{static_cast<proto::Channel>(ch), Bytes(pt->begin() + 1, pt->end())};
    if (r.channel == proto::Channel::Control && r.data.size() > proto::kMaxControlJson) return std::nullopt;
    return r;
}

} // namespace md::link
