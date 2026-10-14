#pragma once

#include "Diagnostics.h"
#include "IpcServer.h"
#include "TrayIcon.h"

#include "Scheduler.h"
#include "Stores.h"
#include "audio/A2dpSink.h"
#include "audio/AudioActivityMonitor.h"
#include "audio/LoopbackSender.h"
#include "audio/VolumeControl.h"
#include "bluetooth/BluetoothWatcher.h"
#include "lyrics/LyricsService.h"
#include "media/Artwork.h"
#include "media/GsmtcSource.h"
#include "media/NeteaseCompat.h"
#include "media/SmtcBridge.h"
#include "network/DnssdBrowser.h"
#include "network/LanTransport.h"
#include "overlay/LyricsOverlay.h"
#include "overlay/TaskbarWidget.h"

#include "md/link/ConnectionManager.h"
#include "md/lyrics/LyricsClock.h"
#include "md/policy/PlaybackArbiter.h"
#include "md/settings/Settings.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace md::app {

using nlohmann::json;

// Orchestrates every module of the Windows host. Single-threaded: all methods
// run on the Win32 message-loop thread through the scheduler.
class HostApp final : public link::ConnectionManager::Listener {
public:
    HostApp();
    ~HostApp() override;

    // Returns the process exit code.
    int run(bool background, const std::string& showWindow);

    // ConnectionManager::Listener
    void onLinkUp(const std::string& peerId, bool resumed) override;
    void onLinkDown(const std::string& peerId) override;
    void onEnvelope(const std::string& peerId, const proto::Envelope& e, uint64_t connId) override;
    void onBlob(const std::string& peerId, const proto::BlobChunk& c) override;
    void onPeerRevoked(const std::string& peerId) override;
    void onStatusChanged() override;
    void onPeerEndpoints(const std::string& peerId, const std::vector<link::LanEndpoint>& eps) override;

private:
    enum class Side { Local, Remote };

    void init();
    void shutdown();
    void applySettings();
    void reloadPeers();
    void updateDiscoveryWatchers();

    // media
    void onLocalSnapshot(const media::MediaSnapshot& snap);
    void onRemoteSnapshot(const proto::Envelope& e);
    void sendLocalSnapshot();
    void executeRemoteCommand(const proto::Envelope& e, uint64_t connId);
    void sendUserCommand(Side side, std::optional<std::string> key, media::CommandAction action,
                         std::optional<int64_t> pos, const std::string& reason, std::string customAction = {},
                         std::optional<int64_t> queueId = std::nullopt);
    void executeArbiterAction(const policy::Action& a);
    void serveArtwork(const proto::Envelope& e, uint64_t connId);
    void requestMissingArtwork();
    // Play queues of the phone's sessions (media.queue), fetched when a session's queue rev changes.
    void requestQueues();
    void sendQueueToUi(const media::MediaQueue& q);
    std::map<std::string, media::MediaQueue> queues_;
    std::map<std::string, std::pair<std::string, int64_t>> queueRequested_; // key -> (rev, at)
    std::string artworkPath(const media::MediaSessionInfo& s) const;

    // focus, lyrics, overlay, SMTC
    const media::MediaSessionInfo* focused(Side* side = nullptr) const;
    void updateFocus();
    void onLyrics(const platform::LyricsService::Result& r);
    bool standalone() const { return standalone_; }
    void rememberPeerApp(platform::DeviceRecord& d, const std::string& app);
    json peerAppFor(const link::LinkStatus& ls) const;
    void pushLyricsToPhone(const platform::LyricsService::Result& r);
    void updateOverlayPlayback();
    // Lyrics timing (docs/ipc.md "Lyrics timing"): output latency of the device that
    // plays the session (this PC's default output, or what the phone reports) and
    // the total delay the lyrics are shifted by (latency if auto + per-app offset).
    int localOutputLatencyMs() const;
    int outputLatencyFor(const media::MediaSessionInfo& s, Side side) const;
    int64_t lyricsDelayFor(const media::MediaSessionInfo& s, Side side) const;
    void adjustLyricsOffset(const std::string& appId, int deltaMs, bool reset);
    void answerVolumeSet(bool ok, const std::string& code);
    void updateSmtc();
    void sendPolicyStatus();

    // pairing
    void startPairing(const std::string& kind, const std::string& target);
    void pairingRespond(bool accept);
    void cancelPairing();
    void setPairing(std::string state, std::string error = {});

    // audio
    void setA2dp(bool enable);
    void sendSinkState();
    // Media-control commands this soon after a stream start / output change come from headsets.
    static constexpr int64_t kSystemCommandWindowMs = 2500;
    int64_t outputChangedAt_ = -1;
    int64_t linkUpAt_ = -1;
    int64_t lyricsAskedAt_ = -1; // the current track's lyrics lookup started (log timing)
    std::string lastSinkJson_;
    void setLanSend(bool enable);

    // UI / IPC
    void handleIpc(const std::string& line);
    void markDirty();
    void pushState();
    json buildState() const;
    json sessionJson(const media::MediaSessionInfo& s, Side side) const;
    void sendLyricsToUi();
    void openUi(const std::string& window);
    void updateTray();
    void tickDiagnostics();
    void exitApp();

    platform::Win32Scheduler sched_;
    platform::Identity identity_;
    platform::DeviceStore devices_;
    settings::Settings settings_;
    std::filesystem::path settingsPath_;
    std::unique_ptr<platform::ArtworkCache> art_;
    std::unique_ptr<platform::TransportFactory> factory_;
    std::unique_ptr<link::ConnectionManager> cm_;
    std::unique_ptr<platform::BluetoothWatcher> bt_;
    std::unique_ptr<platform::DnssdBrowser> dnssd_;
    std::unique_ptr<platform::GsmtcSource> gsmtc_;
    std::unique_ptr<platform::NeteaseCompat> netease_;
    media::MediaSnapshot gsmtcSnap_;
    uint64_t localGen_ = 0;
    void rebuildLocal();
    void executeLocal(const media::MediaCommand& cmd, std::function<void(bool, const std::string&)> done);
    std::unique_ptr<platform::SmtcBridge> smtc_;
    std::unique_ptr<platform::AudioActivityMonitor> audio_;
    std::unique_ptr<policy::PlaybackArbiter> arbiter_;
    std::unique_ptr<platform::A2dpSink> a2dp_;
    std::unique_ptr<platform::LoopbackSender> loopback_;
    std::unique_ptr<platform::VolumeControl> volume_;
    platform::VolumeControl::State localVol_;
    platform::VolumeControl::State remoteVol_;
    ScopedTimer volumeTimer_;
    void sendVolumeState();
    media::MediaSnapshot decoratedLocal() const;
    std::string processFor(const media::MediaSessionInfo& s) const;
    void updateSuspension();
    media::MediaSnapshot lastSentLocal_;
    bool lastSentValid_ = false;
    uint64_t localWireGen_ = 0;
    ScopedTimer appVolTimer_;
    bool gated_ = false;
    std::unique_ptr<platform::LyricsService> lyrics_;
    std::unique_ptr<platform::LyricsOverlay> overlay_;
    // One smoothed playback clock for every lyrics surface (overlay, taskbar).
    std::shared_ptr<lyrics::LyricsClock> lyricsClock_ = std::make_shared<lyrics::LyricsClock>();
    bool lyricsSeek_ = false; // the next clock update follows a seek command
    // A remote volume.set waiting for the phone's volume.state or error (never retried).
    struct PendingVolume {
        uint64_t msgId = 0;
        json rid;
    };
    std::optional<PendingVolume> pendingVolume_;
    ScopedTimer volumeAnswerTimer_;
    std::string remoteVolError_; // e.g. background_restricted, until a set succeeds
    std::unique_ptr<platform::TaskbarWidget> taskbar_;
    bool taskbarShown_ = false;
    std::string taskbarReason_ = "disabled";
    std::unique_ptr<TrayIcon> tray_;
    std::unique_ptr<IpcServer> ipc_;
    Diagnostics diag_;

    media::MediaSnapshot local_;
    media::MediaSnapshot remote_;
    bool remoteValid_ = false;
    uint64_t localGenSent_ = 0;
    std::string focusKey_;
    Side focusSide_ = Side::Remote;
    bool focusAuto_ = true;
    std::string lyricsTrack_;
    // Session and song the current lyrics belong to (same-song trackId changes keep them).
    Side lyricsSide_ = Side::Remote;
    std::string lyricsKey_;
    lyrics::Candidate lyricsSong_;
    uint64_t lyricsSongGen_ = 0; // changes per song, not per trackId (lyrics clock key)
    ScopedTimer lyricsGraceTimer_;
    bool lyricsGraceExpired_ = false;
    std::shared_ptr<lyrics::Lyrics> lyricsData_;
    std::set<std::string> lyricsPushed_;
    std::string lastPolicyJson_;
    json lastPolicyStatus_;

    struct Reassembly {
        uint32_t total = 0;
        Bytes data;
        std::set<uint32_t> offsets;
        uint32_t received = 0;
        int64_t startedAt = 0;
    };
    std::map<std::string, Reassembly> incomingArt_;
    std::map<std::string, int64_t> requestedArt_;

    struct Pairing {
        std::string state = "idle";
        std::string sas, peerName, error, kind, target;
        uint64_t btAddress = 0;
        std::optional<link::LanEndpoint> lan;
        link::ConnectionPtr conn;
        uint64_t attempt = 0;
    } pairing_;
    std::map<std::string, platform::BtCandidate> btCandidates_;
    bool discovering_ = false;
    bool standalone_ = false; // settings "mode" == "standalone": this PC only, no phone links
    std::map<std::string, std::string> btPresenceByAddr_;
    bool btAdapter_ = false, btRadio_ = false;

    struct AudioOffer {
        std::string streamId;
        uint64_t offerId = 0;
        Bytes key;
        std::string host;
    };
    std::optional<AudioOffer> audioOffer_;

    ScopedTimer stateTimer_;
    ScopedTimer policyTimer_;
    ScopedTimer diagTimer_;
    ScopedTimer artTimer_;
    bool dirty_ = false;
    bool diagnostics_ = false;
    int64_t uiLaunchedAt_ = 0;
    std::string pendingShow_;
    int64_t lastCommandLatency_ = -1;
    winrt::event_token netToken_{};
    bool exiting_ = false;
};

} // namespace md::app
