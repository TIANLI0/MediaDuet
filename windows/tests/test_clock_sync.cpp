#include "md/link/ClockSync.h"

#include <doctest/doctest.h>

using md::link::ClockSync;

// The peer's clock runs 50'000 ms ahead of ours in these tests.
static constexpr int64_t kOff = 50'000;

// A ping sent at t0 that takes `up` ms to the peer and `down` ms back.
static void ping(ClockSync& c, int64_t t0, int64_t up, int64_t down) { c.addSample(t0, t0 + up + kOff, t0 + up + down); }

TEST_CASE("clock sync: no samples, no estimate; anchor falls back to now - latency") {
    ClockSync c;
    CHECK_FALSE(c.offsetMs(0).has_value());
    CHECK(c.uncertaintyMs(0) == -1);
    CHECK(c.anchor(12'345, 1000, 40) == 960);
    CHECK(c.anchor(std::nullopt, 1000, 40) == 960);
}

TEST_CASE("clock sync: a symmetric round trip gives the exact offset") {
    ClockSync c;
    ping(c, 1000, 20, 20);
    CHECK(c.offsetMs(1040) == kOff);
    CHECK(c.uncertaintyMs(1040) == 20);
    // A snapshot taken on the peer at its 2000 + kOff is our 2000, whatever its own transit time.
    CHECK(c.anchor(2000 + kOff, 2600, 0) == 2000);
}

TEST_CASE("clock sync: the lowest-RTT sample wins over slow, asymmetric ones") {
    ClockSync c;
    ping(c, 0, 300, 10);     // Wi-Fi power save on the way there: offset off by 145 ms
    ping(c, 1000, 10, 250);  // queued on the way back: off by -120 ms
    ping(c, 2000, 4, 4);     // quiet moment
    ping(c, 3000, 120, 120); // slow but symmetric
    CHECK(c.offsetMs(4000) == kOff);
    CHECK(c.uncertaintyMs(4000) <= 5);
}

TEST_CASE("clock sync: old samples age out by drift, then expire") {
    ClockSync c;
    ping(c, 0, 2, 2);            // excellent but old
    ping(c, 400'000, 30, 30);    // newer, worse RTT
    // At 400 s the old one's bound is 2 + 40 = 42 ms, the new one's 30: the new one wins.
    CHECK(c.uncertaintyMs(400'060) == 30);
    ClockSync d;
    ping(d, 0, 5, 5);
    CHECK(d.offsetMs(ClockSync::kMaxAgeMs).has_value());
    CHECK_FALSE(d.offsetMs(ClockSync::kMaxAgeMs + 20).has_value());
}

TEST_CASE("clock sync: a clock that restarted discards the old samples") {
    ClockSync c;
    ping(c, 0, 5, 5);
    // The peer rebooted: its clock is now 3 s behind ours instead of 50 s ahead.
    c.addSample(10'000, 10'005 - 3000, 10'010);
    CHECK(c.offsetMs(10'010) == -3000);
}

TEST_CASE("clock sync: implausible anchors fall back") {
    ClockSync c;
    ping(c, 0, 5, 5);
    // Further back than kMaxTransitMs: not trusted.
    CHECK(c.anchor(1000 + kOff, 1000 + ClockSync::kMaxTransitMs + 1, 30) == 1000 + ClockSync::kMaxTransitMs + 1 - 30);
    // Slightly in the future (estimate error): clamped to now.
    CHECK(c.anchor(5000 + kOff, 4990, 30) == 4990);
}

TEST_CASE("clock sync: bad samples are ignored; the window is bounded") {
    ClockSync c;
    c.addSample(100, 5, 50); // pong before ping
    CHECK_FALSE(c.offsetMs(100).has_value());
    for (int i = 0; i < 40; ++i) ping(c, i * 1000, 10 + i, 10 + i);
    CHECK(c.uncertaintyMs(40'000) >= 10 + 40 - int(ClockSync::kMaxSamples)); // the oldest (fastest) dropped
}
