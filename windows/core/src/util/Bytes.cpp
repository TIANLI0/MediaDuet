#include "md/util/Bytes.h"

#include <windows.h>

namespace md {

Bytes concat(std::initializer_list<ByteView> parts) {
    size_t total = 0;
    for (auto p : parts) total += p.size();
    Bytes out;
    out.reserve(total);
    for (auto p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

void append(Bytes& out, ByteView part) { out.insert(out.end(), part.begin(), part.end()); }

std::string hexEncode(ByteView data) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    s.resize(data.size() * 2);
    for (size_t i = 0; i < data.size(); ++i) {
        s[2 * i] = kHex[data[i] >> 4];
        s[2 * i + 1] = kHex[data[i] & 0xF];
    }
    return s;
}

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::optional<Bytes> hexDecode(std::string_view hex) {
    if (hex.size() % 2) return std::nullopt;
    Bytes out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        int hi = hexVal(hex[2 * i]), lo = hexVal(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return out;
}

static constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(ByteView data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];  out += kB64[v & 63];
    }
    size_t rem = data.size() - i;
    if (rem == 1) {
        uint32_t v = uint32_t(data[i]) << 16;
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63]; out += "==";
    } else if (rem == 2) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63]; out += '=';
    }
    return out;
}

std::string base64UrlEncode(ByteView data) {
    std::string out = base64Encode(data);
    while (!out.empty() && out.back() == '=') out.pop_back();
    for (char& c : out) c = c == '+' ? '-' : c == '/' ? '_' : c;
    return out;
}

static int b64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::optional<Bytes> base64Decode(std::string_view in) {
    if (in.size() % 4) return std::nullopt;
    Bytes out;
    out.reserve(in.size() / 4 * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            char c = in[i + k];
            if (c == '=') {
                // Padding only allowed in the last quantum, positions 2/3.
                if (i + 4 != in.size() || k < 2) return std::nullopt;
                v[k] = 0;
                ++pad;
            } else {
                if (pad) return std::nullopt;
                v[k] = b64Val(c);
                if (v[k] < 0) return std::nullopt;
            }
        }
        uint32_t n = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
        out.push_back(static_cast<uint8_t>(n >> 16));
        if (pad < 2) out.push_back(static_cast<uint8_t>(n >> 8));
        if (pad < 1) out.push_back(static_cast<uint8_t>(n));
    }
    return out;
}

void secureZero(Bytes& b) {
    if (!b.empty()) SecureZeroMemory(b.data(), b.size());
}

} // namespace md
