#include "md/media/MediaTypes.h"

#include "TestStrings.h"

#include <doctest/doctest.h>

using namespace md::media;

static MediaSessionInfo sample() {
    MediaSessionInfo s;
    s.key = "com.netease.cloudmusic#1";
    s.appId = "com.netease.cloudmusic";
    s.appName = "网易云音乐";
    s.title = "晴天";
    s.artist = "周杰伦";
    s.album = "叶惠美";
    s.durationMs = 269000;
    s.positionMs = 10000;
    s.positionAtMs = 1000;
    s.status = PlaybackStatus::Playing;
    s.actions = action::Play | action::Pause | action::Next | action::Seek;
    s.updateTrackId();
    return s;
}

TEST_CASE("position extrapolation") {
    auto s = sample();
    CHECK(*s.positionAt(1000) == 10000);
    CHECK(*s.positionAt(3500) == 12500);
    s.rate = 2.0;
    CHECK(*s.positionAt(2000) == 12000);
    s.status = PlaybackStatus::Paused;
    CHECK(*s.positionAt(99999) == 10000);
    s.status = PlaybackStatus::Playing;
    s.rate = 1.0;
    CHECK(*s.positionAt(1000 + 10'000'000) == 269000); // clamped to duration
}

TEST_CASE("wire round trip with latency compensation") {
    auto s = sample();
    MediaSnapshot snap;
    snap.gen = 3;
    snap.active = s.key;
    snap.sessions.push_back(s);
    auto j = toWire(snap, 2000); // position 11000 at send time
    auto back = snapshotFromWire(j, 50'000, 40);
    REQUIRE(back);
    REQUIRE(back->sessions.size() == 1);
    auto& b = back->sessions[0];
    CHECK(b.title == "晴天");
    CHECK(*b.positionMs == 11040);
    CHECK(b.positionAtMs == 50'000);
    CHECK(b.supports(CommandAction::Seek));
    CHECK_FALSE(b.supports(CommandAction::Previous));
    CHECK(b.trackId == s.trackId);
    CHECK(back->activeSession() != nullptr);
}

TEST_CASE("snapshot validation rejects bad input") {
    CHECK_FALSE(snapshotFromWire(json::array(), 0, 0));
    json j = {{"gen", 1}, {"sessions", json::array({{{"key", ""}, {"status", "playing"}}})}};
    CHECK_FALSE(snapshotFromWire(j, 0, 0));
    j = {{"gen", 1}, {"sessions", json::array({{{"key", "k"}, {"status", "playing"}, {"title", std::string(600, 'a')}}})}};
    CHECK_FALSE(snapshotFromWire(j, 0, 0));
    json many = {{"gen", 1}, {"sessions", json::array()}};
    for (int i = 0; i < 17; ++i) many["sessions"].push_back({{"key", "k" + std::to_string(i)}, {"status", "paused"}});
    CHECK_FALSE(snapshotFromWire(many, 0, 0));
    json art = {{"gen", 1},
                {"sessions", json::array({{{"key", "k"}, {"status", "paused"}, {"artwork", {{"hash", "zz"}}}}})}};
    CHECK_FALSE(snapshotFromWire(art, 0, 0));
}

TEST_CASE("equivalence ignores natural progress but detects seeks") {
    auto a = sample();
    auto b = a;
    b.positionMs = 15000;
    b.positionAtMs = 6000; // consistent with playback
    CHECK(a.equivalent(b));
    b.positionMs = 60000; // a seek
    CHECK_FALSE(a.equivalent(b));
    b = a;
    b.status = PlaybackStatus::Paused;
    CHECK_FALSE(a.equivalent(b));
}

TEST_CASE("commands parse and validate") {
    MediaCommand c;
    c.action = CommandAction::Seek;
    c.positionMs = 60000;
    c.origin = Origin::Policy;
    auto back = commandFromWire(toWire(c));
    REQUIRE(back);
    CHECK(back->action == CommandAction::Seek);
    CHECK(*back->positionMs == 60000);
    CHECK(back->origin == Origin::Policy);
    CHECK_FALSE(commandFromWire({{"action", "seek"}}));
    CHECK_FALSE(commandFromWire({{"action", "explode"}}));
}

TEST_CASE("outputLatencyMs is optional, round-trips and is range checked") {
    auto s = sample();
    auto j = toWire(s, 1000);
    CHECK_FALSE(j.contains("outputLatencyMs")); // omitted when unknown
    auto back = sessionFromWire(j, 0, 0);
    REQUIRE(back);
    CHECK_FALSE(back->outputLatencyMs.has_value());

    s.outputLatencyMs = 180;
    j = toWire(s, 1000);
    CHECK(j["outputLatencyMs"] == 180);
    back = sessionFromWire(j, 0, 0);
    REQUIRE(back);
    CHECK(back->outputLatencyMs == 180);

    for (auto bad : {json(-1), json(2001), json("180"), json(nullptr)}) {
        j["outputLatencyMs"] = bad;
        back = sessionFromWire(j, 0, 0);
        REQUIRE(back); // the session itself stays valid
        CHECK_FALSE(back->outputLatencyMs.has_value());
    }
    j["outputLatencyMs"] = 2000;
    CHECK(sessionFromWire(j, 0, 0)->outputLatencyMs == 2000);

    // A latency change is a meaningful change (the snapshot is re-sent).
    auto a = sample(), b = sample();
    b.outputLatencyMs = 180;
    CHECK_FALSE(a.equivalent(b));
    a.outputLatencyMs = 0;
    b.outputLatencyMs.reset();
    CHECK(a.equivalent(b)); // 0 == unknown
}

TEST_CASE("custom actions, queue hint and the new commands survive the wire") {
    md::media::MediaSessionInfo s;
    s.key = "com.netease.cloudmusic#1";
    s.title = "t";
    s.customActions.push_back({"like", "Like", md::media::ArtworkRef{std::string(64, 'a'), "image/png", 812}});
    s.customActions.push_back({"mode", "Shuffle", std::nullopt});
    s.queue = md::media::QueueHint{37, "5f0c2a91", 12};
    auto j = md::media::toWire(s, 0);
    auto back = md::media::sessionFromWire(j, 0, 0);
    REQUIRE(back);
    CHECK(back->customActions == s.customActions);
    CHECK(back->queue == s.queue);
    CHECK(back->actions == 0); // custom / playQueueItem are never in the action set
    CHECK_FALSE(back->equivalent([&] { auto o = *back; o.queue->activeId = 13; return o; }()));

    // Lenient: malformed entries are dropped, the session still parses.
    j["customActions"] = {{{"id", ""}}, {{"title", "no id"}}, {{"id", "ok"}, {"icon", {{"hash", "zz"}}}}, 5};
    j["queue"] = {{"count", 0}};
    back = md::media::sessionFromWire(j, 0, 0);
    REQUIRE(back);
    REQUIRE(back->customActions.size() == 1);
    CHECK(back->customActions[0].id == "ok");
    CHECK_FALSE(back->customActions[0].icon);
    CHECK_FALSE(back->queue);

    md::media::MediaCommand c;
    c.action = md::media::CommandAction::Custom;
    c.customAction = "like";
    auto cj = md::media::toWire(c);
    CHECK(cj["action"] == "custom");
    auto cb = md::media::commandFromWire(cj);
    REQUIRE(cb);
    CHECK(cb->customAction == "like");
    CHECK_FALSE(md::media::commandFromWire(md::media::json{{"action", "custom"}}));
    CHECK_FALSE(md::media::commandFromWire(md::media::json{{"action", "playQueueItem"}}));
    auto q = md::media::commandFromWire(md::media::json{{"action", "playQueueItem"}, {"queueId", 7}});
    REQUIRE(q);
    CHECK(*q->queueId == 7);
}

TEST_CASE("media.queue parses a window and skips bad entries") {
    md::media::json j = {{"key", "k"}, {"rev", "r1"}, {"title", "Liked"}, {"activeId", 3}, {"total", 500}, {"offset", 200},
                         {"items", {{{"id", 3}, {"title", "a"}, {"subtitle", "b"}}, {{"title", "no id"}}, {{"id", 4}}}}};
    auto q = md::media::queueFromWire(j);
    REQUIRE(q);
    CHECK(q->items.size() == 2);
    CHECK(q->total == 500);
    CHECK(q->offset == 200);
    CHECK(*q->activeId == 3);
    auto back = md::media::queueFromWire(md::media::toJson(*q));
    REQUIRE(back);
    CHECK(back->items.size() == 2);
    CHECK_FALSE(md::media::queueFromWire(md::media::json{{"items", md::media::json::array()}}));
}
