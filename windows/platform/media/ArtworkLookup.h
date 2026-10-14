#pragma once

#include "Artwork.h"

#include "md/media/ArtworkPolicy.h"
#include "md/media/MediaTypes.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace md::platform {

// High-resolution covers for local sessions whose player thumbnail is small: online
// search (NetEase cloudsearch album picUrl, QQ Music album mid, iTunes Search as last
// resort), strict matching (lyrics::matchScore ≥ 0.6), download, validation and
// normalisation through ArtworkCache. Results are remembered per trackId in an
// append-only index next to the artwork cache (cache\artmap.txt, see
// media::artwork::IndexRecord): hits point at the cached JPEG, misses expire after 72 h.
//
// Thread-safe. cached() does no I/O (an in-memory index loaded by load()), so it can be
// called on the scheduler thread; load(), search(), remember() and touch() block.
class ArtworkLookup {
public:
    struct Query {
        std::string trackId;
        std::string appId; // picks the provider order (the player's own catalogue first)
        std::string title, artist, album;
        std::optional<int64_t> durationMs;
        std::optional<media::ImageSize> current; // the player's thumbnail
    };
    struct Hit {
        media::ArtworkRef ref;
        media::ImageSize size; // decoded size of the downloaded image
        std::string provider;
    };
    enum class Status {
        Found,
        NotFound, // every provider answered: no acceptable cover (remembered as a miss)
        Failed,   // network trouble: not remembered, may be retried later
    };
    struct Result {
        Status status = Status::Failed;
        std::optional<Hit> hit;
    };
    struct Cached {
        std::optional<media::ArtworkRef> ref; // nullopt: a fresh "no cover" record
    };

    ArtworkLookup(ArtworkCache& art, std::filesystem::path indexFile);

    // Reads and compacts the index (drops expired misses and hits whose image is gone).
    void load();
    // nullopt: nothing known for this trackId (or the miss expired).
    std::optional<Cached> cached(const std::string& trackId) const;
    // Online search; never throws.
    Result search(const Query& q);
    // Records a hit (or a miss when ref is nullopt) for trackId, in memory and on disk.
    void remember(const std::string& trackId, const std::optional<media::ArtworkRef>& ref,
                  const std::string& provider = {});
    void touch(const std::string& hash) { art_.touch(hash); }

private:
    ArtworkCache& art_;
    std::filesystem::path indexFile_;
    mutable std::mutex m_;
    std::map<std::string, media::artwork::IndexRecord> index_;
    std::mutex fileM_;
};

} // namespace md::platform
