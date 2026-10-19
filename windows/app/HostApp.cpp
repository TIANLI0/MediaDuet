#include "HostApp.h"

#include "Autostart.h"
#include "AppIdentity.h"
#include "Strings.h"

#include "md/crypto/Crypto.h"
#include "md/util/AppVersion.h"
#include "md/util/Log.h"
#include "md/util/Text.h"

#include <shellapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Connectivity.h>

namespace md::app {

using media::CommandAction;
using media::MediaSessionInfo;
using media::PlaybackStatus;

namespace {

constexpr int64_t kStateCoalesceMs = 100;
// How long lyrics survive the focused session's metadata going blank (updateFocus).
constexpr int64_t kLyricsGraceMs = 1500;
constexpr size_t kChunk = 32 * 1024;

std::vector<std::string> baseCaps() {
    return {proto::caps::MediaSource, proto::caps::MediaControl, proto::caps::Artwork, proto::caps::Lyrics,
            proto::caps::PolicyHost,  proto::caps::SmtcBridge,   proto::caps::AudioA2dpSink,
            proto::caps::AudioSendPcm, proto::caps::Volume, proto::caps::Handoff};
}

std::string osVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{sizeof(v)};
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        fn(&v);
    return std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." +
           std::to_string(v.dwBuildNumber);
}

std::filesystem::path exeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

const char* sideName(int s) { return s == 0 ? "local" : "remote"; }

} // namespace

HostApp::HostApp() = default;
HostApp::~HostApp() = default;

// ======================================================================= lifecycle

int HostApp::run(bool background, const std::string& showWindow) {
    init();
    if (!background) openUi(showWindow.empty() ? "main" : showWindow);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    shutdown();
    return 0;
}

void HostApp::init() {
    auto dir = platform::appDataDir();
    wchar_t lvl[16] = {};
    bool debugLog = GetEnvironmentVariableW(L"MEDIADUET_LOG", lvl, 16) > 0 && std::wstring(lvl) == L"debug";
    log::init((dir / L"logs").wstring(), debugLog ? log::Level::Debug : log::Level::Info);
    log::info("host", "MediaDuet {} starting on Windows {}", MD_VERSION, osVersion());
    settingsPath_ = dir / L"settings.json";
    settings_ = settings::Settings::load(settingsPath_);
    setLanguage(settings_.get<std::string>("language", "auto"));
    identity_ = platform::loadOrCreateIdentity();
    // Before SMTC registers: lets Windows name our media session "MediaDuet".
    platform::ensureAppIdentity(L"MediaDuet");
    log::info("host", "app id resolves to '{}'", text::toUtf8(platform::resolvedAppName()));
    devices_.load();
    standalone_ = settings_.get<std::string>("mode", "linked") == "standalone";
    // Work mode (Settings > General): installs that already have a paired phone keep using
    // it ("linked") without being asked; a fresh install lets the UI ask once.
    if (!settings_.get<bool>("modeChosen", false) && !devices_.devices().empty()) {
        settings_.apply({{"modeChosen", true}});
        settings_.save(settingsPath_);
    }

    stateTimer_.bind(&sched_);
    policyTimer_.bind(&sched_);
    diagTimer_.bind(&sched_);
    artTimer_.bind(&sched_);
    volumeTimer_.bind(&sched_);
    appVolTimer_.bind(&sched_);
    volumeAnswerTimer_.bind(&sched_);
    lyricsGraceTimer_.bind(&sched_);

    art_ = std::make_unique<platform::ArtworkCache>(platform::cacheDir() / L"art");
    art_->prune();
    factory_ = std::make_unique<platform::TransportFactory>(sched_);
    proto::DeviceInfo self{identity_.id, identity_.name, "windows", "PC", osVersion(), MD_VERSION};
    cm_ = std::make_unique<link::ConnectionManager>(sched_, *factory_, *this, self, baseCaps());

    tray_ = std::make_unique<TrayIcon>(TrayIcon::Callbacks{
        [this] { openUi("main"); },
        [this](const std::string& c) {
            if (c == "open") openUi("main");
            else if (c == "compact") openUi("compact");
            else if (c == "settings") openUi("settings");
            else if (c == "lyrics") {
                settings_.apply({{"lyrics", {{"overlay", {{"visible", !overlay_->enabled()}}}}}});
                settings_.save(settingsPath_);
                overlay_->setEnabled(!overlay_->enabled());
                markDirty();
            } else if (c == "a2dp") {
                setA2dp(!a2dp_->active());
            } else if (c == "toggle" || c == "next" || c == "previous") {
                Side s;
                if (auto* f = focused(&s))
                    sendUserCommand(s, f->key, *media::actionFromString(c), std::nullopt, "tray");
            } else if (c.rfind("mode:", 0) == 0) {
                settings_.apply({{"policy", {{"mode", c.substr(5)}}}});
                settings_.save(settingsPath_);
                applySettings();
            } else if (c == "exit") {
                exitApp();
            } else if (c == "update") {
                // Settings > About shows the progress and any error.
                if (updater_) updater_->install();
                openUi("settings");
            }
        },
        [this] {
            TrayMenuState st;
            auto ls = cm_->status();
            using S = link::LinkStatus::State;
            std::wstring s = standalone()                 ? tr("status.standalone")
                             : ls.state == S::NoPeer        ? tr("status.no_peer")
                             : ls.state == S::Connecting   ? tr("status.connecting")
                             : ls.state == S::Disconnected ? tr("status.disconnected")
                             : (ls.primary == link::TransportKind::Bluetooth ? tr("status.bt") : tr("status.lan"));
            if (ls.state == S::Connected && !standalone()) s += L" · " + text::toWide(ls.peerName);
            st.status = s;
            st.standalone = standalone();
            st.lyricsOn = overlay_->enabled();
            st.policyMode = settings_.get<std::string>("policy.mode", "windows_first");
            st.canControl = focused() != nullptr;
            st.a2dpAvailable = !standalone() && (a2dp_->status().available || a2dp_->active());
            st.a2dpOn = a2dp_->active();
            if (updater_ && updater_->phase() == platform::Updater::Phase::Available) {
                wchar_t buf[96];
                swprintf_s(buf, tr("update"), text::toWide(updater_->latest()).c_str());
                st.update = buf;
            }
            return st;
        },
        [this] {
            if (taskbar_) taskbar_->onTaskbarCreated();
        }});
    tray_->create();

    ipc_ = std::make_unique<IpcServer>(sched_, IpcServer::Callbacks{
                                                   [this] {
                                                       ipc_->send(json{{"type", "hello"},
                                                                       {"version", MD_VERSION},
                                                                       {"pid", GetCurrentProcessId()},
                                                                       {"language", languageTag()}}
                                                                      .dump());
                                                       pushState();
                                                       sendLyricsToUi();
                                                       for (auto& [k, q] : queues_) sendQueueToUi(q);
                                                       if (!pendingShow_.empty()) {
                                                           ipc_->send(json{{"type", "show"}, {"window", pendingShow_}}.dump());
                                                           pendingShow_.clear();
                                                       }
                                                   },
                                                   [this] { log::info("ipc", "UI disconnected"); },
                                                   [this](std::string line) { handleIpc(line); }});
    ipc_->start();

    smtc_ = std::make_unique<platform::SmtcBridge>(sched_, [this](CommandAction a, std::optional<int64_t> pos) {
        auto* r = remote_.activeSession();
        if (!r) return;
        // The bridged phone session is Windows' "current" media session, so play / pause that
        // headsets fire on their own reach it: a Bluetooth headset switching over to the PC for a
        // notification ding, or into its call profile when a game opens voice chat. Those arrive
        // right after a stream starts or the output device changes; real key presses don't.
        if (a == CommandAction::Pause || a == CommandAction::Play || a == CommandAction::Toggle || a == CommandAction::Stop) {
            int64_t now = sched_.nowMs();
            int64_t sinceSound = audio_ && audio_->lastStreamStartMs() >= 0 ? now - audio_->lastStreamStartMs() : -1;
            int64_t sinceOutput = outputChangedAt_ >= 0 ? now - outputChangedAt_ : -1;
            int64_t sinceLink = linkUpAt_ >= 0 ? now - linkUpAt_ : -1;
            bool afterSound = sinceSound >= 0 && sinceSound < kSystemCommandWindowMs;
            bool afterOutput = sinceOutput >= 0 && sinceOutput < kSystemCommandWindowMs;
            // The bridged session is published anew when the phone (re)connects; Windows /
            // headsets answer that with a pause of their own (user log: ~200 ms after each link up).
            bool afterLink = sinceLink >= 0 && sinceLink < kSystemCommandWindowMs;
            if (afterSound || afterOutput || afterLink) {
                log::info("host", "ignored {} from Windows media controls {} ms after {} (headset / system, not a key press)",
                          media::toString(a), afterSound ? sinceSound : afterOutput ? sinceOutput : sinceLink,
                          afterSound    ? "a sound started on this PC"
                          : afterOutput ? "the output device changed"
                                        : "the phone connected");
                return;
            }
        }
        sendUserCommand(Side::Remote, r->key, a, pos, "smtc");
    });
    smtc_->init(tray_->hwnd());

    arbiter_ = std::make_unique<policy::PlaybackArbiter>(
        sched_, [this](const policy::Action& a) { executeArbiterAction(a); },
        [this] {
            markDirty();
            if (!policyTimer_.active()) policyTimer_.start(200, [this] { sendPolicyStatus(); });
        });

    gsmtc_ = std::make_unique<platform::GsmtcSource>(
        sched_, *art_,
        [](const std::string& aumid) {
            auto l = text::toLowerAscii(aumid);
            return l == "mediaduet.exe" || l == "mediaduet.host" || l.ends_with("\\mediaduet.exe");
        },
        [this](const media::MediaSnapshot& s) {
            gsmtcSnap_ = s;
            rebuildLocal();
        });
    gsmtc_->start();
    netease_ = std::make_unique<platform::NeteaseCompat>(sched_, *art_, [this] { rebuildLocal(); });

    audio_ = std::make_unique<platform::AudioActivityMonitor>(
        sched_, [this](bool active, bool comm, std::vector<std::string> apps) {
            log::info("audio", "Windows audio {}{} {}", active ? "active" : "idle", comm ? " (call)" : "",
                      apps.empty() ? "" : apps.front());
            netease_->setAudible(std::find(apps.begin(), apps.end(), "cloudmusic.exe") != apps.end());
            arbiter_->onWindowsAudio(active, comm, std::move(apps));
        });
    audio_->start();
    audio_->setOnAppVolumeChanged([this](const std::string&) {
        markDirty();
        if (!appVolTimer_.active()) appVolTimer_.start(150, [this] { sendLocalSnapshot(); });
    });

    a2dp_ = std::make_unique<platform::A2dpSink>(sched_, [this] {
        updateSuspension();
        sendSinkState();
        updateTray();
        markDirty();
    });
    a2dp_->start();
    loopback_ = std::make_unique<platform::LoopbackSender>(sched_, [this] { markDirty(); });
    volume_ = std::make_unique<platform::VolumeControl>(sched_, [this](const platform::VolumeControl::State& s) {
        bool deviceChanged = s.deviceId != localVol_.deviceId;
        bool latencyChanged = s.bluetooth != localVol_.bluetooth;
        if (deviceChanged || latencyChanged) outputChangedAt_ = sched_.nowMs();
        localVol_ = s;
        if (deviceChanged) updateSuspension();
        if (latencyChanged) {
            // Output latency of local sessions changed: lyrics delay + what the phone shows.
            updateOverlayPlayback();
            sendLocalSnapshot();
        }
        markDirty();
        // Coalesce slider drags: at most ~10 updates per second to the phone.
        if (!volumeTimer_.active()) volumeTimer_.start(100, [this] { sendVolumeState(); });
    });
    if (volume_->start()) localVol_ = volume_->state();

    lyrics_ = std::make_unique<platform::LyricsService>(sched_, platform::cacheDir() / L"lyrics",
                                                        [this](const platform::LyricsService::Result& r) { onLyrics(r); });
    updater_ = std::make_unique<platform::Updater>(sched_, dir / L"updates", MD_VERSION,
                                                   platform::Updater::Callbacks{[this] { markDirty(); }, [this] { exitApp(); }});
    handoff_ = std::make_unique<HandoffHost>(
        sched_, HandoffHost::Callbacks{
                    [this]() -> const media::MediaSnapshot& { return local_; },
                    [this]() -> const media::MediaSnapshot& { return remote_; },
                    [this](handoff::Device d, const std::string& key, CommandAction a, std::optional<int64_t> pos) {
                        sendUserCommand(d == handoff::Device::Pc ? Side::Local : Side::Remote, key, a, pos, "handoff");
                    },
                    [this](const std::string& key, int64_t pos) { return netease_->seek(key, pos); },
                    [this](const std::string& key) { return netease_->positionEstimated(key); },
                    [this] { return !standalone_ && cm_->isUp() && peerHasCap(proto::caps::Handoff); },
                    [this](const char* type, json body, std::optional<uint64_t> re) { return cm_->send(type, std::move(body), re); },
                    [this] { return cm_->status().rttMs; },
                    [this] { markDirty(); },
                    [this](const handoff::Outcome& o) {
                        if (ipc_ && ipc_->connected())
                            ipc_->send(json{{"type", "handoff"}, {"outcome", handoff::toJson(o)}}.dump(-1, ' ', false, json::error_handler_t::replace));
                    }});
    overlay_ = std::make_unique<platform::LyricsOverlay>(
        sched_, platform::LyricsOverlay::Callbacks{
                    [this](int x, int y) {
                        settings_.apply({{"lyrics", {{"overlay", {{"x", x}, {"y", y}}}}}});
                        settings_.save(settingsPath_);
                        markDirty();
                    },
                    [this](bool locked) {
                        settings_.apply({{"lyrics", {{"overlay", {{"locked", locked}}}}}});
                        settings_.save(settingsPath_);
                        applySettings();
                    },
                    [this] {
                        overlay_->setEnabled(false);
                        settings_.apply({{"lyrics", {{"overlay", {{"visible", false}}}}}});
                        settings_.save(settingsPath_);
                        markDirty();
                    },
                    [this] { openUi("settings"); },
                    [this](CommandAction a) {
                        Side s;
                        if (auto* f = focused(&s)) sendUserCommand(s, f->key, a, std::nullopt, "overlay");
                    },
                    [this](int delta) {
                        int size = settings_.get<int>("lyrics.overlay.fontSize", 32) + delta;
                        settings_.apply({{"lyrics", {{"overlay", {{"fontSize", std::clamp(size, 12, 96)}}}}}});
                        settings_.save(settingsPath_);
                        applySettings();
                    },
                    [this](int deltaMs, bool reset) {
                        if (auto* f = focused()) adjustLyricsOffset(f->appId, deltaMs, reset);
                    }});

    taskbar_ = std::make_unique<platform::TaskbarWidget>(
        sched_, platform::TaskbarWidget::Callbacks{
                    [this](CommandAction a) {
                        Side s;
                        if (auto* f = focused(&s)) sendUserCommand(s, f->key, a, std::nullopt, "taskbar");
                    },
                    [this] { openUi("main"); },
                    [this](bool shown, const std::string& reason) {
                        taskbarShown_ = shown;
                        taskbarReason_ = reason;
                        markDirty();
                    }});

    bt_ = std::make_unique<platform::BluetoothWatcher>(
        sched_, platform::BluetoothWatcher::Callbacks{
                    [this](bool present, bool on) {
                        btAdapter_ = present;
                        btRadio_ = on;
                        cm_->onBluetoothRadio(present && on);
                        markDirty();
                    },
                    [this](uint64_t addr, bool connected) {
                        for (auto& d : devices_.devices())
                            if (d.btAddress == addr) cm_->onPeerBluetoothPresence(d.id, connected);
                    },
                    [this](std::vector<platform::BtCandidate> list) {
                        btCandidates_.clear();
                        for (auto& c : list) btCandidates_[c.deviceId] = c;
                        markDirty();
                    }});
    bt_->start();

    dnssd_ = std::make_unique<platform::DnssdBrowser>(sched_, [this](const platform::DnssdService& s, bool available) {
        if (devices_.find(s.deviceId)) cm_->onLanEndpoint(s.deviceId, s.endpoint, available);
        markDirty();
    });

    try {
        netToken_ = winrt::Windows::Networking::Connectivity::NetworkInformation::NetworkStatusChanged([this](auto&&) {
            sched_.post([this] {
                if (exiting_) return;
                log::info("host", "network changed");
                cm_->onNetworkChanged();
                if (dnssd_->running()) dnssd_->restart();
            });
        });
    } catch (...) {
    }

    applySettings();
    reloadPeers();
    updateTray();
}

void HostApp::shutdown() {
    exiting_ = true;
    try {
        winrt::Windows::Networking::Connectivity::NetworkInformation::NetworkStatusChanged(netToken_);
    } catch (...) {
    }
    ipc_->stop();
    loopback_->stop("exit");
    a2dp_->disable();
    stateTimer_.cancel();
    policyTimer_.cancel();
    diagTimer_.cancel();
    artTimer_.cancel();
    lyricsGraceTimer_.cancel();
    overlay_.reset();
    taskbar_.reset();
    smtc_.reset();
    dnssd_.reset();
    bt_.reset();
    handoff_.reset();
    updater_.reset();
    cm_.reset();
    gsmtc_.reset();
    audio_.reset();
    arbiter_.reset();
    lyrics_.reset();
    tray_.reset();
    log::info("host", "MediaDuet stopped");
}

void HostApp::exitApp() {
    if (exiting_) return;
    if (cm_->isUp()) cm_->send(proto::types::Bye, {{"reason", "app_exit"}});
    if (ipc_->connected()) ipc_->send(json{{"type", "show"}, {"window", "exit"}}.dump());
    // Give the bye a moment to leave the socket, then quit the loop.
    sched_.schedule(150, [] { PostQuitMessage(0); });
}

void HostApp::applySettings() {
    setLanguage(settings_.get<std::string>("language", "auto"));
    bool wasStandalone = standalone_;
    standalone_ = settings_.get<std::string>("mode", "linked") == "standalone";
    if (standalone_ != wasStandalone) {
        log::info("host", "work mode: {}", standalone_ ? "this PC only (no phone links)" : "linked with a phone");
        if (standalone_) {
            // Drop everything that needs the phone; local sessions, lyrics, overlay and
            // taskbar keep working.
            if (auto id = cm_->activePeer()) cm_->disconnect(*id);
            cancelPairing();
            discovering_ = false;
            if (a2dp_->active()) a2dp_->disable();
            setLanSend(false);
        }
        reloadPeers(); // linked again: transports restart
        updateSmtc();
    }
    cm_->setMode(link::connModeFromString(settings_.get<std::string>("connection.mode", "auto")));
    cm_->setSwitchBackDelay(settings_.get<int64_t>("connection.switchBackDelaySec", 10) * 1000);

    policy::Config pc;
    pc.mode = policy::modeFromString(settings_.get<std::string>("policy.mode", "windows_first"));
    pc.autoResume = settings_.get<bool>("policy.autoResume", true);
    pc.resumeDelayMs = settings_.get<int64_t>("policy.resumeDelaySec", 5) * 1000;
    pc.pauseForCalls = settings_.get<bool>("policy.pauseForCalls", true);
    pc.headsetAssist = settings_.get<bool>("policy.headsetAssist", true);
    for (auto& [k, v] : settings_.data()["policy"]["priorities"].items()) pc.priorities[k] = v.get<int>();
    arbiter_->setConfig(pc);

    platform::AudioActivityConfig ac;
    ac.sensitivity = settings_.get<double>("policy.sensitivity", 0.02);
    ac.activationMs = settings_.get<int64_t>("policy.activationMs", 1500);
    ac.blacklist = settings_.get<std::vector<std::string>>("policy.blacklist", {});
    ac.blacklist.push_back("mediaduetui.exe");
    ac.whitelist = settings_.get<std::vector<std::string>>("policy.whitelist", {});
    audio_->setConfig(ac);

    smtc_->setEnabled(settings_.get<bool>("smtcBridge", true));
    gsmtc_->setArtworkOnline(settings_.get<bool>("artwork.online", true));
    netease_->setEnabled(settings_.get<bool>("compat.neteaseDesktop", true));
    updateSuspension();
    lyrics_->configure(settings_.get<bool>("lyrics.online", true),
                       text::toWide(settings_.get<std::string>("lyrics.localFolder", "")),
                       settings_.get<std::vector<std::string>>("lyrics.providers", {"netease", "qqmusic", "lrclib"}));
    platform::LyricsOverlay::Strings os;
    os.lock = tr("overlay.lock");
    os.unlock = tr("overlay.unlock");
    os.hide = tr("overlay.hide");
    os.settings = tr("overlay.settings");
    os.earlier = tr("overlay.earlier");
    os.later = tr("overlay.later");
    os.resetOffset = tr("overlay.resetOffset");
    os.previous = tr("overlay.previous");
    os.playPause = tr("overlay.playPause");
    os.next = tr("overlay.next");
    os.smaller = tr("overlay.smaller");
    os.larger = tr("overlay.larger");
    os.close = tr("overlay.close");
    os.unlockTip = tr("overlay.unlockTip");
    // lyrics.translation is global: it switches translations on every desktop surface.
    bool showTr = settings_.get<bool>("lyrics.translation", true);
    json overlayCfg = settings_.data()["lyrics"]["overlay"], taskbarCfg = settings_.data()["taskbar"];
    overlayCfg["translation"] = showTr;
    taskbarCfg["translation"] = showTr;
    overlay_->configure(overlayCfg, settings_.get<bool>("ui.animations", true),
                        settings_.get<bool>("ui.lowPower", false), os);
    overlay_->setEnabled(settings_.get<bool>("lyrics.overlay.visible", false));
    taskbar_->configure(taskbarCfg, settings_.get<bool>("ui.animations", true),
                        settings_.get<bool>("ui.lowPower", false));

    bool wantAuto = settings_.get<bool>("autostart", true);
    if (autostartEnabled() != wantAuto) setAutostart(wantAuto);

    // Latency / offsets may have changed.
    updateOverlayPlayback();
    sendLocalSnapshot();

    updater_->setAutoCheck(settings_.get<bool>("update.auto", true));
    diagnostics_ = settings_.get<bool>("diagnostics", false);
    if (diagnostics_ && !diagTimer_.active()) tickDiagnostics();
    if (!diagnostics_) diagTimer_.cancel();
    updateDiscoveryWatchers();
    updateSmtc();
    markDirty();
}

void HostApp::reloadPeers() {
    // This PC only: no peers, so the link manager neither connects nor retries.
    cm_->setPeers(standalone_ ? std::vector<link::PeerRecord>{} : devices_.peers());
    std::vector<uint64_t> addrs;
    for (auto& d : devices_.devices())
        if (d.btAddress && !standalone_) addrs.push_back(d.btAddress);
    bt_->watchDevices(addrs);
    updateDiscoveryWatchers();
    markDirty();
}

void HostApp::updateDiscoveryWatchers() {
    bool lanAllowed = settings_.get<std::string>("connection.mode", "auto") != "bt_only";
    bool want = !standalone_ && (discovering_ || (lanAllowed && !devices_.devices().empty()));
    if (want && !dnssd_->running()) dnssd_->start();
    if (!want && dnssd_->running()) dnssd_->stop();
}

// ======================================================================= link events

void HostApp::onLinkUp(const std::string& peerId, bool resumed) {
    log::info("host", "link up with {} ({})", peerId.substr(0, 8), resumed ? "resumed" : "new");
    linkUpAt_ = sched_.nowMs();
    if (auto* d = devices_.find(peerId)) {
        d->lastSeen = sched_.wallMs();
        rememberPeerApp(*d, cm_->status().peerApp);
        devices_.save();
    }
    if (!resumed) {
        remote_ = {};
        remoteValid_ = false;
        queues_.clear();
        queueRequested_.clear();
        lyricsPushed_.clear();
        lastPolicyJson_.clear();
    }
    lastSentValid_ = false;
    remoteVol_ = {};
    sendLocalSnapshot();
    sendVolumeState();
    sendPolicyStatus();
    lastSinkJson_.clear();
    sendSinkState();
    if (handoff_) handoff_->onLinkUp();
    updateTray();
    markDirty();
}

void HostApp::onLinkDown(const std::string& peerId) {
    log::info("host", "link down with {}", peerId.substr(0, 8));
    remote_ = {};
    remoteValid_ = false;
    remoteVol_ = {};
    remoteVolError_.clear();
    answerVolumeSet(false, "not connected");
    arbiter_->onAndroidLinkDown();
    incomingArt_.clear();
    if (loopback_->running()) loopback_->stop("link down");
    audioOffer_.reset();
    if (handoff_) handoff_->onLinkDown();
    updateSmtc();
    updateFocus();
    updateTray();
    markDirty();
}

void HostApp::onPeerRevoked(const std::string& peerId) {
    log::warn("host", "phone {} revoked this PC; removing pairing", peerId.substr(0, 8));
    devices_.remove(peerId);
    devices_.save();
    reloadPeers();
}

void HostApp::rememberPeerApp(platform::DeviceRecord& d, const std::string& app) {
    if (app.empty() || app.size() > 32 || app == d.app) return;
    d.app = app;
    auto m = version::compare(MD_VERSION, app);
    if (m == version::Match::Mismatch)
        log::warn("host", "{} runs MediaDuet {}, this PC {}: update both to the same version", d.name, app, MD_VERSION);
    else
        log::info("host", "{} runs MediaDuet {} ({})", d.name, app, version::toString(m));
}

void HostApp::onStatusChanged() {
    updateTray();
    markDirty();
}

void HostApp::onPeerEndpoints(const std::string&, const std::vector<link::LanEndpoint>&) { markDirty(); }

void HostApp::onEnvelope(const std::string& peerId, const proto::Envelope& e, uint64_t connId) {
    const auto& t = e.type;
    if (t == proto::types::MediaSnapshot) return onRemoteSnapshot(e);
    if (handoff_ && handoff_->onEnvelope(e)) return;
    if (t == proto::types::MediaCommand) return executeRemoteCommand(e, connId);
    if (t == proto::types::ArtworkGet) return serveArtwork(e, connId);
    if (t == proto::types::VolumeState) {
        remoteVol_.available = true;
        remoteVol_.level = static_cast<float>(std::clamp(e.body.value("level", 0.0), 0.0, 1.0));
        remoteVol_.muted = e.body.value("muted", false);
        remoteVol_.steps = std::clamp(e.body.value("steps", 0), 0, 1000);
        if (pendingVolume_) {
            remoteVolError_.clear(); // the set went through
            answerVolumeSet(true, "");
        }
        markDirty();
        return;
    }
    if (t == proto::types::VolumeSet) {
        if (e.body.contains("key") && e.body["key"].is_string()) {
            if (auto* s = local_.find(e.body["key"].get<std::string>())) {
                std::optional<float> lvl;
                std::optional<bool> mute;
                if (e.body.contains("level") && e.body["level"].is_number()) lvl = static_cast<float>(e.body["level"].get<double>());
                if (e.body.contains("muted") && e.body["muted"].is_boolean()) mute = e.body["muted"].get<bool>();
                audio_->setAppVolume(processFor(*s), lvl, mute);
            }
            return;
        }
        if (e.body.contains("level") && e.body["level"].is_number())
            volume_->setLevel(static_cast<float>(e.body["level"].get<double>()));
        if (e.body.contains("muted") && e.body["muted"].is_boolean()) volume_->setMuted(e.body["muted"].get<bool>());
        if (e.body.contains("delta") && e.body["delta"].is_number_integer())
            volume_->step(std::clamp(e.body["delta"].get<int>(), -50, 50));
        localVol_ = volume_->state();
        sendVolumeState();
        return;
    }
    if (t == proto::types::DeviceInfo) {
        if (auto* d = devices_.find(peerId); d && e.body.is_object() && e.body.contains("app") && e.body["app"].is_string()) {
            std::string before = d->app;
            rememberPeerApp(*d, e.body["app"].get<std::string>());
            if (d->app != before) {
                devices_.save();
                markDirty();
            }
        }
        return;
    }
    if (t == proto::types::LyricsPush) {
        auto text = e.body.value("text", std::string());
        if (text.size() <= proto::kMaxLyricsBytes)
            lyrics_->offerRemote(e.body.value("trackId", std::string()), text, "remote",
                                 e.body.value("translation", std::string()));
        return;
    }
    if (t == proto::types::MediaQueue) {
        std::string err;
        auto q = media::queueFromWire(e.body, &err);
        if (!q) {
            log::warn("host", "invalid media.queue from phone: {}", err);
            return;
        }
        log::debug("host", "queue of {}: {} of {} entries", q->key, q->items.size(), q->total);
        queues_[q->key] = *q;
        sendQueueToUi(*q);
        return;
    }
    if (t == proto::types::AudioSinkSet) {
        // The phone's "play on this PC" switch (Bluetooth A2DP into Windows).
        bool enable = e.body.value("enable", false);
        log::info("host", "phone {} Bluetooth audio on this PC", enable ? "requested" : "stopped");
        if (standalone_) return;
        setA2dp(enable);
        lastSinkJson_.clear(); // always answer, also when nothing changed
        sendSinkState();
        return;
    }
    if (t == proto::types::PolicyConfig) {
        auto mode = e.body.value("mode", std::string());
        if (!mode.empty()) {
            log::info("host", "phone requested arbitration mode {}", mode);
            settings_.apply({{"policy", {{"mode", mode}}}});
            settings_.save(settingsPath_);
            applySettings();
        }
        return;
    }
    if (t == proto::types::TransportInfo) {
        if (auto addr = platform::parseBtAddress(e.body.value("btAddress", std::string()))) {
            if (auto* d = devices_.find(peerId); d && d->btAddress != *addr) {
                d->btAddress = *addr;
                devices_.save();
                reloadPeers();
            }
        } else if (auto* d = devices_.find(peerId); d && d->btAddress == 0 && btAdapter_) {
            // LAN-paired phone: find its bonded Bluetooth twin so RFCOMM can be used too.
            std::string btName = e.body.value("btName", std::string());
            if (btName.empty()) btName = d->name;
            bt_->resolveByName(btName, [this, peerId, btName](uint64_t addr) {
                auto* d = devices_.find(peerId);
                if (!d || !addr || d->btAddress) {
                    if (!addr) log::info("bt", "no bonded Bluetooth device named '{}' offers MediaDuet", btName);
                    return;
                }
                log::info("bt", "mapped {} to Bluetooth {}", d->name, platform::formatBtAddress(addr));
                d->btAddress = addr;
                devices_.save();
                reloadPeers();
            });
        }
        return;
    }
    if (t == proto::types::AudioAnswer) {
        if (!audioOffer_ || !e.re || *e.re != audioOffer_->offerId) return;
        if (!e.body.value("ok", false)) {
            log::warn("host", "phone declined audio stream: {}", e.body.value("error", std::string()));
            audioOffer_.reset();
            markDirty();
            return;
        }
        int port = e.body.value("port", 0);
        auto sid = hexDecode(audioOffer_->streamId);
        if (port <= 0 || port > 65535 || !sid) return;
        Bytes key = crypto::hmacSha256(audioOffer_->key, concat({view(std::string_view("audio key")), *sid}));
        Bytes iv = crypto::hmacSha256(audioOffer_->key, concat({view(std::string_view("audio iv")), *sid}));
        iv.resize(4);
        loopback_->start(audioOffer_->host, static_cast<uint16_t>(port), *sid, key, iv);
        return;
    }
    if (t == proto::types::AudioStop) {
        if (loopback_->running()) loopback_->stop(e.body.value("reason", std::string("peer stopped")));
        audioOffer_.reset();
        markDirty();
        return;
    }
    if (t == proto::types::Error) {
        std::string code = e.body.value("code", std::string());
        if (code.size() > 64) code.resize(64);
        if (pendingVolume_ && e.re && *e.re == pendingVolume_->msgId) {
            // The phone could not change its volume (e.g. background_restricted on
            // Android 17+): pass the code to the UI unchanged, no retry.
            log::info("host", "phone refused volume.set: {}", code);
            remoteVolError_ = code.empty() ? "internal" : code;
            answerVolumeSet(false, remoteVolError_);
            markDirty();
            return;
        }
        log::warn("host", "peer error {}: {}", code, e.body.value("message", std::string()));
        return;
    }
}

// ======================================================================= media

void HostApp::rebuildLocal() {
    media::MediaSnapshot combined = gsmtcSnap_;
    bool neteaseInSmtc = false;
    for (auto& s : combined.sessions)
        if (text::toLowerAscii(s.appId).find("cloudmusic") != std::string::npos) {
            neteaseInSmtc = true;
            netease_->supplement(s); // its own session has no timeline: duration + estimated position
        }
    netease_->setSmtcPresent(neteaseInSmtc);
    if (auto n = netease_->session()) {
        bool anyPlaying = false;
        for (auto& s : combined.sessions) anyPlaying |= s.isPlaying();
        if (combined.sessions.size() < media::kMaxSessions) combined.sessions.push_back(*n);
        if (!combined.active || (n->isPlaying() && !anyPlaying)) combined.active = n->key;
    }
    if (combined.equivalent(local_)) return;
    combined.gen = ++localGen_;
    onLocalSnapshot(combined);
}

void HostApp::executeLocal(const media::MediaCommand& cmd, std::function<void(bool, const std::string&)> done) {
    std::string key = cmd.key.value_or(local_.active.value_or(""));
    if (key == platform::NeteaseCompat::kKey) {
        bool ok = netease_->execute(cmd.action);
        if (done) done(ok, ok ? "" : "action_unsupported");
        return;
    }
    gsmtc_->execute(cmd, std::move(done));
}

void HostApp::onLocalSnapshot(const media::MediaSnapshot& snap) {
    local_ = snap;
    arbiter_->onWindowsSessions(snap);
    bool anyPlaying = false;
    for (auto& s : local_.sessions) anyPlaying |= s.isPlaying();
    if (auto* r = remote_.activeSession()) anyPlaying |= r->isPlaying();
    cm_->setMediaActive(anyPlaying);
    sendLocalSnapshot();
    updateFocus();
    if (handoff_) handoff_->onLocalSnapshot();
    markDirty();
}

bool HostApp::peerHasCap(const char* cap) const {
    const auto* caps = cm_->peerCaps();
    return caps && std::find(caps->begin(), caps->end(), cap) != caps->end();
}

std::string HostApp::processFor(const MediaSessionInfo& s) const {
    // GSMTC app ids -> process names for the per-app mixer.
    std::string id = text::toLowerAscii(s.appId);
    if (id == "msedge") return "msedge.exe";
    if (id == "chrome") return "chrome.exe";
    if (id == "308046b0af4a39cb" || id == "firefox") return "firefox.exe";
    if (id.ends_with(".exe")) {
        auto slash = id.find_last_of("\\/");
        return slash == std::string::npos ? id : id.substr(slash + 1);
    }
    return {};
}

media::MediaSnapshot HostApp::decoratedLocal() const {
    media::MediaSnapshot d = local_;
    int latency = localOutputLatencyMs();
    for (auto& s : d.sessions) {
        s.outputLatencyMs = latency; // spec §8.3: this PC's output (e.g. Bluetooth headphones)
        std::string proc = processFor(s);
        if (proc.empty()) continue;
        auto v = audio_->appVolume(proc);
        if (v.available) {
            s.appVolume = v.level;
            s.appMuted = v.muted;
        }
    }
    return d;
}

void HostApp::sendLocalSnapshot() {
    if (!cm_->isUp()) return;
    auto d = decoratedLocal();
    if (lastSentValid_ && d.equivalent(lastSentLocal_)) return;
    d.gen = ++localWireGen_;
    if (cm_->send(proto::types::MediaSnapshot, media::toWire(d, sched_.nowMs()))) {
        lastSentLocal_ = d;
        lastSentValid_ = true;
    }
}

void HostApp::updateSuspension() {
    bool wasGated = gated_;
    gated_ = false;
    const auto& list = settings_.data()["policy"]["onlyWithOutputs"];
    if (list.is_array() && !list.empty()) {
        gated_ = true;
        std::string id = text::toUtf8(localVol_.deviceId);
        for (auto& o : list) {
            if (!o.is_object()) continue;
            if ((!id.empty() && o.value("id", std::string()) == id) ||
                (!localVol_.deviceName.empty() && o.value("name", std::string()) == localVol_.deviceName))
                gated_ = false;
        }
    }
    if (gated_ != wasGated)
        log::info("policy", "arbitration {} (output: {})", gated_ ? "gated" : "active", localVol_.deviceName);
    arbiter_->setSuspended(a2dp_->active() || gated_ || standalone_); // standalone: nothing to arbitrate with
    markDirty();
}

void HostApp::onRemoteSnapshot(const proto::Envelope& e) {
    auto st = cm_->status();
    int64_t latency = st.rttMs > 0 ? st.rttMs / 2 : 0;
    std::string err;
    auto snap = media::snapshotFromWire(e.body, sched_.nowMs(), latency, &err);
    if (!snap) {
        log::warn("host", "invalid snapshot from phone: {}", err);
        return;
    }
    if (remoteValid_ && snap->gen <= remote_.gen) return;
    remote_ = std::move(*snap);
    remoteValid_ = true;
    arbiter_->onAndroidSnapshot(remote_);
    bool anyPlaying = false;
    for (auto& s : local_.sessions) anyPlaying |= s.isPlaying();
    if (auto* r = remote_.activeSession()) anyPlaying |= r->isPlaying();
    cm_->setMediaActive(anyPlaying);
    requestMissingArtwork();
    requestQueues();
    updateSmtc();
    updateFocus();
    markDirty();
}

void HostApp::requestMissingArtwork() {
    int64_t now = sched_.nowMs();
    auto want = [&](const std::string& hash) {
        if (art_->has(hash)) return;
        auto it = requestedArt_.find(hash);
        if (it != requestedArt_.end() && now - it->second < 30'000) return;
        requestedArt_[hash] = now;
        cm_->send(proto::types::ArtworkGet, {{"hash", hash}});
    };
    for (auto& s : remote_.sessions) {
        if (s.artwork) want(s.artwork->hash);
        for (auto& a : s.customActions) // the player's own buttons: small PNG icons
            if (a.icon) want(a.icon->hash);
    }
    if (requestedArt_.size() > 64) requestedArt_.clear();
}

void HostApp::requestQueues() {
    int64_t now = sched_.nowMs();
    // Forget queues of sessions that are gone or no longer expose one.
    for (auto it = queues_.begin(); it != queues_.end();) {
        auto* s = remote_.find(it->first);
        if (s && s->queue) {
            ++it;
            continue;
        }
        if (ipc_) ipc_->send(json{{"type", "queue"}, {"side", "remote"}, {"key", it->first}, {"queue", nullptr}}.dump());
        it = queues_.erase(it);
    }
    for (auto& s : remote_.sessions) {
        if (!s.queue) continue;
        auto have = queues_.find(s.key);
        if (have != queues_.end() && have->second.rev == s.queue->rev) {
            if (have->second.activeId != s.queue->activeId) {
                have->second.activeId = s.queue->activeId; // same list, another current entry
                sendQueueToUi(have->second);
            }
            continue;
        }
        auto& req = queueRequested_[s.key];
        if (req.first == s.queue->rev && now - req.second < 10'000) continue;
        req = {s.queue->rev, now};
        cm_->send(proto::types::MediaQueueGet, {{"key", s.key}});
    }
}

void HostApp::sendQueueToUi(const media::MediaQueue& q) {
    if (ipc_) ipc_->send(json{{"type", "queue"}, {"side", "remote"}, {"key", q.key}, {"queue", media::toJson(q)}}.dump());
}

void HostApp::onBlob(const std::string&, const proto::BlobChunk& c) {
    std::string hash = hexEncode(c.hash);
    if (!requestedArt_.count(hash) || art_->has(hash)) return;
    auto& r = incomingArt_[hash];
    if (r.total == 0) {
        r.total = c.total;
        r.data.assign(c.total, 0);
        r.startedAt = sched_.nowMs();
    }
    if (c.total != r.total || r.offsets.count(c.offset)) return;
    std::copy(c.data.begin(), c.data.end(), r.data.begin() + c.offset);
    r.offsets.insert(c.offset);
    r.received += static_cast<uint32_t>(c.data.size());
    if (r.received < r.total) return;
    bool ok = art_->store(hash, r.data);
    incomingArt_.erase(hash);
    if (ok) {
        updateSmtc();
        markDirty();
    }
}

void HostApp::serveArtwork(const proto::Envelope& e, uint64_t connId) {
    std::string hash = e.body.value("hash", std::string());
    auto data = art_->read(hash);
    if (!data) {
        cm_->sendVia(connId, proto::types::Error, proto::errorBody("not_found", "artwork not cached"), e.id);
        return;
    }
    auto h = hexDecode(hash);
    for (size_t off = 0; off < data->size(); off += kChunk) {
        proto::BlobChunk c;
        c.hash = *h;
        c.total = static_cast<uint32_t>(data->size());
        c.offset = static_cast<uint32_t>(off);
        c.data.assign(data->begin() + off, data->begin() + std::min(data->size(), off + kChunk));
        cm_->sendBlob(proto::encodeBlobChunk(c), connId);
    }
}

void HostApp::executeRemoteCommand(const proto::Envelope& e, uint64_t connId) {
    std::string err;
    auto cmd = media::commandFromWire(e.body, &err);
    auto reply = [this, id = e.id, connId](bool ok, const std::string& code) {
        json body = {{"ok", ok}};
        if (!ok) body["error"] = proto::errorBody(code.empty() ? "internal" : code);
        proto::Envelope r;
        r.type = proto::types::MediaCommandResult;
        r.re = id;
        r.body = body;
        cm_->cacheResult(id, r);
        cm_->sendVia(connId, proto::types::MediaCommandResult, body, id);
    };
    if (!cmd) return reply(false, "bad_message");
    std::string key = cmd->key.value_or(local_.active.value_or(""));
    if (cmd->origin == media::Origin::User)
        arbiter_->onUserCommand(policy::Side::Windows, key, cmd->action, true);
    log::info("host", "phone -> PC: {} {}", media::toString(cmd->action), key);
    executeLocal(*cmd, [reply](bool ok, const std::string& code) { reply(ok, code); });
}

void HostApp::sendUserCommand(Side side, std::optional<std::string> key, CommandAction action,
                              std::optional<int64_t> pos, const std::string& reason, std::string customAction,
                              std::optional<int64_t> queueId) {
    media::MediaCommand cmd;
    cmd.key = key;
    cmd.action = action;
    cmd.positionMs = pos;
    cmd.customAction = std::move(customAction);
    cmd.queueId = queueId;
    cmd.origin = media::Origin::User;
    cmd.reason = reason;
    if (action == CommandAction::Seek) lyricsSeek_ = true; // lyrics clock snaps instead of slewing
    if (side == Side::Local) {
        if (key) arbiter_->onUserCommand(policy::Side::Windows, *key, action, false);
        log::info("host", "UI -> PC: {} {}", media::toString(action), key.value_or("(active)"));
        executeLocal(cmd, [action](bool ok, const std::string& code) {
            if (!ok) log::warn("host", "local {} failed: {}", media::toString(action), code);
        });
        return;
    }
    if (key) arbiter_->onUserCommand(policy::Side::Android, *key, action, false);
    log::info("host", "PC -> phone: {} ({})", media::toString(action), reason);
    cm_->sendCommand(cmd, [this, action](const link::CommandOutcome& o) {
        lastCommandLatency_ = o.latencyMs;
        if (!o.ok) log::warn("host", "phone {} failed: {} {}", media::toString(action), o.code, o.message);
        markDirty();
    });
}

void HostApp::executeArbiterAction(const policy::Action& a) {
    using K = policy::Action::Kind;
    media::MediaCommand cmd;
    cmd.key = a.key;
    cmd.origin = media::Origin::Policy;
    cmd.reason = a.reason;
    switch (a.kind) {
    case K::PauseAndroid:
    case K::ResumeAndroid:
        cmd.action = a.kind == K::PauseAndroid ? CommandAction::Pause : CommandAction::Play;
        cm_->sendCommand(cmd, [](const link::CommandOutcome& o) {
            if (!o.ok) log::warn("policy", "phone command failed: {}", o.code);
        });
        break;
    case K::PauseWindows:
    case K::ResumeWindows:
        cmd.action = a.kind == K::PauseWindows ? CommandAction::Pause : CommandAction::Play;
        executeLocal(cmd, [](bool ok, const std::string& code) {
            if (!ok) log::warn("policy", "Windows command failed: {}", code);
        });
        break;
    }
}

std::string HostApp::artworkPath(const MediaSessionInfo& s) const {
    if (!s.artwork || !art_->has(s.artwork->hash)) return {};
    return text::toUtf8(art_->pathFor(s.artwork->hash).wstring());
}

// ======================================================================= focus / lyrics / overlay / SMTC

const MediaSessionInfo* HostApp::focused(Side* side) const {
    const auto& snap = focusSide_ == Side::Local ? local_ : remote_;
    if (auto* s = snap.find(focusKey_)) {
        if (side) *side = focusSide_;
        return s;
    }
    return nullptr;
}

void HostApp::updateFocus() {
    if (focusAuto_ || !focused()) {
        focusAuto_ = true;
        const MediaSessionInfo* r = remote_.activeSession();
        const MediaSessionInfo* l = local_.activeSession();
        Side side = focusSide_;
        std::string key = focusKey_;
        if (r && r->isPlaying()) side = Side::Remote, key = r->key;
        else if (l && l->isPlaying()) side = Side::Local, key = l->key;
        else if (!focused()) {
            if (r) side = Side::Remote, key = r->key;
            else if (l) side = Side::Local, key = l->key;
            else key.clear();
        }
        focusSide_ = side;
        focusKey_ = key;
    }
    Side fs = Side::Remote;
    const MediaSessionInfo* f = focused(&fs);
    // The lyrics shown belong to this player session (side + key) and song.
    bool sameSession = f && !lyricsTrack_.empty() && fs == lyricsSide_ && f->key == lyricsKey_;
    // The focused session's metadata went blank (some players clear it between two
    // reports of one song): keep the current lyrics for a moment instead of clearing
    // every surface and reloading them when the same song comes back.
    bool blank = f && f->title.empty() && f->artist.empty();
    if (blank && sameSession && f->trackId != lyricsTrack_ && !lyricsGraceExpired_) {
        if (!lyricsGraceTimer_.active())
            lyricsGraceTimer_.start(kLyricsGraceMs, [this] {
                lyricsGraceExpired_ = true;
                updateFocus();
            });
        return;
    }
    if (!blank) lyricsGraceExpired_ = false;
    lyricsGraceTimer_.cancel();
    if (f) {
        if (f->trackId != lyricsTrack_) {
            lyrics::Candidate song{f->title, f->artist, f->album, f->durationMs};
            if (sameSession && lyrics::sameSong(song, lyricsSong_) && lyrics_->rekey(f->trackId)) {
                // Same song under a new trackId (the player re-reported its duration or
                // album): keep the lyrics on the overlay, taskbar, UI and phone, no reload.
                log::info("lyrics", "track id {} -> {} for the same song; keeping its lyrics", lyricsTrack_, f->trackId);
                lyricsTrack_ = f->trackId;
                lyricsSong_ = std::move(song);
                if (lyrics_->current().status == "found") pushLyricsToPhone(lyrics_->current());
                sendLyricsToUi();
            } else {
                lyricsTrack_ = f->trackId;
                lyricsSide_ = fs;
                lyricsKey_ = f->key;
                lyricsSong_ = std::move(song);
                ++lyricsSongGen_;
                lyricsData_.reset();
                overlay_->setLyrics(nullptr, f->trackId);
                taskbar_->setLyrics(nullptr, f->trackId);
                lyrics_->request(*f, fs == Side::Local && f->key == platform::NeteaseCompat::kKey ? netease_->songId() : 0);
            }
        }
    } else if (!lyricsTrack_.empty()) {
        lyricsTrack_.clear();
        lyricsKey_.clear();
        lyricsSong_ = {};
        ++lyricsSongGen_;
        lyricsData_.reset();
        overlay_->setLyrics(nullptr, "");
        taskbar_->setLyrics(nullptr, "");
        lyrics_->clear();
    }
    updateOverlayPlayback();
}

void HostApp::pushLyricsToPhone(const platform::LyricsService::Result& r) {
    // Share with the phone so it can show them too (once per trackId; lyrics that came
    // from the phone are not sent back).
    if (!cm_->isUp() || r.source == "remote" || r.trackId.empty() || lyricsPushed_.count(r.trackId) ||
        r.lrc.size() > proto::kMaxLyricsBytes)
        return;
    const auto* caps = cm_->peerCaps();
    if (!caps || std::find(caps->begin(), caps->end(), proto::caps::Lyrics) == caps->end()) return;
    json body = {{"trackId", r.trackId}, {"format", "lrc"}, {"source", r.source}, {"text", r.lrc}};
    if (!r.translation.empty() && r.translation.size() <= proto::kMaxLyricsBytes) body["translation"] = r.translation;
    cm_->send(proto::types::LyricsPush, body);
    lyricsPushed_.insert(r.trackId);
}

void HostApp::onLyrics(const platform::LyricsService::Result& r) {
    if (r.trackId != lyricsTrack_) return;
    if (r.status == "loading") lyricsAskedAt_ = sched_.nowMs();
    if (r.status == "found") {
        lyricsData_ = std::make_shared<lyrics::Lyrics>(r.lyrics);
        overlay_->setLyrics(lyricsData_, r.trackId);
        taskbar_->setLyrics(lyricsData_, r.trackId);
        log::info("lyrics", "lyrics for {} from {} ({} lines{}) after {} ms", r.trackId, r.source, r.lyrics.lines.size(),
                  r.lyrics.synced ? ", synced" : "", lyricsAskedAt_ >= 0 ? sched_.nowMs() - lyricsAskedAt_ : 0);
        pushLyricsToPhone(r);
    } else {
        lyricsData_.reset();
        overlay_->setLyrics(nullptr, r.trackId);
        taskbar_->setLyrics(nullptr, r.trackId);
    }
    sendLyricsToUi();
}

void HostApp::updateOverlayPlayback() {
    if (!overlay_ || !taskbar_) return;
    if (lyricsGraceTimer_.active()) return; // focused metadata briefly blank: keep the surfaces as they are
    Side side = Side::Remote;
    const MediaSessionInfo* f = focused(&side);
    // Feed the shared clock; it slews small report jitter instead of jumping.
    double now = lyrics::steadyNowMs();
    lyrics::LyricsClock::Report r;
    if (f) {
        r.positionMs = f->positionMs;
        r.atMs = now - static_cast<double>(sched_.nowMs() - f->positionAtMs);
        r.rate = f->rate;
        r.playing = f->isPlaying();
        // Per song, not per trackId: a same-song trackId change (updateFocus) must not snap.
        r.trackId = f->key + "|" + std::to_string(lyricsSongGen_);
    }
    lyricsClock_->update(r, now, lyricsSeek_);
    lyricsSeek_ = false;
    int64_t delay = f ? lyricsDelayFor(*f, side) : 0;
    int64_t manual = f ? lyrics::manualOffsetMs(settings_.data()["lyrics"]["offsets"], f->appId) : 0;
    taskbar_->setTrack(f);
    taskbar_->setTiming(lyricsClock_, delay);
    overlay_->setTiming(lyricsClock_, delay, manual, f && !f->appId.empty());
}

int HostApp::localOutputLatencyMs() const {
    return localVol_.bluetooth ? std::clamp(settings_.get<int>("lyrics.latency.bluetoothMs", 180), 0, 1000) : 0;
}

int HostApp::outputLatencyFor(const MediaSessionInfo& s, Side side) const {
    return side == Side::Local ? localOutputLatencyMs() : s.outputLatencyMs.value_or(0);
}

int64_t HostApp::lyricsDelayFor(const MediaSessionInfo& s, Side side) const {
    return lyrics::lyricsDelayMs(settings_.get<bool>("lyrics.latency.auto", true), outputLatencyFor(s, side),
                                 settings_.data()["lyrics"]["offsets"], s.appId);
}

void HostApp::adjustLyricsOffset(const std::string& appId, int deltaMs, bool reset) {
    if (appId.empty()) return;
    int64_t cur = lyrics::manualOffsetMs(settings_.data()["lyrics"]["offsets"], appId);
    int64_t next = reset ? 0 : std::clamp<int64_t>(cur + deltaMs, -lyrics::kMaxManualOffsetMs, lyrics::kMaxManualOffsetMs);
    log::info("lyrics", "lyrics offset for {}: {} ms", appId, next);
    settings_.apply({{"lyrics", {{"offsets", {{appId, next == 0 ? json(nullptr) : json(next)}}}}}});
    if (!settings_.save(settingsPath_)) log::warn("host", "could not save settings");
    updateOverlayPlayback();
    markDirty();
}

void HostApp::answerVolumeSet(bool ok, const std::string& code) {
    volumeAnswerTimer_.cancel();
    if (!pendingVolume_) return;
    json rid = pendingVolume_->rid;
    pendingVolume_.reset();
    if (rid.is_null() || !ipc_ || !ipc_->connected()) return;
    json r = {{"type", "result"}, {"rid", rid}, {"ok", ok}};
    if (!ok) r["error"] = code;
    ipc_->send(r.dump());
}

void HostApp::updateSmtc() {
    const MediaSessionInfo* r = cm_->isUp() ? remote_.activeSession() : nullptr;
    std::string name = cm_->status().peerName;
    smtc_->update(r, r && r->artwork ? art_->pathFor(r->artwork->hash) : std::filesystem::path(), name);
}

void HostApp::sendVolumeState() {
    if (!cm_->isUp() || !localVol_.available) return;
    const auto* caps = cm_->peerCaps();
    if (!caps || std::find(caps->begin(), caps->end(), proto::caps::Volume) == caps->end()) return;
    cm_->send(proto::types::VolumeState, {{"level", localVol_.level}, {"muted", localVol_.muted}, {"steps", localVol_.steps}});
}

void HostApp::sendSinkState() {
    if (!cm_->isUp()) return;
    const auto& a2 = a2dp_->status();
    json body = {{"available", a2.available}, {"state", a2.state}};
    if (!a2.device.empty()) body["device"] = a2.device;
    if (!a2.errorCode.empty()) body["error"] = a2.errorCode;
    std::string s = body.dump();
    if (s == lastSinkJson_) return;
    lastSinkJson_ = s;
    cm_->send(proto::types::AudioSinkState, body);
}

void HostApp::sendPolicyStatus() {
    if (!cm_->isUp()) return;
    const auto& last = arbiter_->lastAction();
    std::string localState = "unknown";
    if (auto* l = local_.activeSession()) localState = policy::toString(arbiter_->stateOf(policy::Side::Windows, l->key));
    json body = {{"mode", policy::toString(arbiter_->config().mode)},
                 {"enabled", arbiter_->config().mode != policy::Mode::Off && !arbiter_->suspended()},
                 {"remote", policy::toString(arbiter_->androidState())},
                 {"local", localState},
                 {"windowsAudioActive", arbiter_->windowsAudioActive()}};
    if (!last.what.empty())
        body["lastAction"] = {{"what", last.what},
                              {"reason", last.reason},
                              {"app", last.app},
                              {"at", sched_.wallMs() - (sched_.nowMs() - last.atMs)},
                              {"failed", last.failed}};
    std::string s = body.dump();
    if (s == lastPolicyJson_) return;
    lastPolicyJson_ = s;
    cm_->send(proto::types::PolicyStatus, body);
}

// ======================================================================= pairing

void HostApp::setPairing(std::string state, std::string error) {
    pairing_.state = std::move(state);
    pairing_.error = std::move(error);
    markDirty();
}

void HostApp::startPairing(const std::string& kind, const std::string& target) {
    cancelPairing();
    pairing_ = {};
    pairing_.kind = kind;
    pairing_.target = target;
    uint64_t attempt = ++pairing_.attempt;
    auto onTransport = [this, attempt](link::TransportPtr t, std::string err) {
        if (attempt != pairing_.attempt || pairing_.state != "connecting") {
            if (t) t->close();
            return;
        }
        if (!t) {
            log::info("host", "pairing connect failed: {}", err);
            return setPairing("failed", "connect_failed");
        }
        link::Connection::Events ev;
        ev.onSas = [this, attempt](link::Connection&, const std::string& sas, const proto::DeviceInfo& peer) {
            if (attempt != pairing_.attempt) return;
            pairing_.sas = sas;
            pairing_.peerName = peer.name;
            setPairing("awaiting_user");
            if (!ipc_->connected()) openUi("pair");
        };
        ev.onPaired = [this, attempt](link::Connection&, const proto::DeviceInfo& peer, const Bytes& key) {
            if (attempt != pairing_.attempt) return;
            platform::DeviceRecord rec;
            if (auto* existing = devices_.find(peer.id)) rec = *existing;
            rec.id = peer.id;
            rec.name = peer.name;
            rec.platform = peer.platform;
            rec.key = key;
            rec.pairedAt = sched_.wallMs();
            if (!peer.app.empty() && peer.app.size() <= 32) rec.app = peer.app;
            if (pairing_.btAddress) rec.btAddress = pairing_.btAddress;
            if (pairing_.lan && std::find(rec.manualEndpoints.begin(), rec.manualEndpoints.end(), *pairing_.lan) ==
                                    rec.manualEndpoints.end())
                rec.manualEndpoints.push_back(*pairing_.lan);
            devices_.upsert(rec);
            if (!devices_.save()) log::error("host", "could not persist pairing");
            log::info("host", "paired with {}", peer.name);
            setPairing("succeeded");
            reloadPeers();
            cm_->connectNow(peer.id);
        };
        ev.onClosed = [this, attempt](link::Connection&, const std::string& reason, const std::string& code) {
            if (attempt != pairing_.attempt) return;
            pairing_.conn.reset();
            if (pairing_.state != "succeeded" && pairing_.state != "idle")
                setPairing("failed", code.empty() ? "connection_closed" : code);
        };
        pairing_.conn = link::Connection::create(sched_, std::move(t), std::move(ev));
        proto::DeviceInfo self{identity_.id, identity_.name, "windows", "PC", osVersion(), MD_VERSION};
        pairing_.conn->beginPairing(self);
    };
    setPairing("connecting");
    if (kind == "bt") {
        auto it = btCandidates_.find(target);
        uint64_t addr = it != btCandidates_.end() ? it->second.address : platform::parseBtAddress(target).value_or(0);
        if (!addr) return setPairing("failed", "unknown_device");
        pairing_.btAddress = addr;
        link::PeerRecord pr;
        pr.btAddress = addr;
        factory_->connectBluetooth(pr, onTransport);
    } else {
        auto colon = target.rfind(':');
        std::string host = colon == std::string::npos ? target : target.substr(0, colon);
        int port = colon == std::string::npos ? platform::kDefaultLanPort : std::atoi(target.c_str() + colon + 1);
        if (!host.empty() && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
        if (host.empty() || port <= 0 || port > 65535) return setPairing("failed", "invalid_address");
        pairing_.lan = link::LanEndpoint{host, static_cast<uint16_t>(port), "manual"};
        factory_->connectLan(*pairing_.lan, onTransport);
    }
}

void HostApp::pairingRespond(bool accept) {
    if (pairing_.state != "awaiting_user" || !pairing_.conn) return;
    pairing_.conn->pairingDecision(accept);
    if (accept) setPairing("awaiting_peer");
}

void HostApp::cancelPairing() {
    ++pairing_.attempt;
    if (pairing_.conn) {
        pairing_.conn->pairingDecision(false);
        pairing_.conn->close("cancelled");
        pairing_.conn.reset();
    }
    if (pairing_.state != "idle") setPairing("idle");
}

// ======================================================================= audio

void HostApp::setA2dp(bool enable) {
    if (enable) {
        if (loopback_->running()) setLanSend(false); // never both: audio would loop
        auto ls = cm_->status();
        std::string name = ls.peerName;
        uint64_t addr = 0;
        std::string peerId = ls.peerId;
        if (peerId.empty() && devices_.devices().size() == 1) peerId = devices_.devices().front().id;
        if (auto* d = devices_.find(peerId)) {
            addr = d->btAddress;
            if (name.empty()) name = d->name;
        }
        a2dp_->enable(name, addr);
    } else {
        a2dp_->disable();
    }
    markDirty();
}

void HostApp::setLanSend(bool enable) {
    if (!enable) {
        if (loopback_->running() || audioOffer_) {
            loopback_->stop("user");
            if (audioOffer_ && cm_->isUp())
                cm_->send(proto::types::AudioStop, {{"streamId", audioOffer_->streamId}, {"reason", "user"}});
            audioOffer_.reset();
        }
        markDirty();
        return;
    }
    const auto* caps = cm_->peerCaps();
    if (!caps || std::find(caps->begin(), caps->end(), proto::caps::AudioRecvPcm) == caps->end()) {
        log::warn("host", "phone does not accept PC audio (enable it in the phone app)");
        markDirty();
        return;
    }
    auto route = cm_->audioRoute();
    if (!route) {
        log::warn("host", "PC audio streaming needs the phone on the same LAN");
        markDirty();
        return;
    }
    if (a2dp_->active()) a2dp_->disable();
    AudioOffer o;
    o.streamId = crypto::randomHex(16);
    o.key = route->keyC2S;
    o.host = route->host;
    o.offerId = cm_->sendVia(route->connId, proto::types::AudioOffer,
                             {{"streamId", o.streamId},
                              {"direction", "windows_to_android"},
                              {"codec", "pcm_s16le"},
                              {"sampleRate", 48000},
                              {"channels", 2},
                              {"frameMs", 10},
                              {"transport", "tcp"}},
                             std::nullopt);
    if (o.offerId) audioOffer_ = std::move(o);
    markDirty();
}

// ======================================================================= IPC

void HostApp::handleIpc(const std::string& line) {
    json m = json::parse(line, nullptr, false);
    if (!m.is_object()) return;
    std::string type = m.value("type", std::string());
    log::debug("ipc", "<- {}", line.size() > 300 ? line.substr(0, 300) : line);
    auto result = [&](bool ok, const std::string& error = {}) {
        if (!m.contains("rid")) return;
        json r = {{"type", "result"}, {"rid", m["rid"]}, {"ok", ok}};
        if (!error.empty()) r["error"] = error;
        ipc_->send(r.dump());
    };
    // This PC only: commands that need a phone are refused (the UI hides them anyway).
    static const char* const kNeedsPhone[] = {"pair.start", "device.connect", "connect.manual"};
    bool enabling = m.value("enable", false);
    if (standalone_ && (std::find(std::begin(kNeedsPhone), std::end(kNeedsPhone), type) != std::end(kNeedsPhone) ||
                        (enabling && (type == "discover" || type == "audio.a2dpSink" || type == "audio.lanSend"))))
        return result(false, "standalone");
    try {
        if (type == "media.command") {
            Side side = m.value("side", std::string("remote")) == "local" ? Side::Local : Side::Remote;
            auto act = media::actionFromString(m.value("action", std::string()));
            if (!act) return result(false, "unknown action");
            std::optional<std::string> key;
            if (m.contains("key") && m["key"].is_string()) key = m["key"].get<std::string>();
            std::optional<int64_t> pos;
            if (m.contains("positionMs") && m["positionMs"].is_number()) pos = m["positionMs"].get<int64_t>();
            std::string custom = m.value("customAction", std::string());
            std::optional<int64_t> queueId;
            if (m.contains("queueId") && m["queueId"].is_number_integer()) queueId = m["queueId"].get<int64_t>();
            if (*act == CommandAction::Custom && custom.empty()) return result(false, "customAction missing");
            if (*act == CommandAction::PlayQueueItem && !queueId) return result(false, "queueId missing");
            if (side == Side::Remote && !cm_->isUp()) return result(false, "not connected");
            sendUserCommand(side, key, *act, pos, "ui", std::move(custom), queueId);
            return result(true);
        }
        if (type == "handoff.start") {
            // Continue the session on the other device (docs/ipc.md); answered when it is done.
            auto from = m.value("side", std::string("remote")) == "local" ? handoff::Device::Pc : handoff::Device::Phone;
            std::string key = m.value("key", std::string());
            std::optional<handoff::App> app;
            if (m.contains("app") && m["app"].is_string()) app = handoff::appFromString(m["app"].get<std::string>());
            json rid = m.contains("rid") ? m["rid"] : json(nullptr);
            handoff_->start(from, key, app, [this, rid](const handoff::Outcome& o) {
                if (rid.is_null() || !ipc_ || !ipc_->connected()) return;
                json r = {{"type", "result"}, {"rid", rid}, {"ok", o.ok}};
                if (!o.ok) r["error"] = o.error;
                ipc_->send(r.dump());
            });
            return;
        }
        if (type == "update.check") {
            updater_->check(true);
            return result(true);
        }
        if (type == "update.install") {
            updater_->install();
            return result(true);
        }
        if (type == "focus.set") {
            if (m.value("auto", false) || !m.contains("key") || m["key"].is_null()) {
                focusAuto_ = true;
            } else {
                focusAuto_ = false;
                focusSide_ = m.value("side", std::string("remote")) == "local" ? Side::Local : Side::Remote;
                focusKey_ = m["key"].get<std::string>();
            }
            updateFocus();
            markDirty();
            return result(true);
        }
        if (type == "settings.set") {
            json patch = m.value("patch", json::object());
            // Choosing a work mode anywhere (first-run panel, Settings > General) ends the first-run question.
            if (patch.is_object() && patch.contains("mode")) patch["modeChosen"] = true;
            settings_.apply(patch);
            if (!settings_.save(settingsPath_)) log::warn("host", "could not save settings");
            applySettings();
            if (ipc_->connected())
                ipc_->send(json{{"type", "hello"}, {"version", MD_VERSION}, {"pid", GetCurrentProcessId()},
                                {"language", languageTag()}}
                               .dump());
            return result(true);
        }
        if (type == "pair.start") {
            startPairing(m.value("kind", std::string("lan")), m.value("target", std::string()));
            return result(true);
        }
        if (type == "pair.respond") {
            pairingRespond(m.value("accept", false));
            return result(true);
        }
        if (type == "pair.cancel") {
            cancelPairing();
            return result(true);
        }
        if (type == "device.forget") {
            std::string id = m.value("id", std::string());
            if (cm_->activePeer() == id) cm_->disconnect(id);
            bool ok = devices_.remove(id);
            devices_.save();
            reloadPeers();
            return result(ok, ok ? "" : "unknown device");
        }
        if (type == "device.connect") {
            cm_->connectNow(m.value("id", std::string()));
            return result(true);
        }
        if (type == "device.disconnect") {
            cm_->disconnect(m.value("id", std::string()));
            return result(true);
        }
        if (type == "connect.manual") {
            std::string host = m.value("host", std::string());
            int port = m.value("port", static_cast<int>(platform::kDefaultLanPort));
            if (host.empty() || port <= 0 || port > 65535) return result(false, "invalid address");
            link::LanEndpoint ep{host, static_cast<uint16_t>(port), "manual"};
            // Applies to the active / only paired phone.
            platform::DeviceRecord* d = nullptr;
            if (auto id = cm_->activePeer()) d = devices_.find(*id);
            if (!d && devices_.devices().size() == 1) d = devices_.find(devices_.devices().front().id);
            if (!d) {
                startPairing("lan", ep.str());
                return result(true);
            }
            if (std::find(d->manualEndpoints.begin(), d->manualEndpoints.end(), ep) == d->manualEndpoints.end())
                d->manualEndpoints.insert(d->manualEndpoints.begin(), ep);
            devices_.save();
            reloadPeers();
            cm_->connectNow(d->id);
            return result(true);
        }
        if (type == "discover") {
            discovering_ = m.value("enable", false);
            if (discovering_) bt_->discover();
            updateDiscoveryWatchers();
            markDirty();
            return result(true);
        }
        if (type == "audio.a2dpSink") {
            setA2dp(m.value("enable", false));
            return result(true);
        }
        if (type == "audio.lanSend") {
            setLanSend(m.value("enable", false));
            return result(true);
        }
        if (type == "volume.set") {
            bool remote = m.value("side", std::string("local")) == "remote";
            if (remote) {
                if (!cm_->isUp() || !remoteVol_.available) return result(false, "remote volume unavailable");
                json body = json::object();
                if (m.contains("level") && m["level"].is_number()) body["level"] = std::clamp(m["level"].get<double>(), 0.0, 1.0);
                if (m.contains("muted") && m["muted"].is_boolean()) body["muted"] = m["muted"];
                if (m.contains("delta") && m["delta"].is_number_integer()) body["delta"] = m["delta"];
                uint64_t id = cm_->send(proto::types::VolumeSet, body);
                if (!id) return result(false, "not connected");
                // Answered when the phone confirms (volume.state) or refuses (error, e.g.
                // background_restricted); an older phone that never answers counts as ok.
                answerVolumeSet(true, ""); // a newer request supersedes an unanswered one
                pendingVolume_ = PendingVolume{id, m.contains("rid") ? m["rid"] : json(nullptr)};
                volumeAnswerTimer_.start(3000, [this] { answerVolumeSet(true, ""); });
                return;
            } else if (m.contains("key") && m["key"].is_string()) {
                auto* s = local_.find(m["key"].get<std::string>());
                if (!s) return result(false, "unknown session");
                std::optional<float> lvl;
                std::optional<bool> mute;
                if (m.contains("level") && m["level"].is_number()) lvl = static_cast<float>(m["level"].get<double>());
                if (m.contains("muted") && m["muted"].is_boolean()) mute = m["muted"].get<bool>();
                if (!audio_->setAppVolume(processFor(*s), lvl, mute)) return result(false, "app volume unavailable");
            } else {
                if (m.contains("level") && m["level"].is_number()) volume_->setLevel(static_cast<float>(m["level"].get<double>()));
                if (m.contains("muted") && m["muted"].is_boolean()) volume_->setMuted(m["muted"].get<bool>());
                if (m.contains("delta") && m["delta"].is_number_integer()) volume_->step(m["delta"].get<int>());
            }
            return result(true);
        }
        if (type == "lyrics.overlay") {
            bool v = m.value("visible", false);
            settings_.apply({{"lyrics", {{"overlay", {{"visible", v}}}}}});
            settings_.save(settingsPath_);
            overlay_->setEnabled(v);
            markDirty();
            return result(true);
        }
        if (type == "lyrics.offset") {
            std::string appId = m.value("appId", std::string());
            if (appId.empty())
                if (auto* f = focused()) appId = f->appId;
            if (appId.empty() || appId.size() > 260) return result(false, "no app");
            bool reset = m.value("reset", false);
            int delta = 0;
            if (!reset) {
                if (!m.contains("deltaMs") || !m["deltaMs"].is_number_integer()) return result(false, "bad deltaMs");
                delta = static_cast<int>(std::clamp<int64_t>(m["deltaMs"].get<int64_t>(), -10000, 10000));
            }
            adjustLyricsOffset(appId, delta, reset);
            return result(true);
        }
        if (type == "diagnostics") {
            settings_.apply({{"diagnostics", m.value("enable", false)}});
            settings_.save(settingsPath_);
            applySettings();
            return result(true);
        }
        if (type == "host.exit") {
            result(true);
            exitApp();
            return;
        }
        result(false, "unknown message type");
    } catch (const std::exception& ex) {
        log::warn("ipc", "bad UI message: {}", ex.what());
        result(false, "bad message");
    }
}

void HostApp::markDirty() {
    if (exiting_) return;
    dirty_ = true;
    if (!stateTimer_.active()) stateTimer_.start(kStateCoalesceMs, [this] {
            if (dirty_) pushState();
        });
}

void HostApp::pushState() {
    dirty_ = false;
    updateTray();
    if (!ipc_ || !ipc_->connected()) return;
    ipc_->send(buildState().dump(-1, ' ', false, json::error_handler_t::replace));
}

json HostApp::sessionJson(const MediaSessionInfo& s, Side side) const {
    policy::PState ps = arbiter_->stateOf(side == Side::Local ? policy::Side::Windows : policy::Side::Android, s.key);
    json j = media::toWire(s, sched_.nowMs());
    j["atTick"] = sched_.nowMs();
    auto path = artworkPath(s);
    j["artworkPath"] = path.empty() ? json(nullptr) : json(path);
    j["policyState"] = policy::toString(ps);
    j.erase("artwork");
    if (auto ca = j.find("customActions"); ca != j.end()) {
        for (auto& a : *ca) {
            std::string hash = a.contains("icon") ? a["icon"].value("hash", std::string()) : std::string();
            a["iconPath"] = !hash.empty() && art_->has(hash) ? json(text::toUtf8(art_->pathFor(hash).wstring())) : json(nullptr);
            a.erase("icon");
        }
    }
    j["source"] = side == Side::Remote ? "remote" : s.key == platform::NeteaseCompat::kKey ? "compat" : "smtc";
    j["positionEstimated"] = netease_->positionEstimated(s.key);
    // The UI shows lyrics at position − lyricsDelayMs (the host is the single source of truth).
    j["outputLatencyMs"] = outputLatencyFor(s, side);
    j["lyricsDelayMs"] = lyricsDelayFor(s, side);
    if (side == Side::Local) {
        j["appVolume"] = s.appVolume ? json{{"available", true}, {"level", *s.appVolume}, {"muted", s.appMuted}}
                                     : json{{"available", false}};
        j.erase("volume");
    }
    return j;
}

json HostApp::peerAppFor(const link::LinkStatus& ls) const {
    // This session's DeviceInfo.app, else the version the phone reported last time.
    std::string app = ls.peerApp;
    if (app.empty())
        for (auto& d : devices_.devices())
            if (d.id == ls.peerId) app = d.app;
    return app.empty() ? json(nullptr) : json(app);
}

json HostApp::buildState() const {
    auto ls = cm_->status();
    using S = link::LinkStatus::State;
    const char* status = ls.state == S::NoPeer ? "no_peer" : ls.state == S::Connecting ? "connecting"
                         : ls.state == S::Connected                                 ? "connected"
                                                                                    : "disconnected";
    json conn = {{"status", status},
                 {"transport", ls.primary ? json(link::toString(*ls.primary)) : json(nullptr)},
                 {"secondary", ls.secondary ? json(link::toString(*ls.secondary)) : json(nullptr)},
                 {"peer", ls.peerId.empty() ? json(nullptr)
                                            : json{{"id", ls.peerId}, {"name", ls.peerName}, {"platform", ls.peerPlatform},
                                                   {"app", peerAppFor(ls)},
                                                   {"versionMatch", version::toString(version::compare(
                                                       MD_VERSION, peerAppFor(ls).is_string() ? peerAppFor(ls).get<std::string>() : ""))}}},
                 {"sinceTick", ls.sinceMs},
                 {"rttMs", ls.rttMs},
                 {"mode", settings_.get<std::string>("connection.mode", "auto")},
                 {"lastError", ls.lastError.empty() ? json(nullptr) : json(ls.lastError)},
                 {"bytesIn", ls.bytesIn},
                 {"bytesOut", ls.bytesOut},
                 {"bluetooth", {{"adapter", btAdapter_}, {"on", btRadio_}}}};
    json devs = json::array();
    for (auto& d : devices_.devices()) {
        bool connected = ls.state == S::Connected && ls.peerId == d.id;
        std::string lanHost = d.manualEndpoints.empty() ? "" : d.manualEndpoints.front().str();
        devs.push_back({{"id", d.id},
                        {"name", d.name},
                        {"platform", d.platform},
                        {"btAddress", d.btAddress ? platform::formatBtAddress(d.btAddress) : ""},
                        {"lanHost", lanHost},
                        {"connected", connected},
                        {"transport", connected && ls.primary ? json(link::toString(*ls.primary)) : json(nullptr)},
                        {"lastSeen", d.lastSeen},
                        // Last app version the phone reported (kept while disconnected) and how it
                        // compares with this PC's (md::version: equal | patch | mismatch | unknown).
                        {"app", d.app.empty() ? json(nullptr) : json(d.app)},
                        {"versionMatch", version::toString(version::compare(MD_VERSION, d.app))}});
    }
    auto snapJson = [&](const media::MediaSnapshot& s, Side side) {
        json arr = json::array();
        for (auto& x : s.sessions) arr.push_back(sessionJson(x, side));
        return json{{"gen", s.gen}, {"active", s.active ? json(*s.active) : json(nullptr)}, {"sessions", arr}};
    };
    const auto& last = arbiter_->lastAction();
    json outputs = json::array();
    for (auto& o : volume_->outputs())
        outputs.push_back({{"id", text::toUtf8(o.id)}, {"name", o.name}, {"default", o.isDefault}});
    json policy = {{"mode", policy::toString(arbiter_->config().mode)},
                   {"gated", gated_},
                   {"output", localVol_.deviceName},
                   {"enabled", arbiter_->config().mode != policy::Mode::Off},
                   {"suspended", arbiter_->suspended()},
                   {"remote", policy::toString(arbiter_->androidState())},
                   {"windowsAudioActive", arbiter_->windowsAudioActive()},
                   {"activeApps", arbiter_->windowsAudioApps()}};
    if (!last.what.empty())
        policy["lastAction"] = {{"what", last.what}, {"reason", last.reason}, {"app", last.app}, {"atTick", last.atMs},
                                {"failed", last.failed}};
    auto a2 = a2dp_->status();
    auto lb = loopback_->status();
    const auto* caps = cm_->peerCaps();
    bool peerRecv = caps && std::find(caps->begin(), caps->end(), proto::caps::AudioRecvPcm) != caps->end();
    json audio = {{"a2dpSink", {{"available", a2.available}, {"state", a2.state}, {"device", a2.device},
                                {"error", a2.error.empty() ? json(nullptr) : json(a2.error)},
                                {"errorCode", a2.errorCode.empty() ? json(nullptr) : json(a2.errorCode)}}},
                  {"lanSend", {{"available", peerRecv && cm_->audioRoute().has_value()},
                               {"state", audioOffer_ && lb.state == "off" ? std::string("connecting") : lb.state},
                               {"error", lb.error.empty() ? json(nullptr) : json(lb.error)},
                               {"framesSent", lb.framesSent},
                               {"framesDropped", lb.framesDropped}}}};
    json pairing = {{"state", pairing_.state},
                    {"sas", pairing_.sas.empty() ? json(nullptr) : json(pairing_.sas)},
                    {"peerName", pairing_.peerName.empty() ? json(nullptr) : json(pairing_.peerName)},
                    {"error", pairing_.error.empty() ? json(nullptr) : json(pairing_.error)}};
    json discovered = json::array();
    for (auto& [id, c] : btCandidates_) {
        bool paired = false;
        for (auto& d : devices_.devices()) paired |= d.btAddress == c.address;
        discovered.push_back({{"kind", "bt"}, {"id", id}, {"name", c.name},
                              {"detail", platform::formatBtAddress(c.address)}, {"paired", paired},
                              {"ready", c.hasService}});
    }
    for (auto& s : dnssd_->current()) {
        discovered.push_back({{"kind", "lan"}, {"id", s.endpoint.str()}, {"name", s.name}, {"detail", s.endpoint.str()},
                              {"paired", devices_.devices().end() !=
                                             std::find_if(devices_.devices().begin(), devices_.devices().end(),
                                                          [&](auto& d) { return d.id == s.deviceId; })},
                              {"ready", true}});
    }
    return {{"type", "state"},
            {"tick", sched_.nowMs()},
            {"self", {{"id", identity_.id}, {"name", identity_.name}, {"app", MD_VERSION}}},
            {"standalone", standalone_},
            {"connection", conn},
            {"devices", devs},
            {"local", snapJson(decoratedLocal(), Side::Local)},
            {"audioOutputs", outputs},
            {"taskbar", {{"supported", true}, {"shown", taskbarShown_},
                         {"reason", taskbarReason_.empty() ? json(nullptr) : json(taskbarReason_)}}},
            {"remote", snapJson(remote_, Side::Remote)},
            {"focus", {{"side", sideName(focusSide_ == Side::Local ? 0 : 1)},
                       {"key", focusKey_.empty() ? json(nullptr) : json(focusKey_)},
                       {"auto", focusAuto_}}},
            {"policy", policy},
            {"audio", audio},
            {"pairing", pairing},
            {"discovered", discovered},
            {"lyricsOverlay", {{"visible", overlay_->enabled()}, {"shown", overlay_->windowVisible()}}},
            {"volume", {{"local", {{"available", localVol_.available}, {"level", localVol_.level}, {"muted", localVol_.muted}, {"steps", localVol_.steps}}},
                        {"remote", {{"available", remoteVol_.available}, {"level", remoteVol_.level}, {"muted", remoteVol_.muted}, {"steps", remoteVol_.steps},
                                    {"error", remoteVolError_.empty() ? json(nullptr) : json(remoteVolError_)}}}}},
            {"audioOutputLatency", {{"bluetooth", localVol_.bluetooth}, {"ms", localOutputLatencyMs()}}},
            {"settings", settings_.data()},
            {"handoff", handoff_ ? handoff_->stateJson() : json(nullptr)},
            {"update", updater_ ? updater_->status() : json(nullptr)},
            {"diagnostics", {{"enabled", diagnostics_}}}};
}

void HostApp::sendLyricsToUi() {
    if (!ipc_->connected()) return;
    const auto& r = lyrics_->current();
    json m = {{"type", "lyrics"},
              {"trackId", r.trackId},
              {"side", sideName(focusSide_ == Side::Local ? 0 : 1)},
              {"key", focusKey_},
              {"status", r.status},
              {"source", r.source}};
    // The song these lyrics belong to (docs/ipc.md): lets the UI keep showing them while
    // its state still carries another trackId for the same song (see lyrics::sameSong).
    if (!r.trackId.empty() && r.trackId == lyricsTrack_ && !lyricsSong_.title.empty()) {
        m["title"] = lyricsSong_.title;
        m["artist"] = lyricsSong_.artist;
        m["durationMs"] = lyricsSong_.durationMs ? json(*lyricsSong_.durationMs) : json(nullptr);
    }
    json lj = lyrics::toJson(r.lyrics);
    m["synced"] = lj["synced"];
    m["lines"] = lj["lines"];
    m["hasTranslation"] = lyrics::hasTranslation(r.lyrics);
    ipc_->send(m.dump(-1, ' ', false, json::error_handler_t::replace));
}

void HostApp::openUi(const std::string& window) {
    if (ipc_->connected()) {
        ipc_->send(json{{"type", "show"}, {"window", window}}.dump());
        return;
    }
    pendingShow_ = window;
    if (sched_.nowMs() - uiLaunchedAt_ < 8000 && uiLaunchedAt_ != 0) return; // already starting
    auto dir = exeDir();
    std::filesystem::path ui = dir / L"MediaDuetUI.exe";
    if (!std::filesystem::exists(ui)) ui = dir / L"ui" / L"MediaDuetUI.exe";
    if (!std::filesystem::exists(ui)) {
        log::error("host", "MediaDuetUI.exe not found next to the host");
        MessageBoxW(nullptr, tr("ui.missing"), tr("app"), MB_OK | MB_ICONWARNING);
        return;
    }
    std::wstring args = L"--show " + text::toWide(window);
    HINSTANCE h = ShellExecuteW(nullptr, L"open", ui.c_str(), args.c_str(), ui.parent_path().c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(h) <= 32) log::error("host", "could not start the UI");
    else uiLaunchedAt_ = sched_.nowMs();
}

void HostApp::updateTray() {
    if (!tray_) return;
    auto ls = cm_->status();
    using S = link::LinkStatus::State;
    std::wstring tip = L"MediaDuet\n";
    tip += ls.state == S::NoPeer        ? tr("status.no_peer")
           : ls.state == S::Connecting   ? tr("status.connecting")
           : ls.state == S::Disconnected ? tr("status.disconnected")
           : (ls.primary == link::TransportKind::Bluetooth ? tr("status.bt") : tr("status.lan"));
    if (ls.state == S::Connected) tip += L" · " + text::toWide(ls.peerName);
    if (auto* f = focused()) tip += L"\n" + text::toWide(text::truncate(f->title, 40));
    tray_->setTooltip(tip);
    tray_->setConnected(ls.state == S::Connected);
}

void HostApp::tickDiagnostics() {
    if (!diagnostics_) return;
    auto s = diag_.sample();
    auto ls = cm_->status();
    if (ipc_->connected()) {
        json d = {{"type", "diagnostics"},
                  {"cpuPercent", s.cpuPercent},
                  {"workingSetMB", s.workingSetMB},
                  {"privateMB", s.privateMB},
                  {"threads", s.threads},
                  {"transport", ls.primary ? json(link::toString(*ls.primary)) : json(nullptr)},
                  {"connectedForSec", ls.state == link::LinkStatus::State::Connected ? (sched_.nowMs() - ls.sinceMs) / 1000 : 0},
                  {"bytesIn", ls.bytesIn},
                  {"bytesOut", ls.bytesOut},
                  {"msgsIn", ls.msgsIn},
                  {"msgsOut", ls.msgsOut},
                  {"rttMs", ls.rttMs},
                  {"lastCommandLatencyMs", lastCommandLatency_},
                  {"localSessions", local_.sessions.size()},
                  {"remoteSessions", remote_.sessions.size()},
                  {"audioSessionsActive", audio_->activeSessions()}};
        ipc_->send(d.dump());
    }
    log::debug("diag", "cpu {:.2f}% ws {:.1f} MB private {:.1f} MB threads {}", s.cpuPercent, s.workingSetMB,
               s.privateMB, s.threads);
    diagTimer_.start(2000, [this] { tickDiagnostics(); });
}

} // namespace md::app
