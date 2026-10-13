#include "md/policy/PlaybackArbiter.h"
#include "md/util/ManualScheduler.h"

#include "TestStrings.h"

#include <doctest/doctest.h>

using namespace md;
using namespace md::policy;
using media::PlaybackStatus;

namespace {

struct Rig {
    ManualScheduler s;
    std::vector<Action> actions;
    PlaybackArbiter arb{s, [this](const Action& a) { actions.push_back(a); }, [] {}};
    media::MediaSnapshot android, windows;

    explicit Rig(Mode mode = Mode::WindowsFirst) {
        Config c;
        c.mode = mode;
        arb.setConfig(c);
        // Connect: the baseline snapshot (nothing playing yet).
        android.gen = 1;
        androidSession("netease", "com.netease.cloudmusic", PlaybackStatus::Paused);
        arb.onAndroidSnapshot(android);
        windowsSession("spotify", "Spotify.exe", PlaybackStatus::Paused);
        arb.onWindowsSessions(windows);
        actions.clear();
    }
    void androidSession(const std::string& key, const std::string& app, PlaybackStatus st,
                        const std::string& title = "Song") {
        android.sessions.clear();
        media::MediaSessionInfo m;
        m.key = key;
        m.appId = app;
        m.title = title;
        m.status = st;
        m.updateTrackId();
        android.sessions.push_back(m);
        android.active = key;
    }
    void windowsSession(const std::string& key, const std::string& app, PlaybackStatus st) {
        windows.sessions.clear();
        media::MediaSessionInfo m;
        m.key = key;
        m.appId = app;
        m.title = "Win Song";
        m.status = st;
        m.updateTrackId();
        windows.sessions.push_back(m);
        windows.active = key;
    }
    void phone(PlaybackStatus st, const std::string& title = "Song") {
        androidSession("netease", "com.netease.cloudmusic", st, title);
        ++android.gen;
        arb.onAndroidSnapshot(android);
    }
    void pc(PlaybackStatus st) {
        windowsSession("spotify", "Spotify.exe", st);
        arb.onWindowsSessions(windows);
    }
    int count(Action::Kind k) const {
        int n = 0;
        for (auto& a : actions)
            if (a.kind == k) ++n;
        return n;
    }
};

} // namespace

TEST_CASE("windows-first: PC audio pauses the phone, then resumes it when the PC is quiet") {
    Rig r;
    r.phone(PlaybackStatus::Playing);
    CHECK(r.actions.empty());
    r.arb.onWindowsAudio(true, false, {"chrome.exe"});
    REQUIRE(r.count(Action::Kind::PauseAndroid) == 1);
    CHECK(r.actions[0].reason == "windows_audio_active");
    // The phone reports the pause we caused: no new decision, state is POLICY_PAUSED.
    r.phone(PlaybackStatus::Paused);
    CHECK(r.arb.androidState() == PState::PolicyPaused);
    CHECK(r.actions.size() == 1);

    r.arb.onWindowsAudio(false, false, {});
    r.s.advance(4'000);
    CHECK(r.count(Action::Kind::ResumeAndroid) == 0);
    r.s.advance(1'500);
    CHECK(r.count(Action::Kind::ResumeAndroid) == 1);
    // Echo of our resume does not trigger pausing Windows.
    r.phone(PlaybackStatus::Playing);
    CHECK(r.arb.androidState() == PState::Playing);
    CHECK(r.count(Action::Kind::PauseWindows) == 0);
}

TEST_CASE("headset handover in windows-first mode pauses the PC and resumes the phone, once") {
    Rig r(Mode::WindowsFirst);
    r.pc(PlaybackStatus::Playing);
    r.arb.onWindowsAudio(true, false, {"Spotify.exe"});
    r.phone(PlaybackStatus::Playing); // the user presses play on the phone
    CHECK(r.actions.empty());
    r.s.advance(500);
    r.phone(PlaybackStatus::Paused); // the headset pauses it
    REQUIRE(r.count(Action::Kind::PauseWindows) == 1);
    CHECK(r.actions.back().reason == "headset_handover");
    r.pc(PlaybackStatus::Paused);
    r.arb.onWindowsAudio(false, false, {});
    r.s.advance(900);
    CHECK(r.count(Action::Kind::ResumeAndroid) == 1);
    r.phone(PlaybackStatus::Playing);
    CHECK(r.arb.androidState() == PState::Playing);
    // A second bounce inside the cooldown is not handled again (no ping-pong).
    r.pc(PlaybackStatus::Playing);
    r.arb.onWindowsAudio(true, false, {"Spotify.exe"});
    r.phone(PlaybackStatus::Paused);
    r.phone(PlaybackStatus::Playing);
    r.s.advance(300);
    r.phone(PlaybackStatus::Paused);
    r.s.advance(2'000);
    CHECK(r.count(Action::Kind::ResumeAndroid) == 1);
}

TEST_CASE("a late pause (after the bounce window) is a normal user pause") {
    Rig r(Mode::WindowsFirst);
    r.pc(PlaybackStatus::Playing);
    r.arb.onWindowsAudio(true, false, {"Spotify.exe"});
    r.phone(PlaybackStatus::Playing);
    r.s.advance(4'000);
    r.phone(PlaybackStatus::Paused);
    r.s.advance(5'000);
    CHECK(r.actions.empty());
    CHECK(r.arb.androidState() == PState::UserPaused);
}

TEST_CASE("PC bounced by the headset while the phone plays -> phone paused, PC resumed") {
    Rig r(Mode::WindowsFirst);
    r.phone(PlaybackStatus::Playing);
    r.pc(PlaybackStatus::Playing); // the user starts Spotify on the PC
    r.s.advance(400);
    r.pc(PlaybackStatus::Paused); // the headset pauses the PC
    REQUIRE(r.count(Action::Kind::PauseAndroid) == 1);
    r.phone(PlaybackStatus::Paused);
    r.s.advance(900);
    CHECK(r.count(Action::Kind::ResumeWindows) == 1);
}

TEST_CASE("headset assist can be turned off") {
    Rig r(Mode::WindowsFirst);
    Config c = r.arb.config();
    c.headsetAssist = false;
    r.arb.setConfig(c);
    r.pc(PlaybackStatus::Playing);
    r.arb.onWindowsAudio(true, false, {"Spotify.exe"});
    r.phone(PlaybackStatus::Playing);
    r.s.advance(500);
    r.phone(PlaybackStatus::Paused);
    r.s.advance(3'000);
    CHECK(r.actions.empty());
}

TEST_CASE("a user pause through MediaDuet is never mistaken for a headset bounce") {
    Rig r(Mode::WindowsFirst);
    r.pc(PlaybackStatus::Playing);
    r.arb.onWindowsAudio(true, false, {"Spotify.exe"});
    r.phone(PlaybackStatus::Playing);
    r.s.advance(500);
    r.arb.onUserCommand(Side::Android, "netease", media::CommandAction::Pause, false);
    r.phone(PlaybackStatus::Paused);
    r.s.advance(3'000);
    CHECK(r.actions.empty());
}

TEST_CASE("a user pause is never undone") {
    Rig r;
    r.phone(PlaybackStatus::Playing);
    r.phone(PlaybackStatus::Paused); // paused on the phone by the user
    CHECK(r.arb.androidState() == PState::UserPaused);
    r.arb.onWindowsAudio(true, false, {"vlc.exe"});
    r.arb.onWindowsAudio(false, false, {});
    r.s.advance(60'000);
    CHECK(r.actions.empty());
}

TEST_CASE("user pause while policy-paused converts to USER_PAUSED and blocks resume") {
    Rig r;
    r.phone(PlaybackStatus::Playing);
    r.arb.onWindowsAudio(true, false, {"chrome.exe"});
    r.phone(PlaybackStatus::Paused);
    REQUIRE(r.arb.androidState() == PState::PolicyPaused);
    r.arb.onUserCommand(Side::Android, "netease", media::CommandAction::Pause, false);
    CHECK(r.arb.androidState() == PState::UserPaused);
    r.arb.onWindowsAudio(false, false, {});
    r.s.advance(30'000);
    CHECK(r.count(Action::Kind::ResumeAndroid) == 0);
}

TEST_CASE("user resuming the phone during PC audio is respected (no fight)") {
    Rig r;
    r.phone(PlaybackStatus::Playing);
    r.arb.onWindowsAudio(true, false, {"chrome.exe"});
    r.phone(PlaybackStatus::Paused);
    r.phone(PlaybackStatus::Playing); // user pressed play on the phone
    CHECK(r.arb.androidState() == PState::Playing);
    // Windows audio flickers (new onset within the same episode) -> still no re-pause.
    r.arb.onWindowsAudio(true, false, {"chrome.exe"});
    CHECK(r.count(Action::Kind::PauseAndroid) == 1);
    // Next Windows audio episode applies the policy again.
    r.arb.onWindowsAudio(false, false, {});
    r.arb.onWindowsAudio(true, false, {"chrome.exe"});
    CHECK(r.count(Action::Kind::PauseAndroid) == 2);
}

TEST_CASE("bursty PC audio (games) backs off the auto-resume instead of flapping") {
    Rig r;
    r.phone(PlaybackStatus::Playing);
    int64_t expected[] = {5'000, 10'000, 20'000, 40'000};
    for (int64_t delay : expected) {
        r.arb.onWindowsAudio(true, false, {"game.exe"});
        r.phone(PlaybackStatus::Paused);
        r.arb.onWindowsAudio(false, false, {});
        int before = r.count(Action::Kind::ResumeAndroid);
        r.s.advance(delay - 100);
        CHECK(r.count(Action::Kind::ResumeAndroid) == before);
        r.s.advance(200);
        CHECK(r.count(Action::Kind::ResumeAndroid) == before + 1);
        r.phone(PlaybackStatus::Playing); // our resume took effect
        r.s.advance(3'000);               // game makes noise again shortly after
    }
    // Ten quiet minutes later the penalty is forgotten.
    r.s.advance(11 * 60'000);
    CHECK(r.arb.effectiveAndroidResumeDelay() == 5'000);
}

TEST_CASE("resume is cancelled if PC audio returns during the delay") {
    Rig r;
    r.phone(PlaybackStatus::Playing);
    r.arb.onWindowsAudio(true, false, {"chrome.exe"});
    r.phone(PlaybackStatus::Paused);
    r.arb.onWindowsAudio(false, false, {});
    r.s.advance(3'000);
    r.arb.onWindowsAudio(true, false, {"chrome.exe"});
    r.s.advance(30'000);
    CHECK(r.count(Action::Kind::ResumeAndroid) == 0);
}

TEST_CASE("changing track while policy-paused is treated as a user decision") {
    Rig r;
    r.phone(PlaybackStatus::Playing, "A");
    r.arb.onWindowsAudio(true, false, {"chrome.exe"});
    r.phone(PlaybackStatus::Paused, "A");
    r.phone(PlaybackStatus::Paused, "B");
    r.arb.onWindowsAudio(false, false, {});
    r.s.advance(30'000);
    CHECK(r.count(Action::Kind::ResumeAndroid) == 0);
}

TEST_CASE("android-first: phone playback pauses Windows sessions and resumes them later") {
    Rig r(Mode::AndroidFirst);
    r.pc(PlaybackStatus::Playing);
    r.arb.onWindowsAudio(true, false, {"Spotify.exe"});
    CHECK(r.actions.empty()); // Windows onset never pauses the phone in this mode
    r.phone(PlaybackStatus::Playing);
    REQUIRE(r.count(Action::Kind::PauseWindows) == 1);
    CHECK(r.actions.back().key == "spotify");
    r.pc(PlaybackStatus::Paused);
    CHECK(r.arb.stateOf(Side::Windows, "spotify") == PState::PolicyPaused);
    r.arb.onWindowsAudio(false, false, {});
    r.phone(PlaybackStatus::Paused); // user stops the phone
    r.s.advance(6'000);
    CHECK(r.count(Action::Kind::ResumeWindows) == 1);
    r.pc(PlaybackStatus::Playing);
    r.arb.onWindowsAudio(true, false, {"Spotify.exe"});
    CHECK(r.count(Action::Kind::PauseAndroid) == 0);
}

TEST_CASE("latest source wins in both directions") {
    Rig r(Mode::Latest);
    r.phone(PlaybackStatus::Playing);
    r.arb.onWindowsAudio(true, false, {"chrome.exe"});
    CHECK(r.count(Action::Kind::PauseAndroid) == 1);
    r.phone(PlaybackStatus::Paused);
    r.pc(PlaybackStatus::Playing);
    r.phone(PlaybackStatus::Playing); // user resumes phone: now the phone is the latest
    CHECK(r.count(Action::Kind::PauseWindows) == 1);
}

TEST_CASE("custom priorities") {
    Rig r(Mode::Custom);
    Config c = r.arb.config();
    c.priorities = {{"chrome.exe", 1}, {"com.netease.cloudmusic", 5}, {"zoom.exe", 10}};
    r.arb.setConfig(c);
    r.phone(PlaybackStatus::Playing);
    r.arb.onWindowsAudio(true, false, {"chrome.exe"});
    CHECK(r.count(Action::Kind::PauseAndroid) == 0); // phone app has higher priority
    r.arb.onWindowsAudio(false, false, {});
    r.arb.onWindowsAudio(true, false, {"zoom.exe"});
    CHECK(r.count(Action::Kind::PauseAndroid) == 1);
}

TEST_CASE("calls on the PC pause the phone even in android-first mode") {
    Rig r(Mode::AndroidFirst);
    r.phone(PlaybackStatus::Playing);
    r.arb.onWindowsAudio(true, true, {"Teams.exe"});
    REQUIRE(r.count(Action::Kind::PauseAndroid) == 1);
    CHECK(r.actions.back().reason == "call_active");
}

TEST_CASE("off and suspended modes never act") {
    Rig r(Mode::Off);
    r.phone(PlaybackStatus::Playing);
    r.arb.onWindowsAudio(true, true, {"Teams.exe"});
    CHECK(r.actions.empty());

    Rig s;
    s.arb.setSuspended(true);
    s.phone(PlaybackStatus::Playing);
    s.arb.onWindowsAudio(true, false, {"chrome.exe"});
    CHECK(s.actions.empty());
}

TEST_CASE("a policy pause that never takes effect is not retried") {
    Rig r;
    r.phone(PlaybackStatus::Playing);
    r.arb.onWindowsAudio(true, false, {"chrome.exe"});
    r.s.advance(10'000);
    CHECK(r.arb.lastAction().failed);
    CHECK(r.count(Action::Kind::PauseAndroid) == 1);
    // A later snapshot that still says "playing" is not an onset either.
    r.phone(PlaybackStatus::Playing);
    CHECK(r.count(Action::Kind::PauseAndroid) == 1);
}

TEST_CASE("first snapshot after connecting is a baseline, not an onset") {
    ManualScheduler s;
    std::vector<Action> acts;
    PlaybackArbiter arb(s, [&](const Action& a) { acts.push_back(a); }, [] {});
    Config c;
    c.mode = Mode::AndroidFirst;
    arb.setConfig(c);
    media::MediaSnapshot w;
    media::MediaSessionInfo ws;
    ws.key = "spotify";
    ws.status = PlaybackStatus::Playing;
    w.sessions.push_back(ws);
    arb.onWindowsSessions(w);
    media::MediaSnapshot a;
    media::MediaSessionInfo as;
    as.key = "netease";
    as.status = PlaybackStatus::Playing;
    a.sessions.push_back(as);
    a.active = "netease";
    arb.onAndroidSnapshot(a);
    CHECK(acts.empty());
}

TEST_CASE("link loss forgets phone state and never resumes Windows by itself") {
    Rig r(Mode::AndroidFirst);
    r.pc(PlaybackStatus::Playing);
    r.phone(PlaybackStatus::Playing);
    r.pc(PlaybackStatus::Paused);
    r.arb.onAndroidLinkDown();
    r.s.advance(60'000);
    CHECK(r.count(Action::Kind::ResumeWindows) == 0);
    CHECK(r.arb.androidState() == PState::Unknown);
}

TEST_CASE("remote user pause is recorded as REMOTE_PAUSED") {
    Rig r;
    r.pc(PlaybackStatus::Playing);
    r.arb.onUserCommand(Side::Windows, "spotify", media::CommandAction::Pause, true);
    r.pc(PlaybackStatus::Paused);
    CHECK(r.arb.stateOf(Side::Windows, "spotify") == PState::RemotePaused);
}
