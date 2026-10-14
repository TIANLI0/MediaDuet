#include "md/media/ArtworkPolicy.h"

#include "md/util/Text.h"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstring>

namespace md::media::artwork {

namespace {

// Shorter side after normalising to at most maxSide on the longer side.
double normalisedShort(ImageSize s, uint32_t maxSide) {
    if (s.longSide() == 0) return 0;
    double scale = std::min(1.0, static_cast<double>(maxSide) / s.longSide());
    return s.shortSide() * scale;
}

bool startsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

bool hostIs(const std::string& host, std::string_view domain) {
    if (host == domain) return true;
    return host.size() > domain.size() && host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
           host[host.size() - domain.size() - 1] == '.';
}

// https URL without query/fragment, or "" if not http(s).
std::string httpsBase(std::string_view url) {
    std::string u = text::trim(url);
    if (startsWith(u, "http://")) u = "https://" + u.substr(7);
    if (!startsWith(u, "https://")) return {};
    if (auto q = u.find_first_of("?#"); q != std::string::npos) u.resize(q);
    for (char c : u)
        if (static_cast<unsigned char>(c) <= 0x20 || c == '"' || c == '\'' || c == '<' || c == '>' || c == '\\') return {};
    return u;
}

bool sameText(const std::string& x, const std::string& y) {
    std::string tx = text::trim(x), ty = text::trim(y);
    if (tx == ty) return true;
    std::string nx = lyrics::normalize(tx), ny = lyrics::normalize(ty);
    return !nx.empty() && nx == ny;
}

} // namespace

bool isSquarish(ImageSize s) {
    if (s.shortSide() == 0) return false;
    return static_cast<double>(s.longSide()) / s.shortSide() <= 1.34;
}

bool isBetter(std::optional<ImageSize> current, ImageSize candidate, uint32_t maxSide) {
    if (!isSquarish(candidate) || candidate.shortSide() < kMinOnlinePx) return false;
    double cand = normalisedShort(candidate, maxSide);
    if (!current || current->shortSide() == 0) return true;
    double cur = normalisedShort(*current, maxSide);
    return cand >= cur + std::max(32.0, cur * 0.2);
}

bool shouldUpgrade(std::optional<ImageSize> current, uint32_t maxSide) {
    if (!current || current->shortSide() == 0) return true;
    if (!isSquarish(*current) || current->shortSide() >= kUpgradeBelowPx) return false;
    return isBetter(current, {maxSide, maxSide}, maxSide);
}

std::string urlHost(std::string_view url) {
    std::string u = text::toLowerAscii(text::trim(url));
    size_t start;
    if (startsWith(u, "https://")) start = 8;
    else if (startsWith(u, "http://")) start = 7;
    else return {};
    size_t end = u.find_first_of("/?#", start);
    std::string host = u.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (host.find('@') != std::string::npos) return {}; // no userinfo tricks
    if (auto colon = host.find(':'); colon != std::string::npos) host.resize(colon);
    return host;
}

std::string neteaseCoverUrl(std::string_view picUrl, int px) {
    std::string base = httpsBase(picUrl);
    if (base.empty() || px <= 0 || !hostIs(urlHost(base), "music.126.net")) return {};
    std::string n = std::to_string(px);
    return base + "?param=" + n + "y" + n;
}

std::string qqAlbumCoverUrl(std::string_view albumMid, int px) {
    std::string mid = text::trim(albumMid);
    if (mid.empty() || mid.size() > 32 || px <= 0) return {};
    for (char c : mid)
        if (!std::isalnum(static_cast<unsigned char>(c))) return {};
    std::string n = std::to_string(px);
    return "https://y.gtimg.cn/music/photo_new/T002R" + n + "x" + n + "M000" + mid + ".jpg";
}

std::string itunesCoverUrl(std::string_view artworkUrl100, int px) {
    std::string base = httpsBase(artworkUrl100);
    if (base.empty() || px <= 0 || !hostIs(urlHost(base), "mzstatic.com")) return {};
    // .../<w>x<h>bb.<ext>: replace the last path segment's size.
    auto slash = base.rfind('/');
    if (slash == std::string::npos) return {};
    std::string last = base.substr(slash + 1);
    auto x = last.find('x');
    auto bb = last.find("bb.");
    if (x == std::string::npos || bb == std::string::npos || x == 0 || bb <= x + 1) return {};
    for (size_t i = 0; i < bb; ++i)
        if (i != x && !std::isdigit(static_cast<unsigned char>(last[i]))) return {};
    std::string n = std::to_string(px);
    return base.substr(0, slash + 1) + n + "x" + n + last.substr(bb);
}

std::optional<std::string> sniffImageMime(const uint8_t* d, size_t n) {
    if (!d) return std::nullopt;
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "image/jpeg";
    if (n >= 8 && std::memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
    if (n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0) return "image/webp";
    if (n >= 6 && (std::memcmp(d, "GIF87a", 6) == 0 || std::memcmp(d, "GIF89a", 6) == 0)) return "image/gif";
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return "image/bmp";
    return std::nullopt;
}

std::string searchQuery(std::string_view titleIn, std::string_view artistIn) {
    std::string title(titleIn);
    for (const char* open : {" (", "\xEF\xBC\x88", " [", "\xE3\x80\x90"}) { // ( （ [ 【
        auto p = title.find(open);
        if (p != std::string::npos && p > 0) title.resize(p);
    }
    std::string artist(artistIn);
    for (const char* sep : {"/", ",", "&", "\xE3\x80\x81", ";"}) { // 、
        auto p = artist.find(sep);
        if (p != std::string::npos && p > 0) artist.resize(p);
    }
    return text::trim(text::trim(title) + " " + text::trim(artist));
}

std::vector<std::string> providerOrder(std::string_view appId) {
    std::string a = text::toLowerAscii(appId);
    if (a.find("qqmusic") != std::string::npos) return {"qqmusic", "netease", "itunes"};
    return {"netease", "qqmusic", "itunes"};
}

double coverMatchScore(const lyrics::Candidate& want, const lyrics::Candidate& have) {
    double s = lyrics::matchScore(want, have);
    if (s < kMinMatchScore) return 0;
    if (!want.album.empty() && !have.album.empty() && sameText(want.album, have.album)) s += 0.15;
    if (text::trim(want.title) == text::trim(have.title)) s += 0.05;
    return s;
}

bool sameRecording(const SongIdentity& a, const SongIdentity& b, int64_t tol) {
    if (!a.trackId.empty() && a.trackId == b.trackId) return true;
    if (text::trim(a.title).empty() || text::trim(b.title).empty()) return false;
    if (a.durationMs && b.durationMs && *a.durationMs > 0 && *b.durationMs > 0 &&
        std::llabs(*a.durationMs - *b.durationMs) > tol)
        return false;
    if (!sameText(a.title, b.title) || !sameText(a.artist, b.artist)) return false;
    if (!text::trim(a.album).empty() && !text::trim(b.album).empty() && !sameText(a.album, b.album)) return false;
    return true;
}

bool isValidCacheKey(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

bool negativeFresh(int64_t writtenAtSec, int64_t nowSec, int64_t ttlSec) {
    // A timestamp in the future (clock change) counts as fresh only within one TTL.
    return writtenAtSec + ttlSec > nowSec && writtenAtSec < nowSec + ttlSec;
}

std::string formatIndexRecord(const IndexRecord& r) {
    std::string out = r.trackId;
    out += '\t';
    out += r.hash ? *r.hash : "-";
    out += '\t';
    out += std::to_string(r.hash ? r.size : 0);
    out += '\t';
    out += std::to_string(r.atSec);
    out += '\t';
    for (char c : r.provider)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') out += c;
    return out;
}

std::optional<IndexRecord> parseIndexRecord(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    std::vector<std::string_view> f;
    size_t start = 0;
    for (;;) {
        auto tab = line.find('\t', start);
        f.push_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
        if (tab == std::string_view::npos) break;
        start = tab + 1;
    }
    if (f.size() != 5 || !isValidCacheKey(f[0])) return std::nullopt;
    IndexRecord r;
    r.trackId = std::string(f[0]);
    if (f[1] != "-") {
        if (f[1].size() != 64) return std::nullopt;
        for (char c : f[1])
            if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
        r.hash = text::toLowerAscii(f[1]);
    }
    auto num = [](std::string_view s, auto& out) {
        auto res = std::from_chars(s.data(), s.data() + s.size(), out);
        return res.ec == std::errc() && res.ptr == s.data() + s.size();
    };
    if (!num(f[2], r.size) || !num(f[3], r.atSec)) return std::nullopt;
    if (r.hash && (r.size == 0 || r.size > kMaxBytes)) return std::nullopt;
    if (!r.hash) r.size = 0;
    r.provider = std::string(f[4]);
    if (r.provider.size() > 32) return std::nullopt;
    return r;
}

} // namespace md::media::artwork
