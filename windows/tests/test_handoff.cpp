#include "md/handoff/Handoff.h"
#include "md/util/Bytes.h"
#include "md/util/ManualScheduler.h"

#include "TestStrings.h"

#include <doctest/doctest.h>

using namespace md;
using namespace md::handoff;
using media::MediaSessionInfo;
using media::PlaybackStatus;

namespace doctest {
template <>
struct StringMaker<App> {
    static String convert(App v) { return toString(v); }
};
} // namespace doctest

namespace {

MediaSessionInfo session(const std::string& key, const std::string& app, const std::string& title,
                         PlaybackStatus st = PlaybackStatus::Playing, std::optional<int64_t> dur = 325000) {
    MediaSessionInfo s;
    s.key = key;
    s.appId = app;
    s.title = title;
    s.artist = "买辣椒也用券";
    s.durationMs = dur;
    s.status = st;
    s.positionMs = 0;
    s.actions = media::action::Play | media::action::Pause | media::action::Seek;
    s.updateTrackId();
    return s;
}

json decodeOrpheus(const std::string& uri) {
    REQUIRE(uri.rfind("orpheus://", 0) == 0);
    auto raw = base64Decode(uri.substr(10));
    REQUIRE(raw);
    return json::parse(std::string(raw->begin(), raw->end()));
}

} // namespace

TEST_CASE("handoff: apps of sessions on both platforms") {
    CHECK(appOfSession("cloudmusic.exe") == App::Netease);
    CHECK(appOfSession("com.netease.cloudmusic") == App::Netease);
    CHECK(appOfSession("QQMusic.exe") == App::QqMusic);
    CHECK(appOfSession("com.tencent.qqmusic") == App::QqMusic);
    CHECK(appOfSession("Spotify.exe") == App::Spotify);
    CHECK(appOfSession("SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify") == App::Spotify);
    CHECK(appOfSession("com.spotify.music") == App::Spotify);
    CHECK_FALSE(appOfSession("chrome.exe"));
    CHECK(appsFromJson(json::array({"spotify", "bogus", "netease", "spotify", 3})) ==
          std::vector<App>{App::Spotify, App::Netease});
    CHECK(toJson(std::vector<App>{App::QqMusic}) == json::array({"qqmusic"}));
}

TEST_CASE("handoff: NetEase desktop web commands") {
    auto play = decodeOrpheus(neteaseDesktopPlay("1330348068"));
    CHECK(play["cmd"] == "play");
    CHECK(play["type"] == "song");
    CHECK(play["id"] == "1330348068");
    auto seek = decodeOrpheus(neteaseDesktopSeek(61'260));
    CHECK(seek["cmd"] == "seek");
    CHECK(seek["value"].get<double>() == doctest::Approx(61.3));
    CHECK(decodeOrpheus(neteaseDesktopSeek(-5))["value"].get<double>() == 0.0);
}

TEST_CASE("handoff: QQ Music and Spotify links") {
    CHECK(qqDesktopProtocol(293700875) ==
          "tencent://QQMusic/?version==1&&cmd_count==1&&cmd_0==playsong&&id_0==293700875&&songtype_0==0");
    CHECK(spotifyTrackUri("4uLU6hMCjMI75M1A2tKUQC") == "spotify:track:4uLU6hMCjMI75M1A2tKUQC");
    CHECK(spotifySearchUri({"晴天 (Live)", "周杰伦/五月天", "", std::nullopt}) ==
          "spotify:search:%E6%99%B4%E5%A4%A9%20%E5%91%A8%E6%9D%B0%E4%BC%A6");
    CHECK(spotifyTrackId("spotify:track:4uLU6hMCjMI75M1A2tKUQC") == "4uLU6hMCjMI75M1A2tKUQC");
    CHECK(spotifyTrackId("https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC?si=abc") == "4uLU6hMCjMI75M1A2tKUQC");
    CHECK_FALSE(spotifyTrackId("spotify:album:4uLU6hMCjMI75M1A2tKUQC"));
    CHECK_FALSE(spotifyTrackId("spotify:track:short"));
    CHECK_FALSE(spotifyTrackId("spotify:track:4uLU6hMCjMI75M1A2tKUQCx"));
}

TEST_CASE("handoff: the target shows the song") {
    Song want{"起风了", "买辣椒也用券", "起风了", 325000};
    // NetEase adds the original in brackets; QQ Music spells the artist differently.
    CHECK(shows(session("a", "com.netease.cloudmusic", "起风了 (原曲：《ヤキモチ》—高桥优)"), want));
    CHECK(shows(session("a", "com.tencent.qqmusic", "起风了"), want));
    CHECK(shows(session("a", "x", "起风了", PlaybackStatus::Playing, std::nullopt), want));
    CHECK_FALSE(shows(session("a", "x", "起风了", PlaybackStatus::Playing, 200000), want)); // other version
    CHECK_FALSE(shows(session("a", "x", "晴天"), want));
    CHECK_FALSE(shows(session("a", "x", ""), want));
    // Prefixes count only with two characters or more.
    CHECK_FALSE(shows(session("a", "x", "A Song"), {"A", "", "", std::nullopt}));

    media::MediaSnapshot snap;
    snap.sessions = {session("q#1", "com.tencent.qqmusic", "起风了", PlaybackStatus::Paused),
                     session("n#1", "com.netease.cloudmusic", "起风了", PlaybackStatus::Paused),
                     session("n#2", "com.netease.cloudmusic", "起风了", PlaybackStatus::Playing)};
    auto* t = findTarget(snap, App::Netease, want);
    REQUIRE(t);
    CHECK(t->key == "n#2");
    CHECK(findTarget(snap, App::Spotify, want) == nullptr);
}

TEST_CASE("handoff: songs and results on the wire") {
    auto s = songFromJson({{"title", "晴天"}, {"artist", "周杰伦"}, {"durationMs", 269000}});
    REQUIRE(s);
    CHECK(s->durationMs == 269000);
    CHECK_FALSE(songFromJson({{"title", "  "}}));
    CHECK_FALSE(songFromJson(json::array()));
    CHECK(searchQuery({"晴天 (Live)", "周杰伦/五月天", "", std::nullopt}) == "晴天 周杰伦");

    auto ok = playResultFromJson({{"ok", true}, {"state", "playing"}, {"key", "com.netease.cloudmusic#2"}});
    CHECK(ok.ok);
    CHECK(ok.key == "com.netease.cloudmusic#2");
    auto bad = playResultFromJson({{"ok", false}, {"error", {{"code", "not_found"}}}});
    CHECK_FALSE(bad.ok);
    CHECK(bad.error == "not_found");
    CHECK(playResultFromJson({{"ok", false}}).error == "internal");
    CHECK(playResultFromJson({{"ok", true}, {"state", "weird"}}).state == "opened");
    CHECK(toJson(bad)["error"]["code"] == "not_found");

    auto ids = media::catalogIdsFromJson({{"netease", "1330348068"}, {"qqmusic", "003hKob536i6FA"}, {"spotify", "bad id!"}});
    CHECK(ids.netease == "1330348068");
    CHECK(ids.qqmusic == "003hKob536i6FA");
    CHECK(ids.spotify.empty());
    auto wire = media::toWire(session("k", "a", "t"), 0);
    CHECK_FALSE(wire.contains("ids"));
    auto withIds = session("k", "a", "t");
    withIds.ids = ids;
    auto back = media::sessionFromWire(media::toWire(withIds, 0), 0, 0);
    REQUIRE(back);
    CHECK(back->ids == ids);
}

namespace {

struct Rig {
    ManualScheduler s;
    std::map<std::pair<Device, std::string>, MediaSessionInfo> sessions;
    std::map<Device, std::vector<App>> apps;
    struct Played {
        Device to;
        App app;
        Song song;
        media::CatalogIds ids;
        std::function<void(const PlayResult&)> done;
    };
    std::vector<Played> plays;
    std::vector<std::pair<Device, std::string>> pauses;
    std::vector<std::tuple<Device, std::string, int64_t>> seeks;
    int changes = 0;
    std::unique_ptr<Coordinator> c;

    Rig() {
        Coordinator::Env env;
        env.session = [this](Device d, const std::string& k) -> const MediaSessionInfo* {
            auto it = sessions.find({d, k});
            return it == sessions.end() ? nullptr : &it->second;
        };
        env.apps = [this](Device d) { return apps[d]; };
        env.play = [this](Device to, App a, const Song& song, const media::CatalogIds& ids,
                          std::function<void(const PlayResult&)> done) { plays.push_back({to, a, song, ids, std::move(done)}); };
        env.pause = [this](Device d, const std::string& k) { pauses.emplace_back(d, k); };
        env.seek = [this](Device d, const std::string& k, int64_t p) { seeks.emplace_back(d, k, p); };
        env.commandLatencyMs = [](Device d) { return d == Device::Phone ? int64_t(20) : int64_t(0); };
        c = std::make_unique<Coordinator>(s, env, [this] { ++changes; });
    }
    MediaSessionInfo& add(Device d, MediaSessionInfo m) {
        m.positionAtMs = s.nowMs();
        return sessions[{d, m.key}] = std::move(m);
    }
};

} // namespace

TEST_CASE("handoff coordinator: phone to PC, same app, live position, user pause") {
    Rig r;
    r.apps[Device::Pc] = {App::Spotify, App::Netease};
    auto& src = r.add(Device::Phone, session("com.netease.cloudmusic#1", "com.netease.cloudmusic", "起风了"));
    src.positionMs = 60'000;
    src.ids.netease = "1330348068";
    std::optional<Outcome> out;
    r.c->start(Device::Phone, src.key, std::nullopt, [&](const Outcome& o) { out = o; });
    REQUIRE(r.plays.size() == 1);
    CHECK(r.plays[0].to == Device::Pc);
    CHECK(r.plays[0].app == App::Netease); // the source's own app wins over the first one
    CHECK(r.plays[0].ids.netease == "1330348068");
    CHECK(r.c->status().active);
    CHECK_FALSE(out);
    r.s.advance(4000); // the PC took 4 s to start; the phone kept playing
    r.plays[0].done({true, "playing", "cloudmusic.exe#compat", ""});
    REQUIRE(out);
    CHECK(out->ok);
    CHECK(out->state == "playing");
    CHECK(out->app == App::Netease);
    REQUIRE(r.pauses.size() == 1);
    CHECK(r.pauses[0] == std::pair{Device::Phone, src.key});
    REQUIRE(r.seeks.size() == 1);
    CHECK(std::get<1>(r.seeks[0]) == "cloudmusic.exe#compat");
    CHECK(std::get<2>(r.seeks[0]) == 64'000);
    CHECK_FALSE(r.c->status().active);
}

TEST_CASE("handoff coordinator: PC to phone adds the link latency, no seek near the start") {
    Rig r;
    r.apps[Device::Phone] = {App::QqMusic};
    auto& src = r.add(Device::Pc, session("Spotify.exe", "Spotify.exe", "起风了"));
    src.positionMs = 30'000;
    std::optional<Outcome> out;
    r.c->start(Device::Pc, "Spotify.exe", std::nullopt, [&](const Outcome& o) { out = o; });
    REQUIRE(r.plays.size() == 1);
    CHECK(r.plays[0].app == App::QqMusic); // Spotify is not on the phone: its only app
    r.plays[0].done({true, "playing", "com.tencent.qqmusic#1", ""});
    REQUIRE(r.seeks.size() == 1);
    CHECK(std::get<2>(r.seeks[0]) == 30'020);

    Rig r2;
    r2.apps[Device::Phone] = {App::QqMusic};
    auto& early = r2.add(Device::Pc, session("Spotify.exe", "Spotify.exe", "起风了"));
    early.positionMs = 1'000;
    r2.c->start(Device::Pc, "Spotify.exe", std::nullopt, [](const Outcome&) {});
    r2.plays[0].done({true, "playing", "com.tencent.qqmusic#1", ""});
    CHECK(r2.seeks.empty());
    CHECK(r2.pauses.size() == 1);
}

TEST_CASE("handoff coordinator: a paused source stays paused and is not paused again") {
    Rig r;
    r.apps[Device::Pc] = {App::Netease};
    auto& src = r.add(Device::Phone, session("p#1", "com.netease.cloudmusic", "起风了", PlaybackStatus::Paused));
    src.positionMs = 90'000;
    std::optional<Outcome> out;
    r.c->start(Device::Phone, "p#1", std::nullopt, [&](const Outcome& o) { out = o; });
    r.s.advance(5000);
    r.plays[0].done({true, "playing", "n", ""});
    CHECK(r.pauses.empty());
    REQUIRE(r.seeks.size() == 1);
    CHECK(std::get<2>(r.seeks[0]) == 90'000);
}

TEST_CASE("handoff coordinator: opened, failures, busy, timeout, link loss") {
    Rig r;
    r.apps[Device::Pc] = {App::Spotify};
    r.add(Device::Phone, session("p#1", "com.netease.cloudmusic", "起风了"));
    std::vector<Outcome> outs;
    auto collect = [&](const Outcome& o) { outs.push_back(o); };

    // Only a search could be opened: the source keeps playing.
    r.c->start(Device::Phone, "p#1", std::nullopt, collect);
    r.c->start(Device::Phone, "p#1", std::nullopt, collect); // one at a time
    REQUIRE(outs.size() == 1);
    CHECK(outs[0].error == error::Busy);
    r.plays[0].done({true, "opened", "", ""});
    REQUIRE(outs.size() == 2);
    CHECK(outs[1].ok);
    CHECK(outs[1].state == "opened");
    CHECK(r.pauses.empty());
    CHECK(r.seeks.empty());

    // Not in that app's catalogue.
    r.c->start(Device::Phone, "p#1", std::nullopt, collect);
    r.plays[1].done({false, "", "", error::NotFound});
    CHECK(outs.back().error == error::NotFound);

    // A late answer after the timeout is ignored.
    r.c->start(Device::Phone, "p#1", std::nullopt, collect);
    r.s.advance(Coordinator::kTimeoutMs);
    CHECK(outs.back().error == error::Timeout);
    size_t n = outs.size();
    r.plays[2].done({true, "playing", "x", ""});
    CHECK(outs.size() == n);
    CHECK(r.pauses.empty());

    r.c->start(Device::Phone, "p#1", std::nullopt, collect);
    r.c->onWaitingForTap();
    CHECK(r.c->status().waitingForTap);
    r.c->onLinkLost();
    CHECK(outs.back().error == error::LinkLost);
    CHECK_FALSE(r.c->status().active);

    // Nothing to hand off / nowhere to go.
    r.c->start(Device::Phone, "gone", std::nullopt, collect);
    CHECK(outs.back().error == error::NoSession);
    r.add(Device::Pc, session("pc", "Spotify.exe", "起风了"));
    r.c->start(Device::Pc, "pc", std::nullopt, collect);
    CHECK(outs.back().error == error::AppUnavailable);
    r.c->start(Device::Phone, "p#1", App::QqMusic, collect); // asked for an app the PC lacks
    CHECK(outs.back().error == error::AppUnavailable);
    CHECK(outs.back().app == App::QqMusic);
}
