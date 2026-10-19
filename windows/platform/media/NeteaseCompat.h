#pragma once

#include "Artwork.h"

#include "md/media/MediaTypes.h"
#include "md/util/Scheduler.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace md::platform {

// Compatibility source for NetEase Cloud Music for Windows when it does not
// publish a system media session. No UI automation and no memory reading:
//  * track: the main window title ("Title - Artist"), via a title-change hook
//    scoped to the NetEase process (event driven);
//  * id / duration / album / cover: matched in NetEase's local play queue file;
//  * playing state: whether the process is audible (Core Audio, from the host);
//  * position: estimated from the track start and audible time (flagged);
//  * control: WM_APPCOMMAND media commands posted to its main window.
class NeteaseCompat {
public:
    NeteaseCompat(IScheduler& sched, ArtworkCache& art, std::function<void()> changed);
    ~NeteaseCompat();

    void setEnabled(bool enabled);
    // NetEase already publishes a GSMTC session: stay passive.
    void setSmtcPresent(bool present);
    // From the audio monitor: is cloudmusic.exe audible right now?
    void setAudible(bool audible);

    std::optional<media::MediaSessionInfo> session() const;
    // NetEase song id of the current track (0 if unknown) for exact lyrics.
    int64_t songId() const { return songId_; }

    // NetEase's own system media session (its "Windows media controls" option) carries title,
    // artist, cover and play state but no timeline. Fills in what is missing: the duration from
    // NetEase's local play queue and a position estimated from the time spent playing since the
    // track changed (re-anchored on every play / pause). No-op when the session has a duration.
    void supplement(media::MediaSessionInfo& s);
    // True when the session's position is an estimate (compat session or supplemented).
    bool positionEstimated(const std::string& key) const { return key == kKey || (!supKey_.empty() && key == supSession_); }
    bool execute(media::CommandAction action);
    // Seeks NetEase with its web command and re-anchors the estimated position of session `key`.
    bool seek(const std::string& key, int64_t positionMs);

    static constexpr const char* kKey = "cloudmusic.exe#compat";

    struct Hook;

private:
    void scan();
    void attach(HWND hwnd, DWORD pid);
    void detach();
    void onTitle();
    void enrich(const std::string& title, const std::string& artist, uint64_t gen);
    struct QueueMatch {
        int64_t id = 0, durationMs = 0;
        std::string album, pic;
    };
    // Looks the track up in NetEase's local play queue (blocking file read: worker thread only).
    static QueueMatch lookupQueue(const std::string& title, const std::string& artist);

    IScheduler& sched_;
    ArtworkCache& art_;
    std::function<void()> changed_;
    bool enabled_ = false;
    bool smtcPresent_ = false;
    bool audible_ = false;
    HWND hwnd_ = nullptr;
    DWORD pid_ = 0;
    HWINEVENTHOOK hook_ = nullptr;
    ScopedTimer scanTimer_;
    std::string title_, artist_, album_;
    std::optional<int64_t> durationMs_;
    std::optional<media::ArtworkRef> artwork_;
    int64_t songId_ = 0;
    int64_t posMs_ = 0;   // accumulated audible time at posAt_
    int64_t posAt_ = 0;
    uint64_t gen_ = 0;
    // supplement(): estimator for NetEase's own session.
    std::string supKey_;      // title + artist of the supplemented track
    std::string supSession_;  // its session key
    bool supPlaying_ = false;
    int64_t supPos_ = 0, supAt_ = 0;
    std::optional<int64_t> supDuration_;
    int64_t supId_ = 0; // NetEase song id of the supplemented track (from the play queue)
    uint64_t supGen_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace md::platform
