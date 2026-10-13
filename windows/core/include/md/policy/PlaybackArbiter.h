#pragma once

#include "md/media/MediaTypes.h"
#include "md/util/Scheduler.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace md::policy {

enum class Mode { Off, WindowsFirst, AndroidFirst, Latest, Custom };
const char* toString(Mode m);
Mode modeFromString(std::string_view s);

// Per-session arbitration state.
enum class PState { Unknown, Playing, UserPaused, PolicyPaused, RemotePaused, Stopped };
const char* toString(PState s);

enum class Side { Windows, Android };

struct Config {
    Mode mode = Mode::WindowsFirst;
    bool autoResume = true;
    int64_t resumeDelayMs = 5'000;
    bool pauseForCalls = true;
    std::map<std::string, int> priorities; // app id / process name / package -> priority
    int64_t confirmTimeoutMs = 4'000;      // how long a policy command may take to show effect
    int64_t echoSuppressMs = 3'000;        // ignore Windows audio onsets caused by our own resume
    int64_t intentWindowMs = 4'000;        // how long a user command explains the next transition
    // Flap damping: a policy pause shortly after our own resume (bursty game/UI audio)
    // doubles the next resume delay, up to maxResumeDelayMs; forgotten after flapMemoryMs.
    int64_t flapWindowMs = 60'000;
    int64_t flapMemoryMs = 10 * 60'000;
    int64_t maxResumeDelayMs = 120'000;
    // Multipoint headset assist: a session the user just started that is paused by
    // something else within bounceWindowMs while the other device plays is assumed
    // to have been bounced by the headset; we hand the headset over to it.
    bool headsetAssist = true;
    int64_t bounceWindowMs = 2'500;
    int64_t handoverResumeDelayMs = 800;
    int64_t handoverCooldownMs = 10'000;
};

struct Action {
    enum class Kind { PauseAndroid, ResumeAndroid, PauseWindows, ResumeWindows };
    Kind kind;
    std::string key;
    std::string reason;
};

struct LastAction {
    std::string what;   // e.g. "pause_remote"
    std::string reason; // e.g. "windows_audio_active"
    std::string app;
    int64_t atMs = 0;
    bool failed = false;
};

// Decides when to pause/resume media on either device (docs/architecture.md).
// Inputs are observations; outputs are Actions executed by the host. All state
// changes are attributed (user / policy / remote) so that echoes of our own
// commands never trigger new decisions and user pauses are never undone.
class PlaybackArbiter {
public:
    PlaybackArbiter(IScheduler& sched, std::function<void(const Action&)> execute, std::function<void()> changed);

    void setConfig(Config cfg);
    const Config& config() const { return cfg_; }
    // While suspended (e.g. phone audio is routed through this PC) no decisions are made.
    void setSuspended(bool suspended);
    bool suspended() const { return suspended_; }

    // Debounced aggregate of Windows audio activity (Core Audio), excluding MediaDuet itself.
    void onWindowsAudio(bool active, bool communication, std::vector<std::string> apps);
    // Windows media sessions (GSMTC), excluding MediaDuet's own SMTC bridge.
    void onWindowsSessions(const media::MediaSnapshot& snap);
    // Android media sessions as reported by the phone.
    void onAndroidSnapshot(const media::MediaSnapshot& snap);
    void onAndroidLinkDown();
    // A person issued a command through MediaDuet (local UI, SMTC bridge, or the other device).
    void onUserCommand(Side side, const std::string& key, media::CommandAction action, bool fromRemoteDevice);

    PState stateOf(Side side, const std::string& key) const;
    PState androidState() const;
    bool windowsAudioActive() const { return winAudio_; }
    const std::vector<std::string>& windowsAudioApps() const { return winApps_; }
    const LastAction& lastAction() const { return last_; }
    // Resume delay currently in effect for the phone (grows while audio flaps).
    int64_t effectiveAndroidResumeDelay();

private:
    struct Tracked {
        PState state = PState::Unknown;
        media::PlaybackStatus status = media::PlaybackStatus::Unknown;
        std::string appId;
        std::string trackId;
        std::string reason;
    };
    struct Expect {
        Side side;
        std::string key;
        bool playing;
        int64_t deadline;
        std::string reason;
    };
    struct Intent {
        Side side;
        std::string key;
        media::CommandAction action;
        bool remote;
        int64_t at;
    };

    std::map<std::string, Tracked>& map(Side s) { return s == Side::Windows ? windows_ : android_; }
    void applySnapshot(Side side, const media::MediaSnapshot& snap);
    void onTransition(Side side, const std::string& key, Tracked& t, media::PlaybackStatus from,
                      media::PlaybackStatus to);
    bool consumeExpect(Side side, const std::string& key, bool playing, std::string* reason);
    std::optional<Intent> consumeIntent(Side side, const std::string& key, bool playing);
    void windowsOnset(bool becameCall);
    void windowsOffset();
    void androidOnset(const std::string& key, bool userResumedPolicyPause);
    void androidOffset();
    void pauseAndroid(const std::string& key, const std::string& reason, const std::string& app);
    void pauseWindowsSessions(const std::string& reason, const std::string& app);
    void scheduleAndroidResume();
    void scheduleWindowsResume();
    void checkExpectations();
    bool androidPlaying(std::string* key = nullptr) const;
    int priorityOf(const std::string& app) const;
    int windowsPriority() const;
    void record(std::string what, std::string reason, std::string app, bool failed = false);
    bool tryHeadsetHandover(Side side, const std::string& key, Tracked& t);

    IScheduler& sched_;
    std::function<void(const Action&)> exec_;
    std::function<void()> changed_;
    Config cfg_;
    bool suspended_ = false;
    std::map<std::string, Tracked> windows_, android_;
    std::optional<std::string> androidActive_;
    bool winAudio_ = false;
    bool winComm_ = false;
    std::vector<std::string> winApps_;
    bool overrideAndroid_ = false; // user resumed the phone during the current Windows audio episode
    bool androidSeen_ = false;     // a snapshot was received since the link came up
    bool baseline_ = false;        // applying the first snapshot: existing state, not onsets
    int64_t windowsResumedAt_ = INT64_MIN / 2;
    int64_t androidResumedAt_ = INT64_MIN / 2;
    std::map<std::string, int64_t> userOnsetAt_;  // "<side>|<key>" -> when the user started it
    std::map<std::string, int64_t> lastHandover_; // "<side>|<key>" -> last handover attempt
    ScopedTimer handoverTimer_;
    std::vector<int64_t> androidFlaps_;
    std::vector<Expect> expects_;
    std::vector<Intent> intents_;
    ScopedTimer resumeAndroid_;
    ScopedTimer resumeWindows_;
    ScopedTimer expectTimer_;
    LastAction last_;
};

} // namespace md::policy
