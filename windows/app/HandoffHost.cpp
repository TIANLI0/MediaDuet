#include "HandoffHost.h"

#include "md/util/Log.h"

namespace md::app {

using handoff::App;
using handoff::PlayResult;
using json = nlohmann::json;

namespace {
constexpr int64_t kRefreshMs = 5 * 60'000; // re-read which music apps are installed
}

HandoffHost::HandoffHost(IScheduler& sched, Callbacks cb) : sched_(sched), cb_(std::move(cb)), launcher_(sched) {
    localTimer_.bind(&sched_);
    localTimeout_.bind(&sched_);
    refreshTimer_.bind(&sched_);
    handoff::Coordinator::Env env;
    env.session = [this](Device d, const std::string& key) { return session(d, key); };
    env.apps = [this](Device d) {
        if (d == Device::Pc) return launcher_.apps();
        return cb_.peerReady() ? remoteApps_ : std::vector<App>{};
    };
    env.play = [this](Device to, App app, const handoff::Song& song, const media::CatalogIds& ids,
                      std::function<void(const PlayResult&)> done) {
        if (to == Device::Pc) playLocal(app, song, ids, std::move(done));
        else playRemote(app, song, ids, std::move(done));
    };
    env.pause = [this](Device d, const std::string& key) { cb_.command(d, key, media::CommandAction::Pause, std::nullopt); };
    env.seek = [this](Device d, const std::string& key, int64_t pos) {
        auto* s = session(d, key);
        if (d == Device::Pc && s && handoff::appOfSession(s->appId) == App::Netease &&
            (!s->supports(media::CommandAction::Seek) || cb_.positionEstimated(key))) {
            cb_.neteaseSeek(key, pos);
            return;
        }
        cb_.command(d, key, media::CommandAction::Seek, pos);
    };
    env.commandLatencyMs = [this](Device d) { return d == Device::Phone ? std::max<int64_t>(cb_.rttMs(), 0) / 2 : int64_t(0); };
    coord_ = std::make_unique<handoff::Coordinator>(sched_, std::move(env), [this] {
        if (cb_.changed) cb_.changed();
    });
    refreshTimer_.start(kRefreshMs, [this] { refreshTick(); });
}

void HandoffHost::refreshTick() {
    if (launcher_.refresh()) {
        sendApps();
        if (cb_.changed) cb_.changed();
    }
    refreshTimer_.start(kRefreshMs, [this] { refreshTick(); });
}

HandoffHost::~HandoffHost() { *alive_ = false; }

const media::MediaSessionInfo* HandoffHost::session(Device d, const std::string& key) const {
    return (d == Device::Pc ? cb_.local() : cb_.remote()).find(key);
}

// ------------------------------------------------------------------ link

void HandoffHost::sendApps() {
    if (!cb_.peerReady()) return;
    json body = {{"apps", handoff::toJson(launcher_.apps())}};
    std::string s = body.dump();
    if (s == lastAppsSent_) return;
    if (cb_.send(proto::types::HandoffApps, body, std::nullopt)) lastAppsSent_ = s;
}

void HandoffHost::onLinkUp() {
    launcher_.refresh();
    lastAppsSent_.clear();
    sendApps();
    if (cb_.changed) cb_.changed();
}

void HandoffHost::onLinkDown() {
    remoteApps_.clear();
    lastAppsSent_.clear();
    if (remote_) {
        remote_.reset();
        coord_->onLinkLost();
    } else if (coord_->status().active && coord_->status().to == Device::Phone) {
        coord_->onLinkLost();
    }
    if (cb_.changed) cb_.changed();
}

bool HandoffHost::onEnvelope(const proto::Envelope& e) {
    const auto& t = e.type;
    if (t == proto::types::HandoffApps) {
        remoteApps_ = handoff::appsFromJson(e.body.value("apps", json::array()));
        log::info("handoff", "music apps on the phone: {}", handoff::toJson(remoteApps_).dump());
        if (cb_.changed) cb_.changed();
        return true;
    }
    if (t == proto::types::HandoffResult) {
        if (remote_ && e.re && *e.re == remote_->msgId) {
            auto done = std::move(remote_->done);
            remote_.reset();
            if (done) done(handoff::playResultFromJson(e.body));
        }
        return true;
    }
    if (t == proto::types::HandoffProgress) {
        if (remote_ && e.re && *e.re == remote_->msgId && e.body.value("state", std::string()) == "tap_to_continue")
            coord_->onWaitingForTap();
        return true;
    }
    if (t == proto::types::HandoffStart) {
        // The phone's "continue on …" button: this PC coordinates both directions.
        uint64_t id = e.id;
        std::string from = e.body.value("from", std::string("phone"));
        std::string key = e.body.value("key", std::string());
        std::optional<App> app;
        if (auto a = e.body.find("app"); a != e.body.end() && a->is_string()) app = handoff::appFromString(a->get<std::string>());
        log::info("handoff", "phone asked to move {} session {}", from, key);
        start(from == "pc" ? Device::Pc : Device::Phone, key, app, [this, id](const handoff::Outcome& o) {
            cb_.send(proto::types::HandoffResult, handoff::toJson(o), id);
        });
        return true;
    }
    if (t == proto::types::HandoffPlay) {
        // Only the phone plays on request; this PC coordinates instead.
        cb_.send(proto::types::HandoffResult, handoff::toJson(PlayResult{false, "", "", handoff::error::Unsupported}), e.id);
        return true;
    }
    return false;
}

void HandoffHost::start(Device from, const std::string& key, std::optional<App> app,
                        std::function<void(const handoff::Outcome&)> done) {
    coord_->start(from, key, app, [this, done = std::move(done)](const handoff::Outcome& o) {
        if (cb_.outcome) cb_.outcome(o);
        if (done) done(o);
    });
}

json HandoffHost::stateJson() const {
    json active = nullptr;
    const auto& st = coord_->status();
    if (st.active)
        active = {{"to", handoff::toString(st.to)},
                  {"app", st.app ? json(handoff::toString(*st.app)) : json(nullptr)},
                  {"title", st.title},
                  {"waitingForTap", st.waitingForTap}};
    return {{"apps", {{"local", handoff::toJson(launcher_.apps())},
                      {"remote", handoff::toJson(cb_.peerReady() ? remoteApps_ : std::vector<App>{})}}},
            {"active", active}};
}

// ------------------------------------------------------------------ the phone plays

void HandoffHost::playRemote(App app, const handoff::Song& song, const media::CatalogIds& ids,
                             std::function<void(const PlayResult&)> done) {
    json body = {{"app", handoff::toString(app)}, {"song", handoff::toJson(song)}};
    if (!ids.empty()) body["ids"] = media::toJson(ids);
    uint64_t id = cb_.peerReady() ? cb_.send(proto::types::HandoffPlay, body, std::nullopt) : 0;
    if (!id) {
        if (done) done(PlayResult{false, "", "", handoff::error::LinkLost});
        return;
    }
    remote_ = RemotePlay{id, std::move(done)};
}

// ------------------------------------------------------------------ this PC plays

void HandoffHost::playLocal(App app, const handoff::Song& song, const media::CatalogIds& ids,
                            std::function<void(const PlayResult&)> done) {
    LocalPlay p;
    p.id = ++localSeq_;
    p.app = app;
    p.song = song;
    p.done = std::move(done);
    local_ = std::move(p);
    uint64_t id = local_->id;
    std::weak_ptr<bool> alive = alive_;
    launcher_.launch(app, song, ids, [this, alive, id](const platform::HandoffLauncher::Launched& l) {
        if (alive.expired() || !local_ || local_->id != id) return;
        if (!l.ok) return finishLocal({false, "", "", l.error.empty() ? std::string(handoff::error::LaunchFailed) : l.error});
        // A search (Spotify without a track id): shown, never played by us.
        if (!l.exact) return finishLocal({true, "opened", "", ""});
        local_->launched = true;
        local_->launchedAt = sched_.nowMs();
        localTimeout_.start(kLocalWaitMs, [this, id] {
            if (!local_ || local_->id != id) return;
            log::info("handoff", "the song was opened on this PC but did not start within {} s", kLocalWaitMs / 1000);
            finishLocal({true, "opened", "", ""});
        });
        checkLocal();
    });
}

void HandoffHost::onLocalSnapshot() {
    if (local_ && local_->launched) checkLocal();
}

void HandoffHost::checkLocal() {
    if (!local_ || !local_->launched) return;
    const auto* t = handoff::findTarget(cb_.local(), local_->app, local_->song);
    if (t && t->status == media::PlaybackStatus::Playing) return finishLocal({true, "playing", t->key, ""});
    int64_t waited = sched_.nowMs() - local_->launchedAt;
    if (t && !local_->nudged && waited >= kPlayNudgeMs) {
        // The app shows the song but has not started it (it was already loaded, paused).
        local_->nudged = true;
        log::info("handoff", "{} shows the song but is paused: play", t->key);
        cb_.command(Device::Pc, t->key, media::CommandAction::Play, std::nullopt);
    }
    // Snapshots are event driven: re-check while waiting (nudge timing, apps without events).
    localTimer_.start(500, [this] { checkLocal(); });
}

void HandoffHost::finishLocal(PlayResult r) {
    localTimer_.cancel();
    localTimeout_.cancel();
    auto done = local_ ? std::move(local_->done) : nullptr;
    local_.reset();
    if (done) done(r);
}

} // namespace md::app
