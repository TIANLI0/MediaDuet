#pragma once

#include "md/util/Bytes.h"

#include <deque>
#include <optional>

namespace md::proto {

enum class FrameType : uint8_t { Handshake = 0x01, Secure = 0x02, Audio = 0x03 };
enum class Channel : uint8_t { Control = 0x01, Blob = 0x02 };

inline constexpr uint32_t kMaxFrameLength = 256 * 1024;      // type byte + body
inline constexpr size_t kMaxHandshakeBody = 16 * 1024;
inline constexpr size_t kMaxControlJson = 64 * 1024;
inline constexpr size_t kBlobChunkMax = 32 * 1024;
inline constexpr size_t kBlobHeader = 32 + 4 + 4;
inline constexpr size_t kMaxArtworkBytes = 1024 * 1024;
inline constexpr size_t kMaxLyricsBytes = 256 * 1024;

struct Frame {
    FrameType type;
    Bytes body;
};

Bytes encodeFrame(FrameType type, ByteView body);

// Incremental decoder. Feed arbitrary chunks; pop complete frames.
// Any violation puts it into a permanent error state.
class FrameDecoder {
public:
    explicit FrameDecoder(uint32_t maxLength = kMaxFrameLength) : max_(maxLength) {}
    void feed(ByteView data);
    std::optional<Frame> next();
    bool failed() const { return failed_; }
    const char* error() const { return error_; }
    size_t buffered() const { return buf_.size() - pos_; }

private:
    void fail(const char* why) { failed_ = true; error_ = why; }
    uint32_t max_;
    Bytes buf_;
    size_t pos_ = 0;
    bool failed_ = false;
    const char* error_ = "";
};

// Artwork chunk helpers (secure record channel 0x02, spec §8.6).
struct BlobChunk {
    Bytes hash;        // 32 bytes
    uint32_t total = 0;
    uint32_t offset = 0;
    Bytes data;
};
Bytes encodeBlobChunk(const BlobChunk& c);
std::optional<BlobChunk> decodeBlobChunk(ByteView channelData);

} // namespace md::proto
