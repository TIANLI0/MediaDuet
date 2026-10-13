#pragma once

#include "Artwork.h"

#include "md/media/MediaTypes.h"
#include "md/util/Scheduler.h"

#include <winrt/Windows.Media.Control.h>

#include <functional>
#include <map>
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

private:
    using Session = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
    struct Entry {
        Session session{nullptr};
        std::string aumid;
        media::MediaSessionInfo info;
        winrt::event_token propsTok{}, playTok{}, timeTok{};
        uint64_t propsGen = 0;
    };

    void rebuild();
    void subscribe(const std::string& key, Entry& e);
    void unsubscribe(Entry& e);
    void updatePlayback(const std::string& key);
    void updateTimeline(const std::string& key);
    void fetchProperties(const std::string& key);
    void scheduleEmit();
    void emit();
    std::string appName(const std::string& aumid);
    Entry* find(const std::string& key);

    IScheduler& sched_;
    ArtworkCache& art_;
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
