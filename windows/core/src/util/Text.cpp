#include "md/util/Text.h"

#include <windows.h>

namespace md::text {

std::wstring toWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string toUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// Length of the UTF-8 sequence starting at s[i], or 1 for invalid lead bytes.
static size_t seqLen(std::string_view s, size_t i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    if (i + len > s.size()) return 1;
    for (size_t k = 1; k < len; ++k)
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 1;
    return len;
}

size_t codePoints(std::string_view s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i += seqLen(s, i)) ++n;
    return n;
}

std::string truncate(std::string_view s, size_t maxCp) {
    size_t i = 0, n = 0;
    while (i < s.size() && n < maxCp) {
        i += seqLen(s, i);
        ++n;
    }
    return std::string(s.substr(0, i));
}

bool isValidUtf8(std::string_view s) {
    return s.empty() ||
           MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0) > 0;
}

std::string toLowerAscii(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return std::string(s.substr(b, e - b));
}

} // namespace md::text
