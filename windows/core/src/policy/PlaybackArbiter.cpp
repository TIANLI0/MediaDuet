#include "md/policy/PlaybackArbiter.h"

#include "md/util/Log.h"
#include "md/util/Text.h"

#include <algorithm>

namespace md::policy {

using media::PlaybackStatus;

const char* toString(Mode m) {
    switch (m) {
    case Mode::Off: return "off";
    case Mode::AndroidFirst: return "android_first";
    case Mode::Latest: return "latest";
    case Mode::Custom: return "custom";
    default: return "windows_first";
    }
}

Mode modeFromString(std::string_view s) {
    if (s == "off") return Mode::Off;
    if (s == "android_first") return Mode::AndroidFirst;
    if (s == "latest") return Mode::Latest;
    if (s == "custom") return Mode::Custom;
    return Mode::WindowsFirst;
}

const char* toString(PState s) {
    switch (s) {
    case PState::Playing: return "playing";
    case PState::UserPaused: return "user_paused";
    case PState::PolicyPaused: return "policy_paused";
    case PState::RemotePaused: return "remote_paused";
    case PState::Stopped: return "stopped";
    default: return "unknown";
    }
}

static bool isPausedLike(PlaybackStatus s) { return s == PlaybackStatus::Paused || s == PlaybackStatus::Stopped; }

PlaybackArbiter::PlaybackArbiter(IScheduler& sched, std::function<void(const Action&)> execute,
                                 std::function<void()> changed)
    : sched_(sched), exec_(std::move(execute)), changed_(std::move(changed)), resumeAndroid_(&sched),
      resumeWindows_(&sched), expectTimer_(&sched), handoverTimer_(&sched) {}

static std::string sideKey(Side s, const std::string& key) { return (s == Side::Windows ? "w|" : "a|") + key; }

void PlaybackArbiter::setConfig(Config cfg) {
    bool turnedOff = cfg.mode == Mode::Off && cfg_.mode != Mode::Off;
    cfg_ = std::move(cfg);
    if (turnedOff) {
        resumeAndroid_.cancel();
        resumeWindows_.cancel();
        expects_.clear();
    }
    if (changed_) changed_();
}

void PlaybackArbiter::setSuspended(bool s) {
    if (suspended_ == s) return;
    suspended_ = s;
    if (s) {
        resumeAndroid_.cancel();
        resumeWindows_.cancel();
    }
    if (changed_) changed_();
}

int PlaybackArbiter::priorityOf(const std::string& app) const {
    auto lower = text::toLowerAscii(app);
    for (auto& [k, v] : cfg_.priorities)
        if (text::toLowerAscii(k) == lower) return v;
    return 0;
}

int PlaybackArbiter::windowsPriority() const {
    int best = INT32_MIN;
    for (auto& a : winApps_) best = std::max(best, priorityOf(a));
    for (auto& [k, t] : windows_)
        if (t.status == PlaybackStatus::Playing) best = std::max(best, priorityOf(t.appId));
    return best == INT32_MIN ? 0 : best;
}

bool PlaybackArbiter::androidPlaying(std::string* key) const {
    for (auto& [k, t] : android_)
        if (t.status == PlaybackStatus::Playing) {
            if (key) *key = k;
            return true;
        }
    return false;
}

PState PlaybackArbiter::stateOf(Side side, const std::string& key) const {
    const auto& m = side == Side::Windows ? windows_ : android_;
    auto it = m.find(key);
    return it == m.end() ? PState::Unknown : it->second.state;
}

PState PlaybackArbiter::androidState() const {
    if (androidActive_) return stateOf(Side::Android, *androidActive_);
    return PState::Unknown;
}

void PlaybackArbiter::record(std::string what, std::string reason, std::string app, bool failed) {
    last_ = {std::move(what), std::move(reason), std::move(app), sched_.nowMs(), failed};
    log::info("policy", "{} ({}{}{}){}", last_.what, last_.reason, last_.app.empty() ? "" : ", ", last_.app,
              failed ? " FAILED" : "");
}

// ------------------------------------------------------------------ observations

void PlaybackArbiter::onWindowsAudio(bool active, bool communication, std::vector<std::string> apps) {
    bool wasActive = winAudio_;
    bool wasComm = winComm_;
    winAudio_ = active;
    winComm_ = active && communication;
    winApps_ = std::move(apps);
    if (active && (!wasActive || (winComm_ && !wasComm))) {
        windowsOnset(winComm_ && !wasComm);
    } else if (!active && wasActive) {
        windowsOffset();
    }
    if (changed_) changed_();
}

void PlaybackArbiter::onWindowsSessions(const media::MediaSnapshot& snap) { applySnapshot(Side::Windows, snap); }

void PlaybackArbiter::onAndroidSnapshot(const media::MediaSnapshot& snap) {
    androidActive_ = snap.active;
    // The first snapshot after (re)connecting describes existing state, not new activity.
    baseline_ = !androidSeen_;
    androidSeen_ = true;
    applySnapshot(Side::Android, snap);
    baseline_ = false;
}

void PlaybackArbiter::onAndroidLinkDown() {
    // We can no longer observe or control the phone; forget its state, but do not
    // resume anything on Windows: the phone may still be playing.
    android_.clear();
    androidActive_.reset();
    androidSeen_ = false;
    resumeAndroid_.cancel();
    expects_.erase(std::remove_if(expects_.begin(), expects_.end(),
                                  [](const Expect& e) { return e.side == Side::Android; }),
                   expects_.end());
    if (changed_) changed_();
}

void PlaybackArbiter::onUserCommand(Side side, const std::string& key, media::CommandAction action, bool remote) {
    intents_.push_back({side, key, action, remote, sched_.nowMs()});
    if (intents_.size() > 16) intents_.erase(intents_.begin());
    auto& m = map(side);
    auto it = m.find(key);
    if (it == m.end()) return;
    // A user decision about a policy-paused session ends our claim on it.
    if (it->second.state == PState::PolicyPaused &&
        (action == media::CommandAction::Pause || action == media::CommandAction::Stop)) {
        it->second.state = remote ? PState::RemotePaused : PState::UserPaused;
        if (side == Side::Android) resumeAndroid_.cancel();
        else resumeWindows_.cancel();
        if (changed_) changed_();
    }
}

void PlaybackArbiter::applySnapshot(Side side, const media::MediaSnapshot& snap) {
    auto& m = map(side);
    // Sessions that disappeared.
    for (auto it = m.begin(); it != m.end();) {
        if (!snap.find(it->first)) it = m.erase(it);
        else ++it;
    }
    for (auto& s : snap.sessions) {
        auto [it, inserted] = m.try_emplace(s.key);
        Tracked& t = it->second;
        PlaybackStatus from = t.status;
        bool trackChanged = !inserted && t.trackId != s.trackId;
        t.appId = s.appId.empty() ? s.key : s.appId;
        t.trackId = s.trackId;
        if (inserted) {
            t.status = s.status;
            t.state = s.status == PlaybackStatus::Playing ? PState::Playing
                      : isPausedLike(s.status)            ? PState::UserPaused
                                                          : PState::Unknown;
            if (s.status == PlaybackStatus::Playing && side == Side::Android && !baseline_)
                androidOnset(s.key, false);
            continue;
        }
        // Skipping to another track while policy-paused is a user decision.
        if (trackChanged && t.state == PState::PolicyPaused && s.status != PlaybackStatus::Playing) {
            t.state = PState::UserPaused;
        }
        if (s.status != from) {
            t.status = s.status;
            onTransition(side, s.key, t, from, s.status);
        }
    }
    checkExpectations();
    if (changed_) changed_();
}

bool PlaybackArbiter::consumeExpect(Side side, const std::string& key, bool playing, std::string* reason) {
    for (auto it = expects_.begin(); it != expects_.end(); ++it) {
        if (it->side == side && it->key == key && it->playing == playing) {
            if (reason) *reason = it->reason;
            expects_.erase(it);
            return true;
        }
    }
    return false;
}

std::optional<PlaybackArbiter::Intent> PlaybackArbiter::consumeIntent(Side side, const std::string& key,
                                                                      bool playing) {
    int64_t now = sched_.nowMs();
    for (auto it = intents_.rbegin(); it != intents_.rend(); ++it) {
        if (it->side != side || it->key != key || now - it->at > cfg_.intentWindowMs) continue;
        bool wantsPlay = it->action == media::CommandAction::Play ||
                         (it->action == media::CommandAction::Toggle && playing);
        bool wantsPause = it->action == media::CommandAction::Pause || it->action == media::CommandAction::Stop ||
                          (it->action == media::CommandAction::Toggle && !playing);
        if ((playing && wantsPlay) || (!playing && wantsPause)) {
            Intent i = *it;
            intents_.erase(std::next(it).base());
            return i;
        }
    }
    return std::nullopt;
}

void PlaybackArbiter::onTransition(Side side, const std::string& key, Tracked& t, PlaybackStatus from,
                                   PlaybackStatus to) {
    (void)from;
    if (to == PlaybackStatus::Playing) {
        std::string reason;
        if (consumeExpect(side, key, true, &reason)) {
            // Our own resume took effect: not a user onset.
            t.state = PState::Playing;
            t.reason = reason;
            if (side == Side::Windows) windowsResumedAt_ = sched_.nowMs();
            return;
        }
        consumeIntent(side, key, true);
        userOnsetAt_[sideKey(side, key)] = sched_.nowMs();
        bool resumedPolicyPause = t.state == PState::PolicyPaused;
        t.state = PState::Playing;
        t.reason = "user";
        if (side == Side::Android) {
            androidOnset(key, resumedPolicyPause);
        } else if (resumedPolicyPause) {
            // User resumed a Windows session we paused: stop trying to resume it ourselves.
            resumeWindows_.cancel();
        }
        return;
    }
    if (isPausedLike(to)) {
        std::string reason;
        if (consumeExpect(side, key, false, &reason)) {
            t.state = PState::PolicyPaused;
            t.reason = reason;
            return;
        }
        auto intent = consumeIntent(side, key, false);
        t.state = intent && intent->remote ? PState::RemotePaused : PState::UserPaused;
        t.reason = intent ? (intent->remote ? "remote_user" : "user") : "external";
        if (!intent && tryHeadsetHandover(side, key, t)) return;
        if (side == Side::Android) {
            resumeAndroid_.cancel();
            if (!androidPlaying()) androidOffset();
        }
    }
}

// ------------------------------------------------------------------ headset handover

bool PlaybackArbiter::tryHeadsetHandover(Side side, const std::string& key, Tracked& t) {
    if (!cfg_.headsetAssist || suspended_ || cfg_.mode == Mode::Off) return false;
    int64_t now = sched_.nowMs();
    std::string sk = sideKey(side, key);
    auto onset = userOnsetAt_.find(sk);
    if (onset == userOnsetAt_.end() || now - onset->second > cfg_.bounceWindowMs) return false;
    if (auto last = lastHandover_.find(sk); last != lastHandover_.end() && now - last->second < cfg_.handoverCooldownMs)
        return false; // never loop: one attempt per cooldown
    // The other device must hold the headset with something we can pause.
    std::vector<std::string> others;
    if (side == Side::Android) {
        for (auto& [k, w] : windows_)
            if (w.status == PlaybackStatus::Playing) others.push_back(k);
    } else {
        std::string ak;
        if (androidPlaying(&ak)) others.push_back(ak);
    }
    if (others.empty()) return false;
    lastHandover_[sk] = now;
    userOnsetAt_.erase(sk);
    record("headset_handover", side == Side::Android ? "phone_bounced_by_headset" : "pc_bounced_by_headset", t.appId);
    Side os = side == Side::Android ? Side::Windows : Side::Android;
    for (auto& k : others) {
        expects_.push_back({os, k, false, now + cfg_.confirmTimeoutMs, "headset_handover"});
        exec_({os == Side::Android ? Action::Kind::PauseAndroid : Action::Kind::PauseWindows, k, "headset_handover"});
    }
    // Give the headset a moment to switch, then resume what the user started.
    t.state = PState::PolicyPaused;
    t.reason = "headset_handover";
    handoverTimer_.start(cfg_.handoverResumeDelayMs, [this, side, key] {
        auto& m = map(side);
        auto it = m.find(key);
        if (it == m.end() || it->second.state != PState::PolicyPaused || it->second.reason != "headset_handover") return;
        expects_.push_back({side, key, true, sched_.nowMs() + cfg_.confirmTimeoutMs, "headset_handover"});
        exec_({side == Side::Android ? Action::Kind::ResumeAndroid : Action::Kind::ResumeWindows, key, "headset_handover"});
        checkExpectations();
        if (changed_) changed_();
    });
    checkExpectations();
    return true;
}

// ------------------------------------------------------------------ decisions

void PlaybackArbiter::windowsOnset(bool becameCall) {
    resumeAndroid_.cancel();
    if (suspended_ || cfg_.mode == Mode::Off) return;
    if (!becameCall && sched_.nowMs() - windowsResumedAt_ < cfg_.echoSuppressMs) return; // our own resume
    std::string key;
    if (!androidPlaying(&key)) return;
    std::string app = winApps_.empty() ? std::string() : winApps_.front();
    if (winComm_ && cfg_.pauseForCalls) {
        overrideAndroid_ = false;
        pauseAndroid(key, "call_active", app);
        return;
    }
    if (overrideAndroid_) return;
    bool pause = false;
    switch (cfg_.mode) {
    case Mode::WindowsFirst:
    case Mode::Latest: pause = true; break;
    case Mode::AndroidFirst: pause = false; break;
    case Mode::Custom: pause = windowsPriority() >= priorityOf(android_[key].appId); break;
    case Mode::Off: break;
    }
    if (pause) pauseAndroid(key, "windows_audio_active", app);
}

void PlaybackArbiter::windowsOffset() {
    overrideAndroid_ = false;
    scheduleAndroidResume();
}

void PlaybackArbiter::androidOnset(const std::string& key, bool userResumedPolicyPause) {
    resumeWindows_.cancel();
    if (suspended_ || cfg_.mode == Mode::Off) return;
    if (winAudio_ && userResumedPolicyPause) {
        // The user deliberately resumed the phone while Windows is playing: respect it
        // for the rest of this Windows audio episode.
        overrideAndroid_ = true;
    }
    bool pause = false;
    switch (cfg_.mode) {
    case Mode::AndroidFirst:
    case Mode::Latest: pause = true; break;
    case Mode::WindowsFirst: pause = false; break;
    case Mode::Custom: pause = priorityOf(android_[key].appId) >= windowsPriority(); break;
    case Mode::Off: break;
    }
    if (winComm_ && cfg_.pauseForCalls) pause = false; // never interrupt a call on the PC
    if (pause) pauseWindowsSessions("android_playing", android_[key].appId);
}

void PlaybackArbiter::androidOffset() { scheduleWindowsResume(); }

int64_t PlaybackArbiter::effectiveAndroidResumeDelay() {
    int64_t now = sched_.nowMs();
    androidFlaps_.erase(std::remove_if(androidFlaps_.begin(), androidFlaps_.end(),
                                       [&](int64_t t) { return now - t > cfg_.flapMemoryMs; }),
                        androidFlaps_.end());
    int shift = std::min<int>(static_cast<int>(androidFlaps_.size()), 10);
    return std::min(cfg_.maxResumeDelayMs, std::max<int64_t>(cfg_.resumeDelayMs, 0) << shift);
}

void PlaybackArbiter::pauseAndroid(const std::string& key, const std::string& reason, const std::string& app) {
    auto& t = android_[key];
    if (t.status != PlaybackStatus::Playing) return;
    // Re-pausing right after our own resume means the PC audio is bursty: back off.
    if (sched_.nowMs() - androidResumedAt_ < cfg_.flapWindowMs) androidFlaps_.push_back(sched_.nowMs());
    expects_.push_back({Side::Android, key, false, sched_.nowMs() + cfg_.confirmTimeoutMs, reason});
    record("pause_remote", reason, app);
    exec_({Action::Kind::PauseAndroid, key, reason});
    checkExpectations();
}

void PlaybackArbiter::pauseWindowsSessions(const std::string& reason, const std::string& app) {
    bool any = false;
    for (auto& [k, t] : windows_) {
        if (t.status != PlaybackStatus::Playing) continue;
        expects_.push_back({Side::Windows, k, false, sched_.nowMs() + cfg_.confirmTimeoutMs, reason});
        exec_({Action::Kind::PauseWindows, k, reason});
        any = true;
    }
    if (any) record("pause_local", reason, app);
    checkExpectations();
}

void PlaybackArbiter::scheduleAndroidResume() {
    if (!cfg_.autoResume || suspended_ || cfg_.mode == Mode::Off) return;
    bool any = false;
    for (auto& [k, t] : android_)
        if (t.state == PState::PolicyPaused) any = true;
    if (!any) return;
    resumeAndroid_.start(effectiveAndroidResumeDelay(), [this] {
        if (winAudio_ || suspended_ || cfg_.mode == Mode::Off) return;
        // Resume only the session we paused, and only if it is still exactly as we left it.
        for (auto& [k, t] : android_) {
            if (t.state != PState::PolicyPaused || !isPausedLike(t.status)) continue;
            if (androidActive_ && *androidActive_ != k) continue;
            expects_.push_back({Side::Android, k, true, sched_.nowMs() + cfg_.confirmTimeoutMs, "windows_idle"});
            androidResumedAt_ = sched_.nowMs();
            record("resume_remote", "windows_idle", t.appId);
            exec_({Action::Kind::ResumeAndroid, k, "windows_idle"});
            break;
        }
        checkExpectations();
        if (changed_) changed_();
    });
}

void PlaybackArbiter::scheduleWindowsResume() {
    if (!cfg_.autoResume || suspended_ || cfg_.mode == Mode::Off) return;
    bool any = false;
    for (auto& [k, t] : windows_)
        if (t.state == PState::PolicyPaused) any = true;
    if (!any) return;
    resumeWindows_.start(cfg_.resumeDelayMs, [this] {
        if (androidPlaying() || suspended_ || cfg_.mode == Mode::Off) return;
        for (auto& [k, t] : windows_) {
            if (t.state != PState::PolicyPaused || !isPausedLike(t.status)) continue;
            expects_.push_back({Side::Windows, k, true, sched_.nowMs() + cfg_.confirmTimeoutMs, "android_idle"});
            record("resume_local", "android_idle", t.appId);
            exec_({Action::Kind::ResumeWindows, k, "android_idle"});
        }
        checkExpectations();
        if (changed_) changed_();
    });
}

void PlaybackArbiter::checkExpectations() {
    int64_t now = sched_.nowMs();
    for (auto it = expects_.begin(); it != expects_.end();) {
        if (it->deadline <= now) {
            log::warn("policy", "{} of {} did not take effect", it->playing ? "resume" : "pause", it->key);
            last_.failed = true;
            it = expects_.erase(it);
        } else {
            ++it;
        }
    }
    if (expects_.empty()) {
        expectTimer_.cancel();
        return;
    }
    int64_t next = INT64_MAX;
    for (auto& e : expects_) next = std::min(next, e.deadline);
    expectTimer_.start(next - now, [this] {
        checkExpectations();
        if (changed_) changed_();
    });
}

} // namespace md::policy
