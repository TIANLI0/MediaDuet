#include "md/handoff/Handoff.h"

#include "md/lyrics/Lrc.h"
#include "md/util/Bytes.h"
#include "md/util/Log.h"
#include "md/util/Text.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace md::handoff {

const char* toString(App a) {
    switch (a) {
    case App::Netease: return "netease";
    case App::QqMusic: return "qqmusic";
    case App::Spotify: return "spotify";
    }
    return "netease";
}

std::optional<App> appFromString(std::string_view s) {
    if (s == "netease") return App::Netease;
    if (s == "qqmusic") return App::QqMusic;
    if (s == "spotify") return App::Spotify;
    return std::nullopt;
}

std::optional<App> appOfSession(std::string_view appId) {
    std::string id = text::toLowerAscii(appId);
    if (id.find("cloudmusic") != std::string::npos) return App::Netease;
    if (id.find("qqmusic") != std::string::npos) return App::QqMusic;
    if (id.find("spotify") != std::string::npos) return App::Spotify;
    return std::nullopt;
}

json toJson(const std::vector<App>& apps) {
    json arr = json::array();
    for (auto a : apps) arr.push_back(toString(a));
    return arr;
}

std::vector<App> appsFromJson(const json& j) {
    std::vector<App> out;
    if (!j.is_array()) return out;
    for (auto& x : j) {
        if (!x.is_string()) continue;
        auto a = appFromString(x.get_ref<const std::string&>());
        if (a && std::find(out.begin(), out.end(), *a) == out.end()) out.push_back(*a);
    }
    return out;
}

Song songOf(const media::MediaSessionInfo& s) { return {s.title, s.artist, s.album, s.durationMs}; }

json toJson(const Song& s) {
    json j = {{"title", s.title}, {"artist", s.artist}, {"album", s.album}};
    j["durationMs"] = s.durationMs ? json(*s.durationMs) : json(nullptr);
    return j;
}

std::optional<Song> songFromJson(const json& j) {
    if (!j.is_object()) return std::nullopt;
    auto str = [&](const char* k) {
        auto it = j.find(k);
        return it != j.end() && it->is_string() ? text::truncate(it->get<std::string>(), 512) : std::string();
    };
    Song s{str("title"), str("artist"), str("album"), std::nullopt};
    if (text::trim(s.title).empty()) return std::nullopt;
    if (auto d = j.find("durationMs"); d != j.end() && d->is_number()) {
        double v = d->get<double>();
        if (std::isfinite(v) && v > 0 && v < 1e9) s.durationMs = static_cast<int64_t>(v);
    }
    return s;
}

std::string searchQuery(const Song& s) {
    std::string title = s.title;
    for (const char* open : {" (", "\xEF\xBC\x88", " [", "\xE3\x80\x90"}) { // ( （ [ 【
        auto p = title.find(open);
        if (p != std::string::npos && p > 0) title.resize(p);
    }
    std::string artist = s.artist;
    for (const char* sep : {"/", ",", "&", "\xE3\x80\x81", ";"}) { // 、
        auto p = artist.find(sep);
        if (p != std::string::npos && p > 0) artist.resize(p);
    }
    return text::trim(title + " " + artist);
}

bool shows(const media::MediaSessionInfo& s, const Song& want) {
    if (s.durationMs && want.durationMs && *s.durationMs > 0 && *want.durationMs > 0 &&
        std::llabs(*s.durationMs - *want.durationMs) > 5000)
        return false;
    std::string a = lyrics::normalize(s.title), b = lyrics::normalize(want.title);
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    // "起风了" vs "起风了 原曲ヤキモチ" style suffixes that normalize() does not strip; at least
    // two characters so that one-letter titles never match everything.
    const std::string& shorter = a.size() < b.size() ? a : b;
    const std::string& longer = a.size() < b.size() ? b : a;
    return text::codePoints(shorter) >= 2 && longer.rfind(shorter, 0) == 0;
}

const media::MediaSessionInfo* findTarget(const media::MediaSnapshot& snap, App app, const Song& want) {
    const media::MediaSessionInfo* best = nullptr;
    for (auto& s : snap.sessions) {
        if (appOfSession(s.appId) != app || !shows(s, want)) continue;
        bool playing = s.status == media::PlaybackStatus::Playing || s.status == media::PlaybackStatus::Buffering;
        if (!best || (playing && best->status != media::PlaybackStatus::Playing)) best = &s;
    }
    return best;
}

std::string neteaseDesktopCommand(const json& message) {
    std::string j = message.dump();
    return "orpheus://" + base64Encode(view(j));
}

std::string neteaseDesktopPlay(std::string_view songId) {
    return neteaseDesktopCommand({{"type", "song"}, {"id", std::string(songId)}, {"cmd", "play"}});
}

std::string neteaseDesktopSeek(int64_t positionMs) {
    // NetEase keeps positions in seconds (its own seek/forward commands use seconds).
    double sec = std::round(static_cast<double>(std::max<int64_t>(positionMs, 0)) / 100.0) / 10.0;
    return neteaseDesktopCommand({{"cmd", "seek"}, {"value", sec}});
}

std::string qqDesktopProtocol(int64_t songId) {
    // QQMusic.exe parses "key==value" pairs separated by "&&" (QQMusic_Protocol.dll:
    // version, cmd_count, cmd_N; playsong takes id_N and songtype_N).
    return "tencent://QQMusic/?version==1&&cmd_count==1&&cmd_0==playsong&&id_0==" + std::to_string(songId) +
           "&&songtype_0==0";
}

namespace {

std::string uriEscape(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

bool isBase62Id(std::string_view s) {
    return s.size() == 22 && std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
}

} // namespace

std::string spotifyTrackUri(std::string_view trackId) { return "spotify:track:" + std::string(trackId); }

std::string spotifySearchUri(const Song& s) { return "spotify:search:" + uriEscape(searchQuery(s)); }

std::optional<std::string> spotifyTrackId(std::string_view u) {
    for (std::string_view prefix : {"spotify:track:", "https://open.spotify.com/track/", "http://open.spotify.com/track/"}) {
        if (u.substr(0, prefix.size()) != prefix) continue;
        auto id = u.substr(prefix.size(), 22);
        if (isBase62Id(id) && (u.size() == prefix.size() + 22 || u[prefix.size() + 22] == '?' || u[prefix.size() + 22] == ':'))
            return std::string(id);
    }
    return std::nullopt;
}

const char* toString(Device d) { return d == Device::Pc ? "pc" : "phone"; }

PlayResult playResultFromJson(const json& j) {
    PlayResult r;
    if (!j.is_object()) {
        r.error = error::Unsupported;
        return r;
    }
    r.ok = j.value("ok", false);
    r.state = text::truncate(j.value("state", std::string(r.ok ? "playing" : "")), 32);
    r.key = text::truncate(j.value("key", std::string()), 300);
    if (auto e = j.find("error"); e != j.end()) {
        if (e->is_object()) r.error = text::truncate(e->value("code", std::string()), 64);
        else if (e->is_string()) r.error = text::truncate(e->get<std::string>(), 64);
    }
    if (!r.ok && r.error.empty()) r.error = "internal";
    if (r.ok && r.state != "playing" && r.state != "opened") r.state = "opened";
    return r;
}

json toJson(const PlayResult& r) {
    json j = {{"ok", r.ok}};
    if (r.ok) {
        j["state"] = r.state;
        if (!r.key.empty()) j["key"] = r.key;
    } else {
        j["error"] = {{"code", r.error}};
    }
    return j;
}

json toJson(const Outcome& o) {
    json j = {{"ok", o.ok}, {"to", toString(o.to)}, {"title", o.title}};
    j["app"] = o.app ? json(toString(*o.app)) : json(nullptr);
    if (o.ok) j["state"] = o.state;
    else j["error"] = {{"code", o.error}};
    return j;
}

// ------------------------------------------------------------------ coordinator

Coordinator::Coordinator(IScheduler& sched, Env env, std::function<void()> changed)
    : sched_(sched), env_(std::move(env)), changed_(std::move(changed)) {
    timeout_.bind(&sched_);
}

Coordinator::~Coordinator() { *alive_ = false; }

std::optional<App> Coordinator::chooseApp(Device to, const media::MediaSessionInfo& source, std::optional<App> wanted) const {
    auto avail = env_.apps ? env_.apps(to) : std::vector<App>{};
    if (avail.empty()) return std::nullopt;
    auto has = [&](App a) { return std::find(avail.begin(), avail.end(), a) != avail.end(); };
    if (wanted && has(*wanted)) return wanted;
    if (wanted) return std::nullopt; // the app asked for is not installed there
    if (auto own = appOfSession(source.appId); own && has(*own)) return own;
    return avail.front();
}

void Coordinator::start(Device from, const std::string& key, std::optional<App> wanted,
                        std::function<void(const Outcome&)> done) {
    Outcome o;
    o.to = other(from);
    if (run_) {
        o.error = error::Busy;
        if (done) done(o);
        return;
    }
    const media::MediaSessionInfo* src = env_.session ? env_.session(from, key) : nullptr;
    if (!src || text::trim(src->title).empty()) {
        o.error = error::NoSession;
        if (done) done(o);
        return;
    }
    o.title = src->title;
    auto app = chooseApp(o.to, *src, wanted);
    if (!app) {
        o.error = error::AppUnavailable;
        o.app = wanted;
        if (done) done(o);
        return;
    }
    Run r;
    r.id = ++seq_;
    r.from = from;
    r.key = key;
    r.song = songOf(*src);
    r.done = std::move(done);
    media::CatalogIds ids = src->ids;
    run_ = std::move(r);
    status_ = {true, o.to, app, src->title, false};
    log::info("handoff", "{} -> {}: '{}' in {}", toString(from), toString(o.to), src->title, toString(*app));
    if (changed_) changed_();
    uint64_t id = run_->id;
    timeout_.start(kTimeoutMs, [this, id] {
        if (!run_ || run_->id != id) return;
        Outcome t;
        t.to = other(run_->from);
        t.app = status_.app;
        t.title = status_.title;
        t.error = error::Timeout;
        finish(std::move(t));
    });
    std::weak_ptr<bool> alive = alive_;
    env_.play(o.to, *app, run_->song, ids, [this, alive, id, app](const PlayResult& pr) {
        if (alive.expired() || !run_ || run_->id != id) return;
        Outcome res;
        res.to = other(run_->from);
        res.app = app;
        res.title = status_.title;
        if (!pr.ok) {
            res.error = pr.error.empty() ? "internal" : pr.error;
            return finish(std::move(res));
        }
        res.ok = true;
        res.state = pr.state;
        if (pr.state == "playing") {
            // The source's live position (it kept playing while the target started).
            int64_t pos = -1;
            bool sourcePlaying = false;
            if (auto* s = env_.session(run_->from, run_->key)) {
                pos = s->positionAt(sched_.nowMs()).value_or(-1);
                sourcePlaying = s->status == media::PlaybackStatus::Playing || s->status == media::PlaybackStatus::Buffering;
            }
            if (sourcePlaying) env_.pause(run_->from, run_->key);
            if (pos >= kMinSeekMs && !pr.key.empty()) {
                int64_t lead = env_.commandLatencyMs ? env_.commandLatencyMs(res.to) : 0;
                env_.seek(res.to, pr.key, pos + std::clamp<int64_t>(lead, 0, 1000));
            }
            log::info("handoff", "'{}' continues on the {} at {} ms", status_.title, toString(res.to), pos);
        } else {
            log::info("handoff", "'{}' opened on the {} (playback not confirmed); the source keeps playing",
                      status_.title, toString(res.to));
        }
        finish(std::move(res));
    });
}

void Coordinator::onWaitingForTap() {
    if (!run_ || status_.waitingForTap) return;
    status_.waitingForTap = true;
    if (changed_) changed_();
}

void Coordinator::onLinkLost() {
    if (!run_) return;
    Outcome o;
    o.to = other(run_->from);
    o.app = status_.app;
    o.title = status_.title;
    o.error = error::LinkLost;
    finish(std::move(o));
}

void Coordinator::finish(Outcome o) {
    timeout_.cancel();
    auto done = run_ ? std::move(run_->done) : nullptr;
    run_.reset();
    status_ = {};
    if (!o.ok) log::info("handoff", "handoff to the {} failed: {}", toString(o.to), o.error);
    if (changed_) changed_();
    if (done) done(o);
}

} // namespace md::handoff
