#pragma once

#include "md/crypto/Crypto.h"
#include "md/protocol/Frame.h"

#include <optional>

namespace md::link {

// One direction of AES-256-GCM record protection (spec §5).
class RecordCipher {
public:
    RecordCipher(ByteView key32, ByteView iv4, proto::FrameType type = proto::FrameType::Secure);
    // Encrypts plaintext with the next sequence number; returns ciphertext||tag.
    Bytes seal(ByteView plaintext);
    // Decrypts with the next expected sequence number; nullopt on failure.
    std::optional<Bytes> open(ByteView body);
    uint64_t sequence() const { return seq_; }

private:
    Bytes nonce() const;
    crypto::AesGcm aead_;
    Bytes iv_;
    Bytes aad_;
    uint64_t seq_ = 0;
};

struct Record {
    proto::Channel channel;
    Bytes data;
};

class SecureChannel {
public:
    SecureChannel(ByteView sendKey, ByteView sendIv, ByteView recvKey, ByteView recvIv)
        : send_(sendKey, sendIv), recv_(recvKey, recvIv) {}

    // Returns a full frame (length + type + encrypted body).
    Bytes sealFrame(proto::Channel ch, ByteView data);
    // Body of a SECURE frame -> record; nullopt on any failure.
    std::optional<Record> openFrameBody(ByteView body);

private:
    RecordCipher send_;
    RecordCipher recv_;
};

} // namespace md::link
