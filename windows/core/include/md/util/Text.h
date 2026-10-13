#pragma once

#include <string>
#include <string_view>

namespace md::text {

std::wstring toWide(std::string_view utf8);
std::string toUtf8(std::wstring_view wide);

// Number of Unicode code points in a UTF-8 string (invalid bytes count as one).
size_t codePoints(std::string_view utf8);
// Truncates to at most maxCodePoints without splitting a code point.
std::string truncate(std::string_view utf8, size_t maxCodePoints);
bool isValidUtf8(std::string_view s);

std::string toLowerAscii(std::string_view s);
std::string trim(std::string_view s);

} // namespace md::text
