#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace md {

using Bytes = std::vector<uint8_t>;
using ByteView = std::span<const uint8_t>;

inline ByteView view(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}
inline ByteView view(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}
inline Bytes toBytes(std::string_view s) { return Bytes(s.begin(), s.end()); }
inline std::string toString(ByteView b) { return std::string(b.begin(), b.end()); }

Bytes concat(std::initializer_list<ByteView> parts);
void append(Bytes& out, ByteView part);

std::string hexEncode(ByteView data);
std::optional<Bytes> hexDecode(std::string_view hex);

std::string base64Encode(ByteView data);
std::optional<Bytes> base64Decode(std::string_view b64);
// RFC 4648 §5 alphabet, no padding (URLs, QR codes).
std::string base64UrlEncode(ByteView data);

inline void putU32BE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24); p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);  p[3] = static_cast<uint8_t>(v);
}
inline uint32_t getU32BE(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline void putU64BE(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) { p[i] = static_cast<uint8_t>(v); v >>= 8; }
}
inline uint64_t getU64BE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

// Overwrites memory in a way the optimiser cannot elide.
void secureZero(Bytes& b);

} // namespace md
