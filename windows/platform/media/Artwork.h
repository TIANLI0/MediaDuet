#pragma once

#include "md/media/MediaTypes.h"
#include "md/util/Bytes.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>

namespace md::platform {

// Content-addressed artwork cache (%LOCALAPPDATA%\MediaDuet\cache\art\<sha256>.jpg).
// Local images are normalised once (≤512 px JPEG) so every device fetches and
// decodes each cover at most once. Thread-safe.
class ArtworkCache {
public:
    explicit ArtworkCache(std::filesystem::path dir);

    // Decodes any WIC-supported image, downsizes and re-encodes as JPEG. Blocking.
    std::optional<media::ArtworkRef> ingest(ByteView raw);
    // Stores bytes received from a peer after verifying their SHA-256.
    bool store(const std::string& hash, ByteView data);
    bool has(const std::string& hash) const;
    std::optional<Bytes> read(const std::string& hash) const;
    std::filesystem::path pathFor(const std::string& hash) const;
    // Keeps the newest `maxFiles` entries.
    void prune(size_t maxFiles = 400);

private:
    std::filesystem::path dir_;
    mutable std::mutex m_;
    std::map<std::string, media::ArtworkRef> rawToRef_; // sha256(raw) -> normalised ref
};

} // namespace md::platform
