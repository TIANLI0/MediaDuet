#pragma once

#include "media/HandoffLauncher.h"

#include "md/handoff/Handoff.h"
#include "md/media/MediaTypes.h"
#include "md/protocol/Envelope.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace md::app {

// Handoff on the Windows host (spec §8.7): the coordinator for both directions, the PC side
// of "play this song here" (HandoffLauncher + watching the local sessions until the song
// plays) and the messages to and from the phone.
class HandoffHost {
public:
    using Device = handoff::Device;
    struct Callbacks {
        std::function<const media::MediaSnapshot&()> local;
        std::function<const media::MediaSnapshot&()> remote;
        // A user command (as from MediaDuet's UI; the arbiter attributes it to the user).
        std::function<void(Device, const std::string& key, media::CommandAction, std::optional<int64_t> pos)> command;
        // NetEase sessions without a timeline: seek with NetEase's web command.
        std::function<bool(const std::string& key, int64_t pos)> neteaseSeek;
        std::function<bool(const std::string& key)> positionEstimated;
        // The phone is connected and announced `handoff`.
        std::function<bool()> peerReady;
        std::function<uint64_t(const char* type, nlohmann::json body, std::optional<uint64_t> re)> send;
        std::function<int64_t()> rttMs;
        std::function<void()> changed;
        std::function<void(const handoff::Outcome&)> outcome;
    };

    HandoffHost(IScheduler& sched, Callbacks cb);
    ~HandoffHost();

    void onLocalSnapshot();
    void onLinkUp();
    void onLinkDown();
    // handoff.* from the phone; false if `e` is not a handoff message.
    bool onEnvelope(const proto::Envelope& e);
    // From the UI: move session `key` of `from` to the other device.
    void start(Device from, const std::string& key, std::optional<handoff::App> app,
               std::function<void(const handoff::Outcome&)> done);
    // state.handoff for the UI (docs/ipc.md).
    nlohmann::json stateJson() const;
    // Music apps on this PC, and the executable of one (for its icon).
    const std::vector<handoff::App>& localApps() const { return launcher_.apps(); }
    std::filesystem::path appExe(handoff::App app) const { return launcher_.exePath(app); }
    bool busy() const { return coord_->status().active; }

    // How long the PC waits for the opened song to play before reporting "opened".
    static constexpr int64_t kLocalWaitMs = 25'000;
    // A target that shows the song but stays paused is started after this.
    static constexpr int64_t kPlayNudgeMs = 2'500;

private:
    void sendApps();
    void refreshTick();
    void playLocal(handoff::App app, const handoff::Song& song, const media::CatalogIds& ids,
                   std::function<void(const handoff::PlayResult&)> done);
    void playRemote(handoff::App app, const handoff::Song& song, const media::CatalogIds& ids,
                    std::function<void(const handoff::PlayResult&)> done);
    void checkLocal();
    void finishLocal(handoff::PlayResult r);
    const media::MediaSessionInfo* session(Device d, const std::string& key) const;

    IScheduler& sched_;
    Callbacks cb_;
    platform::HandoffLauncher launcher_;
    std::unique_ptr<handoff::Coordinator> coord_;
    std::vector<handoff::App> remoteApps_;
    std::string lastAppsSent_;

    struct LocalPlay {
        uint64_t id = 0;
        handoff::App app = handoff::App::Netease;
        handoff::Song song;
        bool launched = false;
        int64_t launchedAt = 0;
        bool nudged = false;
        std::function<void(const handoff::PlayResult&)> done;
    };
    std::optional<LocalPlay> local_;
    uint64_t localSeq_ = 0;
    ScopedTimer localTimer_, localTimeout_, refreshTimer_;

    struct RemotePlay {
        uint64_t msgId = 0;
        std::function<void(const handoff::PlayResult&)> done;
    };
    std::optional<RemotePlay> remote_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace md::app
