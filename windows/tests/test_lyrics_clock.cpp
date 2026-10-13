#include "md/lyrics/LyricsClock.h"

#include <doctest/doctest.h>

using namespace md::lyrics;

static LyricsClock::Report rep(int64_t pos, double at, bool playing = true, std::string track = "t") {
    LyricsClock::Report r;
    r.positionMs = pos;
    r.atMs = at;
    r.playing = playing;
    r.trackId = std::move(track);
    return r;
}

TEST_CASE("lyrics clock: first report snaps and extrapolates") {
    LyricsClock c;
    CHECK_FALSE(c.position(0).has_value());
    CHECK(c.update(rep(10000, 0), 0));
    CHECK(*c.position(0) == doctest::Approx(10000));
    CHECK(*c.position(2500) == doctest::Approx(12500));
}

TEST_CASE("lyrics clock: small errors are slewed, never backwards") {
    LyricsClock c;
    c.update(rep(10000, 0), 0);
    // At t=1000 a report says 40 ms less than predicted (jitter).
    CHECK_FALSE(c.update(rep(10960, 1000), 1000));
    CHECK(*c.position(1000) == doctest::Approx(11000)); // no jump
    // Slews at -4 %: absorbed within 1 s, and it still moves forward.
    double prev = *c.position(1000);
    for (int t = 1016; t <= 2200; t += 16) {
        double p = *c.position(t);
        CHECK(p > prev);
        prev = p;
    }
    CHECK(*c.position(2000) == doctest::Approx(11960));
    CHECK(*c.position(3000) == doctest::Approx(12960));
}

TEST_CASE("lyrics clock: slewing is capped at 5 % and finishes later for bigger errors") {
    LyricsClock c;
    c.update(rep(0, 0), 0);
    c.update(rep(1200, 1000), 1000); // 200 ms ahead
    CHECK(*c.position(2000) == doctest::Approx(2050)); // +5 % in the first second
    CHECK(c.pendingCorrection(2000) == doctest::Approx(150));
    CHECK(*c.position(5000) == doctest::Approx(5200)); // done after 4 s
    CHECK(c.pendingCorrection(5000) == doctest::Approx(0));
}

TEST_CASE("lyrics clock: snaps on seek, pause, rate and track changes") {
    LyricsClock c;
    c.update(rep(0, 0), 0);
    CHECK(c.update(rep(60000, 1000), 1000)); // seek (> 300 ms)
    CHECK(*c.position(1000) == doctest::Approx(60000));
    CHECK(c.update(rep(60500, 1500, false), 1500)); // pause
    CHECK(*c.position(9000) == doctest::Approx(60500));
    CHECK(c.update(rep(60500, 9000, true), 9000)); // play
    auto r = rep(61000, 9500);
    r.rate = 2.0;
    CHECK(c.update(r, 9500)); // rate
    CHECK(*c.position(10500) == doctest::Approx(63000));
    CHECK(c.update(rep(100, 11000, true, "other"), 11000)); // track
    CHECK(*c.position(11000) == doctest::Approx(100));
    CHECK(c.update(rep(150, 11010), 11010, true)); // explicit discontinuity
    CHECK(*c.position(11010) == doctest::Approx(150));
}

TEST_CASE("lyrics clock: stale report anchors are extrapolated before comparing") {
    LyricsClock c;
    c.update(rep(5000, 0), 0);
    // Report produced at t=800 (position 5800) but delivered at t=1000.
    CHECK_FALSE(c.update(rep(5800, 800), 1000));
    CHECK(*c.position(1000) == doctest::Approx(6000));
}

TEST_CASE("lyrics delay: output latency + per-app offset") {
    nlohmann::json offs = {{"cloudmusic.exe", 300}, {"com.netease.cloudmusic", -200}, {"bad", "x"}, {"huge", 99999}};
    CHECK(manualOffsetMs(offs, "cloudmusic.exe") == 300);
    CHECK(manualOffsetMs(offs, "bad") == 0);
    CHECK(manualOffsetMs(offs, "huge") == 5000);
    CHECK(manualOffsetMs(offs, "") == 0);
    CHECK(lyricsDelayMs(true, 180, offs, "cloudmusic.exe") == 480);
    CHECK(lyricsDelayMs(false, 180, offs, "cloudmusic.exe") == 300);
    CHECK(lyricsDelayMs(true, 0, offs, "com.netease.cloudmusic") == -200);
    CHECK(lyricsDelayMs(true, 5000, nlohmann::json::object(), "x") == 2000);
}
