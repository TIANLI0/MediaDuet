#pragma once

#include "md/lyrics/Lrc.h"
#include "md/media/MediaTypes.h"
#include "md/util/Scheduler.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace md::platform {

// Provider chain: cache -> lyrics pushed by the phone (app-exposed) -> local .lrc
// folder -> online providers in the user's order: NetEase Cloud Music (public web
// API, word-timed "yrc" when available), QQ Music (public web API), LRCLIB (open
// API). Only title/artist/album/duration are sent; online lookup can be disabled.
// Results are matched by title/artist/duration and never invented.
class LyricsService {
public:
    struct Result {
        std::string trackId;
        std::string status = "none"; // none | loading | found | notfound | disabled
        std::string source;          // cache | local | remote | lrclib
        lyrics::Lyrics lyrics;
        std::string lrc;             // raw text (for forwarding to the phone)
        std::string translation;     // translated LRC, if any
    };
    using Fn = std::function<void(const Result&)>;

    LyricsService(IScheduler& sched, std::filesystem::path cacheDir, Fn onResult);
    ~LyricsService();

    void configure(bool online, std::filesystem::path localFolder, std::vector<std::string> providers);
    // neteaseId: exact NetEase song id when known (NetEase desktop), skips searching.
    void request(const media::MediaSessionInfo& track, int64_t neteaseId = 0);
    void clear();
    // Lyrics pushed by the peer for a track (spec lyrics.push).
    void offerRemote(const std::string& trackId, const std::string& lrc, const std::string& source,
                     const std::string& translation = {});
    const Result& current() const { return cur_; }

private:
    void deliver(Result r);

    IScheduler& sched_;
    std::filesystem::path cacheDir_;
    Fn onResult_;
    bool online_ = true;
    std::vector<std::string> providers_{"netease", "qqmusic", "lrclib"};
    std::filesystem::path local_;
    Result cur_;
    uint64_t gen_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace md::platform
