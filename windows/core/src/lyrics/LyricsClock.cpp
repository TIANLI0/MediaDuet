#include "md/lyrics/LyricsClock.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace md::lyrics {

double steadyNowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / freq;
}

void LyricsClock::reset() { *this = LyricsClock(); }

std::optional<double> LyricsClock::position(double now) const {
    if (!valid_) return std::nullopt;
    if (!playing_) return basePos_;
    double el = std::max(0.0, now - baseAt_);
    return basePos_ + el * rate_ + slew_ * std::min(el, slewMs_);
}

double LyricsClock::pendingCorrection(double now) const {
    if (!valid_ || !playing_) return 0;
    double el = std::max(0.0, now - baseAt_);
    return slew_ * std::max(0.0, slewMs_ - el);
}

bool LyricsClock::update(const Report& r, double now, bool discontinuity) {
    if (!r.positionMs) {
        bool was = valid_;
        valid_ = false;
        playing_ = r.playing;
        rate_ = r.rate;
        trackId_ = r.trackId;
        return was;
    }
    double rate = std::isfinite(r.rate) && r.rate > 0 ? r.rate : 1.0;
    double reported = static_cast<double>(*r.positionMs);
    if (r.playing) reported += std::max(0.0, now - r.atMs) * rate;
    bool snap = discontinuity || !valid_ || r.playing != playing_ || std::abs(rate - rate_) > 1e-3 ||
                r.trackId != trackId_;
    double predicted = snap ? reported : *position(now);
    double err = reported - predicted;
    if (std::abs(err) > kSnapMs) snap = true;
    playing_ = r.playing;
    rate_ = rate;
    trackId_ = r.trackId;
    valid_ = true;
    baseAt_ = now;
    if (snap || !playing_) {
        basePos_ = reported;
        slew_ = slewMs_ = 0;
        return true;
    }
    // Slew: absorb err within ~kSlewWindowMs, never faster than ±kMaxSlew of the
    // rate (so the shown position keeps moving forwards), longer for larger errors.
    basePos_ = predicted;
    double maxSlew = kMaxSlew * rate;
    slew_ = std::clamp(err / kSlewWindowMs, -maxSlew, maxSlew);
    slewMs_ = slew_ != 0 ? err / slew_ : 0;
    return false;
}

int64_t manualOffsetMs(const nlohmann::json& offsets, const std::string& appId) {
    if (!offsets.is_object() || appId.empty()) return 0;
    auto it = offsets.find(appId);
    if (it == offsets.end() || !it->is_number()) return 0;
    double v = it->get<double>();
    if (!std::isfinite(v)) return 0;
    return std::clamp<int64_t>(static_cast<int64_t>(std::llround(v)), -kMaxManualOffsetMs, kMaxManualOffsetMs);
}

int64_t lyricsDelayMs(bool autoLatency, int64_t outputLatencyMs, const nlohmann::json& offsets,
                      const std::string& appId) {
    int64_t lat = autoLatency ? std::clamp<int64_t>(outputLatencyMs, 0, kMaxOutputLatencyMs) : 0;
    return lat + manualOffsetMs(offsets, appId);
}

} // namespace md::lyrics
