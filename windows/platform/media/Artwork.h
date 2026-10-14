#pragma once

#include "md/media/ArtworkPolicy.h"
#include "md/media/MediaTypes.h"
#include "md/util/Bytes.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>

namespace md::platform {

// Content-addressed artwork cache (%LOCALAPPDATA%\MediaDuet\cache\art\<sha256>.jpg).
// Local images are normalised once (≤512 px JPEG, ≤1 MiB) so every device fetches and
// decodes each cover at most once. Thread-safe.
class ArtworkCache {
public:
    struct Ingested {
        media::ArtworkRef ref;
        media::ImageSize source; // decoded size of the input
        media::ImageSize stored; // size of the normalised JPEG
    };

    explicit ArtworkCache(std::filesystem::path dir);

    // Decodes any WIC-supported image, downsizes and re-encodes as JPEG. Blocking.
    std::optional<media::ArtworkRef> ingest(ByteView raw);
    // Same, also reporting the image sizes; maxSide bounds the longer side of the stored
    // JPEG (the result always stays within media::artwork::kMaxBytes). Blocking.
    std::optional<Ingested> ingestImage(ByteView raw, uint32_t maxSide = media::artwork::kMaxSide);
    // Decoded size of an image without converting it (reads the header only). Blocking.
    static std::optional<media::ImageSize> probe(ByteView raw);
    // Stores bytes received from a peer after verifying their SHA-256.
    bool store(const std::string& hash, ByteView data);
    bool has(const std::string& hash) const;
    std::optional<Bytes> read(const std::string& hash) const;
    std::filesystem::path pathFor(const std::string& hash) const;
    const std::filesystem::path& dir() const { return dir_; }
    // Marks an entry as recently used so prune() keeps it. Blocking (file metadata).
    void touch(const std::string& hash);
    // Keeps the newest `maxFiles` entries.
    void prune(size_t maxFiles = 400);

private:
    std::filesystem::path dir_;
    mutable std::mutex m_;
    std::map<std::string, Ingested> rawToRef_; // sha256(raw):maxSide -> normalised image
};

} // namespace md::platform
