#pragma once

#include "Artwork.h"
#include "ArtworkLookup.h"

#include "md/media/MediaTypes.h"
#include "md/util/Scheduler.h"

#include <winrt/Windows.Media.Control.h>

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace md::platform {

// Windows media sessions via GlobalSystemMediaTransportControlsSessionManager.
// Fully event driven: no polling. Excludes MediaDuet's own SMTC bridge session.
class GsmtcSource {
public:
    using SnapshotFn = std::function<void(const media::MediaSnapshot&)>;
    using ResultFn = std::function<void(bool ok, const std::string& code)>;

    GsmtcSource(IScheduler& sched, ArtworkCache& art, std::function<bool(const std::string& aumid)> isOwn,
                SnapshotFn onSnapshot);
    ~GsmtcSource();

    void start();
    void execute(const media::MediaCommand& cmd, ResultFn done);
    const media::MediaSnapshot& snapshot() const { return snap_; }
    bool available() const { return manager_ != nullptr; }
    // Online lookup of high-resolution covers when a player's thumbnail is missing or
    // small (see ArtworkLookup; default on). Off: no network requests; covers already
    // found and cached on disk are still used. Turning it on looks up the current tracks.
    void setArtworkOnline(bool on);

private:
    using Session = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
    struct Entry {
        Session session{nullptr};
        std::string aumid;
        media::MediaSessionInfo info;
        winrt::event_token propsTok{}, playTok{}, timeTok{};
        uint64_t propsGen = 0;
        // When the player first reported empty properties after having metadata
        // (players such as QQ Music briefly clear them between updates); -1 = not blank.
        int64_t blankSince = -1;
        // Cover art: the player's own thumbnail and, when that is small, a high-resolution
        // cover found online for the current song (kept for the whole song; see .cpp).
        std::optional<media::ArtworkRef> thumb;
        std::optional<media::ImageSize> thumbSize; // decoded size of the player's thumbnail
        struct Upgrade {
            media::ArtworkRef ref;
            media::artwork::SongIdentity song;
        };
        std::optional<Upgrade> hd;
        std::optional<media::artwork::SongIdentity> lookupFor; // song an online lookup was started for
        uint64_t artGen = 0;                                    // invalidates lookups of a previous song
    };

    void rebuild();
    void subscribe(const std::string& key, Entry& e);
    void unsubscribe(Entry& e);
    void updatePlayback(const std::string& key);
    void updateTimeline(const std::string& key);
    // Reads the timeline into e.info. trackChanged: title/artist/album just changed, so
    // the duration is taken as reported; otherwise it is kept stable (see .cpp).
    // Returns true if the duration changed (the trackId must then be recomputed).
    bool readTimeline(Entry& e, bool trackChanged);
    void fetchProperties(const std::string& key);
    // Picks e.info.artwork from the thumbnail and the cover upgrade once the trackId is final.
    void resolveArtwork(const std::string& key, Entry& e);
    void startArtworkLookup(const std::string& key, uint64_t gen);
    void applyArtworkResult(const std::string& key, uint64_t gen, const ArtworkLookup::Query& q,
                            const ArtworkLookup::Result& r);
    // Background: marks the cover recently used; alias: also records it under trackId.
    void persistArtwork(const std::string& trackId, const media::ArtworkRef& ref, bool alias);
    void scheduleEmit();
    void emit();
    std::string appName(const std::string& aumid);
    Entry* find(const std::string& key);

    IScheduler& sched_;
    ArtworkCache& art_;
    std::shared_ptr<ArtworkLookup> artLookup_;
    bool artworkOnline_ = true;
    uint64_t artSeq_ = 0;
    std::function<bool(const std::string&)> isOwn_;
    SnapshotFn onSnapshot_;
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager manager_{nullptr};
    winrt::event_token sessionsTok_{}, currentTok_{};
    std::map<std::string, Entry> entries_;
    std::string currentKey_;
    std::map<std::string, std::string> appNames_;
    media::MediaSnapshot snap_;
    ScopedTimer emitTimer_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace md::platform
