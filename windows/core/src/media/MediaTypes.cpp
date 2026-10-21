#include "md/media/MediaTypes.h"

#include "md/crypto/Crypto.h"
#include "md/util/Text.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace md::media {

const char* toString(PlaybackStatus s) {
    switch (s) {
    case PlaybackStatus::Playing: return "playing";
    case PlaybackStatus::Paused: return "paused";
    case PlaybackStatus::Stopped: return "stopped";
    case PlaybackStatus::Buffering: return "buffering";
    default: return "unknown";
    }
}

std::optional<PlaybackStatus> statusFromString(std::string_view s) {
    if (s == "playing") return PlaybackStatus::Playing;
    if (s == "paused") return PlaybackStatus::Paused;
    if (s == "stopped") return PlaybackStatus::Stopped;
    if (s == "buffering") return PlaybackStatus::Buffering;
    if (s == "unknown") return PlaybackStatus::Unknown;
    return std::nullopt;
}

const char* toString(MediaKind k) {
    switch (k) {
    case MediaKind::Video: return "video";
    case MediaKind::Other: return "other";
    default: return "music";
    }
}

static constexpr std::pair<CommandAction, const char*> kActions[] = {
    {CommandAction::Play, "play"},         {CommandAction::Pause, "pause"}, {CommandAction::Toggle, "toggle"},
    {CommandAction::Next, "next"},         {CommandAction::Previous, "previous"},
    {CommandAction::Seek, "seek"},         {CommandAction::Stop, "stop"},
    {CommandAction::Custom, "custom"},     {CommandAction::PlayQueueItem, "playQueueItem"}};

const char* toString(CommandAction a) {
    for (auto& [k, n] : kActions)
        if (k == a) return n;
    return "toggle";
}

std::optional<CommandAction> actionFromString(std::string_view s) {
    for (auto& [k, n] : kActions)
        if (s == n) return k;
    return std::nullopt;
}

uint32_t actionBit(CommandAction a) {
    switch (a) {
    case CommandAction::Play: return action::Play;
    case CommandAction::Pause: return action::Pause;
    case CommandAction::Toggle: return action::Toggle;
    case CommandAction::Next: return action::Next;
    case CommandAction::Previous: return action::Previous;
    case CommandAction::Seek: return action::Seek;
    case CommandAction::Stop: return action::Stop;
    case CommandAction::Custom:
    case CommandAction::PlayQueueItem: return 0; // never in a session's action set
    }
    return 0;
}

const char* toString(Origin o) { return o == Origin::Policy ? "policy" : "user"; }

std::optional<int64_t> MediaSessionInfo::positionAt(int64_t nowMs) const {
    if (!positionMs) return std::nullopt;
    int64_t pos = *positionMs;
    if (status == PlaybackStatus::Playing) {
        pos += static_cast<int64_t>(std::llround(static_cast<double>(nowMs - positionAtMs) * rate));
    }
    if (pos < 0) pos = 0;
    if (durationMs && *durationMs > 0 && pos > *durationMs) pos = *durationMs;
    return pos;
}

bool MediaSessionInfo::equivalent(const MediaSessionInfo& o, int64_t tol) const {
    if (key != o.key || appId != o.appId || appName != o.appName || title != o.title || artist != o.artist ||
        album != o.album || albumArtist != o.albumArtist || durationMs != o.durationMs || status != o.status ||
        actions != o.actions || artwork != o.artwork || kind != o.kind || std::abs(rate - o.rate) > 1e-3 ||
        appVolume.has_value() != o.appVolume.has_value() || appMuted != o.appMuted ||
        (appVolume && std::abs(*appVolume - *o.appVolume) > 0.005f) ||
        outputLatencyMs.value_or(0) != o.outputLatencyMs.value_or(0) || customActions != o.customActions ||
        queue != o.queue || ids != o.ids)
        return false;
    if (positionMs.has_value() != o.positionMs.has_value()) return false;
    if (!positionMs) return true;
    int64_t t = std::max(positionAtMs, o.positionAtMs);
    auto a = positionAt(t), b = o.positionAt(t);
    return std::llabs(*a - *b) <= tol;
}

void MediaSessionInfo::updateTrackId() { trackId = computeTrackId(title, artist, album, durationMs); }

const MediaSessionInfo* MediaSnapshot::find(const std::string& k) const {
    for (auto& s : sessions)
        if (s.key == k) return &s;
    return nullptr;
}

const MediaSessionInfo* MediaSnapshot::activeSession() const { return active ? find(*active) : nullptr; }

bool MediaSnapshot::equivalent(const MediaSnapshot& o, int64_t tol) const {
    if (active != o.active || sessions.size() != o.sessions.size()) return false;
    for (size_t i = 0; i < sessions.size(); ++i)
        if (!sessions[i].equivalent(o.sessions[i], tol)) return false;
    return true;
}

std::string computeTrackId(const std::string& title, const std::string& artist, const std::string& album,
                           std::optional<int64_t> durationMs) {
    std::string data = title;
    data += '\x1f';
    data += artist;
    data += '\x1f';
    data += album;
    data += '\x1f';
    if (durationMs) data += std::to_string((*durationMs + 500) / 1000);
    return hexEncode(crypto::sha256(view(data))).substr(0, 16);
}

void sanitize(MediaSessionInfo& s) {
    s.key = text::truncate(s.key, 300);
    s.appId = text::truncate(s.appId, 256);
    s.appName = text::truncate(s.appName, 128);
    s.title = text::truncate(s.title, 512);
    s.artist = text::truncate(s.artist, 512);
    s.album = text::truncate(s.album, 512);
    s.albumArtist = text::truncate(s.albumArtist, 512);
    if (s.durationMs && *s.durationMs <= 0) s.durationMs.reset();
    if (!std::isfinite(s.rate) || s.rate < 0 || s.rate > 16) s.rate = 1.0;
}

static json actionsToJson(uint32_t a) {
    json arr = json::array();
    for (auto& [k, n] : kActions)
        if (a & actionBit(k)) arr.push_back(n);
    return arr;
}

json toWire(const MediaSessionInfo& s, int64_t nowMs) {
    json j = {{"key", s.key},
              {"app", {{"id", s.appId}, {"name", s.appName}}},
              {"title", s.title},
              {"artist", s.artist},
              {"album", s.album},
              {"albumArtist", s.albumArtist},
              {"trackId", s.trackId},
              {"rate", s.rate},
              {"status", toString(s.status)},
              {"actions", actionsToJson(s.actions)},
              {"kind", toString(s.kind)}};
    j["durationMs"] = s.durationMs ? json(*s.durationMs) : json(nullptr);
    auto pos = s.positionAt(nowMs);
    j["positionMs"] = pos ? json(*pos) : json(nullptr);
    if (s.artwork)
        j["artwork"] = {{"hash", s.artwork->hash}, {"mime", s.artwork->mime}, {"size", s.artwork->size}};
    else
        j["artwork"] = nullptr;
    if (s.appVolume) j["volume"] = {{"level", *s.appVolume}, {"muted", s.appMuted}};
    if (s.outputLatencyMs) j["outputLatencyMs"] = std::clamp(*s.outputLatencyMs, 0, 2000);
    if (!s.customActions.empty()) {
        json arr = json::array();
        for (auto& a : s.customActions) {
            json x = {{"id", a.id}, {"title", a.title}};
            if (a.icon) x["icon"] = {{"hash", a.icon->hash}, {"mime", a.icon->mime}, {"size", a.icon->size}};
            arr.push_back(std::move(x));
        }
        j["customActions"] = std::move(arr);
    }
    if (s.queue) {
        j["queue"] = {{"count", s.queue->count}, {"rev", s.queue->rev}};
        if (s.queue->activeId) j["queue"]["activeId"] = *s.queue->activeId;
    }
    if (!s.ids.empty()) j["ids"] = toJson(s.ids);
    return j;
}

json toJson(const CatalogIds& ids) {
    json j = json::object();
    if (!ids.netease.empty()) j["netease"] = ids.netease;
    if (!ids.qqmusic.empty()) j["qqmusic"] = ids.qqmusic;
    if (!ids.spotify.empty()) j["spotify"] = ids.spotify;
    return j;
}

CatalogIds catalogIdsFromJson(const json& j) {
    CatalogIds ids;
    if (!j.is_object()) return ids;
    auto get = [&](const char* k) -> std::string {
        auto it = j.find(k);
        if (it == j.end() || !it->is_string()) return {};
        const auto& v = it->get_ref<const std::string&>();
        if (v.empty() || v.size() > 64) return {};
        for (char c : v)
            if (!std::isalnum(static_cast<unsigned char>(c))) return {};
        return v;
    };
    ids.netease = get("netease");
    ids.qqmusic = get("qqmusic");
    ids.spotify = get("spotify");
    return ids;
}

namespace {
bool err(std::string* e, const char* m);
bool isSha256Hex(const std::string& h) {
    return h.size() == 64 && std::all_of(h.begin(), h.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
} // namespace

std::optional<MediaQueue> queueFromWire(const json& j, std::string* e) {
    if (!j.is_object() || !j.contains("key") || !j["key"].is_string()) {
        err(e, "queue key");
        return std::nullopt;
    }
    MediaQueue q;
    q.key = text::truncate(j["key"].get<std::string>(), 300);
    q.rev = text::truncate(j.value("rev", std::string()), 64);
    q.title = text::truncate(j.value("title", std::string()), 512);
    if (auto a = j.find("activeId"); a != j.end() && a->is_number_integer()) q.activeId = a->get<int64_t>();
    if (auto it = j.find("items"); it != j.end() && it->is_array()) {
        for (auto& x : *it) {
            if (q.items.size() >= 300) break;
            if (!x.is_object() || !x.contains("id") || !x["id"].is_number_integer()) continue;
            q.items.push_back({x["id"].get<int64_t>(), text::truncate(x.value("title", std::string()), 512),
                               text::truncate(x.value("subtitle", std::string()), 512)});
        }
    }
    auto num = [&](const char* k, int def) {
        auto it = j.find(k);
        return it != j.end() && it->is_number_integer() ? static_cast<int>(std::clamp<int64_t>(it->get<int64_t>(), 0, 100000)) : def;
    };
    q.total = num("total", static_cast<int>(q.items.size()));
    q.offset = num("offset", 0);
    return q;
}

json toJson(const MediaQueue& q) {
    json items = json::array();
    for (auto& i : q.items) items.push_back({{"id", i.id}, {"title", i.title}, {"subtitle", i.subtitle}});
    json j = {{"key", q.key}, {"rev", q.rev}, {"title", q.title}, {"total", q.total}, {"offset", q.offset}, {"items", items}};
    j["activeId"] = q.activeId ? json(*q.activeId) : json(nullptr);
    return j;
}

json toWire(const MediaSnapshot& s, int64_t nowMs) {
    json arr = json::array();
    for (auto& x : s.sessions) arr.push_back(toWire(x, nowMs));
    // at: our monotonic clock at which the positions hold (spec §8.3, §11.1).
    return {{"gen", s.gen}, {"at", nowMs}, {"active", s.active ? json(*s.active) : json(nullptr)}, {"sessions", std::move(arr)}};
}

namespace {

bool err(std::string* e, const char* m) {
    if (e) *e = m;
    return false;
}

bool getStr(const json& j, const char* k, size_t maxCp, std::string& out, bool required, std::string* e) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) {
        if (required) return err(e, k);
        out.clear();
        return true;
    }
    if (!it->is_string()) return err(e, k);
    const auto& s = it->get_ref<const std::string&>();
    if (text::codePoints(s) > maxCp) return err(e, k);
    out = s;
    return true;
}

bool getOptInt(const json& j, const char* k, std::optional<int64_t>& out, std::string* e) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) {
        out.reset();
        return true;
    }
    if (!it->is_number()) return err(e, k);
    double d = it->get<double>();
    if (!std::isfinite(d) || d < 0 || d > 1e12) return err(e, k);
    out = static_cast<int64_t>(d);
    return true;
}

} // namespace

std::optional<MediaSessionInfo> sessionFromWire(const json& j, int64_t receivedAtMs, int64_t latencyMs,
                                                std::string* e) {
    if (!j.is_object()) {
        err(e, "session not object");
        return std::nullopt;
    }
    MediaSessionInfo s;
    if (!getStr(j, "key", 300, s.key, true, e) || s.key.empty()) return std::nullopt;
    auto app = j.find("app");
    if (app != j.end() && app->is_object()) {
        if (!getStr(*app, "id", 256, s.appId, false, e) || !getStr(*app, "name", 128, s.appName, false, e))
            return std::nullopt;
    }
    if (!getStr(j, "title", 512, s.title, false, e) || !getStr(j, "artist", 512, s.artist, false, e) ||
        !getStr(j, "album", 512, s.album, false, e) || !getStr(j, "albumArtist", 512, s.albumArtist, false, e) ||
        !getStr(j, "trackId", 64, s.trackId, false, e))
        return std::nullopt;
    if (!getOptInt(j, "durationMs", s.durationMs, e) || !getOptInt(j, "positionMs", s.positionMs, e))
        return std::nullopt;
    auto st = j.find("status");
    if (st == j.end() || !st->is_string()) {
        err(e, "status");
        return std::nullopt;
    }
    s.status = statusFromString(st->get_ref<const std::string&>()).value_or(PlaybackStatus::Unknown);
    if (auto r = j.find("rate"); r != j.end() && r->is_number()) s.rate = r->get<double>();
    if (auto a = j.find("actions"); a != j.end() && a->is_array()) {
        for (auto& x : *a)
            if (x.is_string())
                if (auto act = actionFromString(x.get_ref<const std::string&>())) s.actions |= actionBit(*act);
    }
    if (auto k = j.find("kind"); k != j.end() && k->is_string()) {
        const auto& ks = k->get_ref<const std::string&>();
        s.kind = ks == "video" ? MediaKind::Video : ks == "other" ? MediaKind::Other : MediaKind::Music;
    }
    if (auto art = j.find("artwork"); art != j.end() && art->is_object()) {
        ArtworkRef ref;
        if (!getStr(*art, "hash", 64, ref.hash, true, e) || !getStr(*art, "mime", 64, ref.mime, false, e))
            return std::nullopt;
        if (ref.hash.size() != 64 || !hexDecode(ref.hash)) {
            err(e, "artwork hash");
            return std::nullopt;
        }
        if (auto sz = art->find("size"); sz != art->end() && sz->is_number_unsigned()) {
            uint64_t v = sz->get<uint64_t>();
            if (v > 1024 * 1024) {
                err(e, "artwork size");
                return std::nullopt;
            }
            ref.size = static_cast<uint32_t>(v);
        }
        s.artwork = ref;
    }
    if (auto v = j.find("volume"); v != j.end() && v->is_object()) {
        if (auto l = v->find("level"); l != v->end() && l->is_number())
            s.appVolume = static_cast<float>(std::clamp(l->get<double>(), 0.0, 1.0));
        s.appMuted = v->value("muted", false);
    }
    // Optional; an invalid or out-of-range value is ignored (treated as unknown).
    if (auto l = j.find("outputLatencyMs"); l != j.end() && l->is_number()) {
        double v = l->get<double>();
        if (std::isfinite(v) && v >= 0 && v <= 2000) s.outputLatencyMs = static_cast<int>(std::llround(v));
    }
    // Optional and lenient (forward compatible): malformed entries are skipped.
    if (auto ca = j.find("customActions"); ca != j.end() && ca->is_array()) {
        for (auto& x : *ca) {
            if (s.customActions.size() >= kMaxCustomActions) break;
            if (!x.is_object() || !x.contains("id") || !x["id"].is_string() || x["id"].get_ref<const std::string&>().empty())
                continue;
            CustomAction a;
            a.id = text::truncate(x["id"].get<std::string>(), 128);
            a.title = text::truncate(x.value("title", std::string()), 64);
            if (auto ic = x.find("icon"); ic != x.end() && ic->is_object() && ic->contains("hash") && (*ic)["hash"].is_string()) {
                ArtworkRef r;
                r.hash = (*ic)["hash"].get<std::string>();
                r.mime = ic->value("mime", std::string("image/png"));
                if (auto sz = ic->find("size"); sz != ic->end() && sz->is_number_unsigned()) r.size = sz->get<uint32_t>();
                if (isSha256Hex(r.hash) && r.size <= 1024 * 1024) a.icon = r;
            }
            s.customActions.push_back(std::move(a));
        }
    }
    if (auto qh = j.find("queue"); qh != j.end() && qh->is_object()) {
        auto c = qh->find("count");
        if (c != qh->end() && c->is_number_integer() && c->get<int64_t>() > 0) {
            QueueHint h;
            h.count = static_cast<int>(std::min<int64_t>(c->get<int64_t>(), 100000));
            h.rev = text::truncate(qh->value("rev", std::string()), 64);
            if (auto a = qh->find("activeId"); a != qh->end() && a->is_number_integer()) h.activeId = a->get<int64_t>();
            s.queue = h;
        }
    }
    if (auto ids = j.find("ids"); ids != j.end()) s.ids = catalogIdsFromJson(*ids);
    sanitize(s);
    if (s.trackId.empty()) s.updateTrackId();
    if (s.positionMs && s.status == PlaybackStatus::Playing)
        *s.positionMs += static_cast<int64_t>(static_cast<double>(latencyMs) * s.rate);
    s.positionAtMs = receivedAtMs;
    return s;
}

std::optional<int64_t> snapshotAt(const json& j) {
    if (!j.is_object()) return std::nullopt;
    auto a = j.find("at");
    if (a == j.end() || !a->is_number_integer()) return std::nullopt;
    int64_t v = a->get<int64_t>();
    if (v < 0) return std::nullopt;
    return v;
}

std::optional<MediaSnapshot> snapshotFromWire(const json& j, int64_t receivedAtMs, int64_t latencyMs,
                                              std::string* e) {
    if (!j.is_object()) {
        err(e, "snapshot not object");
        return std::nullopt;
    }
    MediaSnapshot snap;
    auto g = j.find("gen");
    if (g == j.end() || !g->is_number_unsigned()) {
        err(e, "gen");
        return std::nullopt;
    }
    snap.gen = g->get<uint64_t>();
    if (auto a = j.find("active"); a != j.end() && a->is_string()) snap.active = a->get<std::string>();
    auto ss = j.find("sessions");
    if (ss == j.end() || !ss->is_array() || ss->size() > kMaxSessions) {
        err(e, "sessions");
        return std::nullopt;
    }
    for (auto& x : *ss) {
        auto s = sessionFromWire(x, receivedAtMs, latencyMs, e);
        if (!s) return std::nullopt;
        snap.sessions.push_back(std::move(*s));
    }
    if (snap.active && !snap.find(*snap.active)) snap.active.reset();
    return snap;
}

json toWire(const MediaCommand& c) {
    json j = {{"key", c.key ? json(*c.key) : json(nullptr)},
              {"action", toString(c.action)},
              {"origin", toString(c.origin)},
              {"reason", c.reason}};
    if (c.positionMs) j["positionMs"] = *c.positionMs;
    if (c.action == CommandAction::Custom) j["customAction"] = c.customAction;
    if (c.queueId) j["queueId"] = *c.queueId;
    return j;
}

std::optional<MediaCommand> commandFromWire(const json& j, std::string* e) {
    if (!j.is_object()) {
        err(e, "command not object");
        return std::nullopt;
    }
    MediaCommand c;
    if (auto k = j.find("key"); k != j.end() && !k->is_null()) {
        if (!k->is_string() || text::codePoints(k->get_ref<const std::string&>()) > 300) {
            err(e, "key");
            return std::nullopt;
        }
        c.key = k->get<std::string>();
    }
    auto a = j.find("action");
    if (a == j.end() || !a->is_string()) {
        err(e, "action");
        return std::nullopt;
    }
    auto act = actionFromString(a->get_ref<const std::string&>());
    if (!act) {
        err(e, "unknown action");
        return std::nullopt;
    }
    c.action = *act;
    if (!getOptInt(j, "positionMs", c.positionMs, e)) return std::nullopt;
    if (c.action == CommandAction::Seek && !c.positionMs) {
        err(e, "seek without position");
        return std::nullopt;
    }
    if (auto ca = j.find("customAction"); ca != j.end() && ca->is_string())
        c.customAction = text::truncate(ca->get_ref<const std::string&>(), 128);
    if (c.action == CommandAction::Custom && c.customAction.empty()) {
        err(e, "custom without customAction");
        return std::nullopt;
    }
    if (!getOptInt(j, "queueId", c.queueId, e)) return std::nullopt;
    if (c.action == CommandAction::PlayQueueItem && !c.queueId) {
        err(e, "playQueueItem without queueId");
        return std::nullopt;
    }
    if (auto o = j.find("origin"); o != j.end() && o->is_string())
        c.origin = o->get_ref<const std::string&>() == "policy" ? Origin::Policy : Origin::User;
    if (auto r = j.find("reason"); r != j.end() && r->is_string())
        c.reason = text::truncate(r->get_ref<const std::string&>(), 128);
    return c;
}

} // namespace md::media
