#pragma once

#include "md/lyrics/Lrc.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Decisions for upgrading low-resolution player thumbnails (GSMTC) to high-resolution
// covers found online. Pure logic; the network and image codecs live in the platform
// layer (platform/media/ArtworkLookup, platform/media/Artwork).
namespace md::media {

struct ImageSize {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t shortSide() const { return std::min(width, height); }
    uint32_t longSide() const { return std::max(width, height); }
    bool operator==(const ImageSize&) const = default;
};

namespace artwork {

// Longest side of every image in the artwork cache (host ArtworkCache and the phone's
// ArtworkProcessor.MAX_EDGE both normalise to 512 px JPEG).
inline constexpr uint32_t kMaxSide = 512;
// Longest side of covers found online. Kept at the cache's limit; raising it only needs
// this constant (the encoder steps the JPEG quality down to stay within kMaxBytes).
inline constexpr uint32_t kOnlineMaxSide = kMaxSide;
// Spec: artwork.get BLOB transfers are at most 1 MiB.
inline constexpr uint32_t kMaxBytes = 1024 * 1024;
// A thumbnail whose shorter side is below this is worth replacing.
inline constexpr uint32_t kUpgradeBelowPx = 500;
// Smallest online cover we accept at all.
inline constexpr uint32_t kMinOnlinePx = 300;
// Download limit for an online cover (before re-encoding).
inline constexpr size_t kMaxDownloadBytes = 8 * 1024 * 1024;
// lyrics::matchScore threshold for a search result to be used (never a weak match).
inline constexpr double kMinMatchScore = 0.6;
// Pixel size requested from the providers (QQ Music serves 150/300/500/800 only).
inline constexpr int kFetchPx = 1000;
inline constexpr int kQqFetchPx = 800;
// "No cover found" is remembered this long (like the lyrics negative cache).
inline constexpr int64_t kNegativeTtlSec = 72 * 3600;

// Covers are (nearly) square; a wide image is a video frame, not a cover.
bool isSquarish(ImageSize s);
// True if the player's thumbnail (nullopt: none / undecodable) should be replaced by an
// online cover: missing, or a square-ish image with a shorter side < kUpgradeBelowPx
// that a maxSide cover would actually improve (see isBetter). Wide thumbnails (video
// frames) are never replaced.
bool shouldUpgrade(std::optional<ImageSize> current, uint32_t maxSide = kOnlineMaxSide);
// True if `candidate` (decoded size of a downloaded cover) is a real improvement over the
// current thumbnail once both are normalised to at most maxSide: square-ish, at least
// kMinOnlinePx, and clearly larger (≥ 20 % and ≥ 32 px) than the current one.
bool isBetter(std::optional<ImageSize> current, ImageSize candidate, uint32_t maxSide = kOnlineMaxSide);

// Image URL builders. Return "" when the input is not a plausible URL/id of that
// provider (only https URLs on the provider's image hosts are produced).
// NetEase album picUrl (p1.music.126.net/…jpg) -> https + "?param=<px>y<px>".
std::string neteaseCoverUrl(std::string_view picUrl, int px = kFetchPx);
// QQ Music album mid -> https://y.gtimg.cn/music/photo_new/T002R<px>x<px>M000<mid>.jpg
std::string qqAlbumCoverUrl(std::string_view albumMid, int px = kQqFetchPx);
// iTunes Search artworkUrl100 (…/100x100bb.jpg) -> …/<px>x<px>bb.jpg
std::string itunesCoverUrl(std::string_view artworkUrl100, int px = kFetchPx);
// Lower-case host of an http(s) URL, or "".
std::string urlHost(std::string_view url);

// "image/jpeg" | "image/png" | "image/webp" | "image/gif" | "image/bmp" from the magic
// bytes, or nullopt if the data is not an image.
std::optional<std::string> sniffImageMime(const uint8_t* data, size_t size);

// Search keyword: title without bracketed suffixes plus the first artist.
std::string searchQuery(std::string_view title, std::string_view artist);
// Online provider order for a player: its own catalogue first (QQ Music, NetEase).
std::vector<std::string> providerOrder(std::string_view appId);
// Ranking of a search result: lyrics::matchScore (0 if below kMinMatchScore) plus small
// bonuses for the same album and the literally same title, so the right edition's cover wins.
double coverMatchScore(const lyrics::Candidate& want, const lyrics::Candidate& have);

// The song a cover belongs to.
struct SongIdentity {
    std::string trackId;
    std::string title, artist, album;
    std::optional<int64_t> durationMs;
};
// True if an upgraded cover found for `a` still applies to `b`: the same trackId, or the
// same title and artist (normalised), durations within durationToleranceMs (or unknown)
// and no conflicting album (equal after normalisation, or one of them empty, e.g. a
// player filling the album in later). Players re-fire MediaPropertiesChanged for the
// same song (thumbnail re-sent, duration jitter); those must keep the upgrade.
bool sameRecording(const SongIdentity& a, const SongIdentity& b, int64_t durationToleranceMs = 3000);

// Cache keys are trackIds (spec §8.3: hex); anything else is rejected.
bool isValidCacheKey(std::string_view trackId);
// True while a negative ("no cover") record written at writtenAtSec is still valid.
bool negativeFresh(int64_t writtenAtSec, int64_t nowSec, int64_t ttlSec = kNegativeTtlSec);

// One line of the on-disk index (cache\artmap.txt), tab separated:
//   <trackId> <sha256 hex | -> <bytes> <unix seconds> <provider>
// "-" is a negative record. Later lines override earlier ones.
struct IndexRecord {
    std::string trackId;
    std::optional<std::string> hash; // nullopt = no cover found
    uint32_t size = 0;
    int64_t atSec = 0;
    std::string provider;
    bool operator==(const IndexRecord&) const = default;
};
std::string formatIndexRecord(const IndexRecord& r);
std::optional<IndexRecord> parseIndexRecord(std::string_view line);

} // namespace artwork
} // namespace md::media
