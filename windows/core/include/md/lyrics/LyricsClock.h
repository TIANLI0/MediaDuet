#pragma once

// Smoothed playback clock for lyrics (shared contract with the WinUI app and
// Android, docs/ipc.md "Lyrics timing").
//
// Position reports (GSMTC updates, phone snapshots) carry jitter of tens of ms.
// Re-anchoring on every report makes a karaoke fill jump back and forth, so the
// clock extrapolates locally and only *slews* towards new reports: the local rate
// changes by at most ±5 % until the error is gone (≈ 1 s for errors ≤ 50 ms), so
// the position shown never moves backwards while playing. It snaps instead when
// the error exceeds 300 ms (a seek), on play/pause, rate or track changes, and on
// explicit discontinuities (a seek command).

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace md::lyrics {

// Monotonic high-resolution milliseconds (QueryPerformanceCounter).
double steadyNowMs();

class LyricsClock {
public:
    static constexpr double kSnapMs = 300.0;
    static constexpr double kMaxSlew = 0.05;     // fraction of the playback rate
    static constexpr double kSlewWindowMs = 1000; // target time to absorb an error

    struct Report {
        std::optional<int64_t> positionMs; // valid at atMs
        double atMs = 0;                   // steadyNowMs() time base
        double rate = 1.0;
        bool playing = false;
        std::string trackId;
    };

    // Feeds a position report. Returns true if the clock snapped (re-anchored).
    bool update(const Report& r, double nowMs, bool discontinuity = false);
    void reset();

    bool valid() const { return valid_; }
    bool playing() const { return playing_; }
    double rate() const { return rate_; }
    const std::string& trackId() const { return trackId_; }
    // Smoothed position at nowMs (nullopt when unknown).
    std::optional<double> position(double nowMs) const;
    // Remaining correction being slewed in (ms, + = clock is behind); for tests/diagnostics.
    double pendingCorrection(double nowMs) const;

private:
    bool valid_ = false;
    bool playing_ = false;
    double rate_ = 1.0;
    std::string trackId_;
    double basePos_ = 0; // position at baseAt_
    double baseAt_ = 0;
    double slew_ = 0;    // extra ms of position per ms of time while correcting
    double slewMs_ = 0;  // how long the correction lasts from baseAt_
};

// Total delay the lyrics are shifted by (display position = smoothed position −
// delay): output latency (only if autoLatency) + the per-app manual offset.
// offsets = settings "lyrics.offsets" ({appId: ms}, + = lyrics later).
int64_t manualOffsetMs(const nlohmann::json& offsets, const std::string& appId);
int64_t lyricsDelayMs(bool autoLatency, int64_t outputLatencyMs, const nlohmann::json& offsets,
                      const std::string& appId);

inline constexpr int64_t kMaxManualOffsetMs = 5000;
inline constexpr int64_t kMaxOutputLatencyMs = 2000;

} // namespace md::lyrics
