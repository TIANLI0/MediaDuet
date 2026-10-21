#pragma once

// Peer clock estimate for position sync (spec §11.1). Each pong carries the
// peer's monotonic clock at the moment it answered (`rt`); with our send and
// receive times that is one NTP-style sample: offset = rt − (t0 + t1) / 2,
// wrong by at most half the round trip. Samples are kept for a while and the
// one with the smallest error bound (half its RTT plus worst-case clock drift
// since it was taken) wins, so a single slow ping (Wi-Fi power save, Bluetooth
// sniff mode, a queue behind an artwork transfer) never moves the estimate.
//
// Snapshots carry the sender's clock at which their positions hold (`at`);
// converting it to our clock makes the anchor independent of how long the
// snapshot itself took to arrive.

#include <cstdint>
#include <optional>
#include <vector>

namespace md::link {

class ClockSync {
public:
    static constexpr size_t kMaxSamples = 16;
    static constexpr int64_t kMaxAgeMs = 10 * 60'000;
    // Worst-case drift between two device clocks (100 ppm), added to a sample's error bound as it ages.
    static constexpr double kDriftPerMs = 100e-6;
    // A converted `at` further in the past than this is not trusted (clock discontinuity); fallback used.
    static constexpr int64_t kMaxTransitMs = 10'000;

    // t0: our clock when the ping left, peerMs: the peer's clock when it answered, t1: our clock at the pong.
    void addSample(int64_t t0, int64_t peerMs, int64_t t1);
    void reset() { samples_.clear(); }

    // Peer clock − our clock, from the best fresh sample (nullopt without one).
    std::optional<int64_t> offsetMs(int64_t nowMs) const;
    // Error bound of offsetMs (ms), -1 without a sample.
    int64_t uncertaintyMs(int64_t nowMs) const;

    // Our clock at which a snapshot's positions hold. With the sender's `at` and a clock estimate it is
    // `at` converted to our clock (never later than nowMs); otherwise the legacy estimate
    // nowMs − fallbackLatencyMs (half the RTT).
    int64_t anchor(std::optional<int64_t> peerAt, int64_t nowMs, int64_t fallbackLatencyMs) const;

private:
    struct Sample {
        int64_t at;     // our clock when taken
        int64_t rtt;
        int64_t offset; // peer − local
    };
    const Sample* best(int64_t nowMs) const;
    std::vector<Sample> samples_; // oldest first
};

} // namespace md::link
