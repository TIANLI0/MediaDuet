#include "md/util/AppVersion.h"

#include <cctype>

namespace md::version {

namespace {

// Reads up to 6 digits at s[i]; nullopt if there is no digit.
std::optional<int> number(std::string_view s, size_t& i) {
    size_t start = i;
    int v = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) && i - start < 6) v = v * 10 + (s[i++] - '0');
    if (i == start) return std::nullopt;
    if (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) return std::nullopt; // absurdly long
    return v;
}

} // namespace

std::optional<AppVersion> parse(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i < s.size() && (s[i] == 'v' || s[i] == 'V')) ++i;
    AppVersion v;
    auto major = number(s, i);
    if (!major || i >= s.size() || s[i] != '.') return std::nullopt;
    ++i;
    auto minor = number(s, i);
    if (!minor) return std::nullopt;
    v.major = *major;
    v.minor = *minor;
    if (i < s.size() && s[i] == '.') {
        size_t j = i + 1;
        if (auto patch = number(s, j)) {
            v.patch = *patch;
            i = j;
        }
    }
    // Anything else must be a suffix, not more numbers ("1.2.3.4" is not a version we know).
    if (i < s.size() && s[i] == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))
        return std::nullopt;
    return v;
}

Match compare(std::string_view self, std::string_view peer) {
    auto a = parse(self), b = parse(peer);
    if (!a || !b) return Match::Unknown;
    if (a->major != b->major || a->minor != b->minor) return Match::Mismatch;
    return a->patch != b->patch ? Match::PatchDiffers : Match::Equal;
}

const char* toString(Match m) {
    switch (m) {
    case Match::Equal: return "equal";
    case Match::PatchDiffers: return "patch";
    case Match::Mismatch: return "mismatch";
    default: return "unknown";
    }
}

} // namespace md::version
