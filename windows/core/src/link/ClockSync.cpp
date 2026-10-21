#include "md/link/ClockSync.h"

#include <algorithm>
#include <cmath>

namespace md::link {

namespace {

double errorBound(int64_t rtt, int64_t age) {
    return static_cast<double>(rtt) / 2.0 + static_cast<double>(std::max<int64_t>(age, 0)) * ClockSync::kDriftPerMs;
}

} // namespace

void ClockSync::addSample(int64_t t0, int64_t peerMs, int64_t t1) {
    int64_t rtt = t1 - t0;
    if (rtt < 0 || rtt > 120'000) return;
    // Rounded midpoint; (t0 + t1) cannot overflow for monotonic ms.
    int64_t offset = peerMs - (t0 + rtt / 2);
    if (const Sample* b = best(t1)) {
        // Both samples are within their RTT/2 of the truth; anything further apart means one of the
        // clocks restarted (a reboot behind a resumed session): start over.
        double slack = static_cast<double>(rtt + b->rtt) / 2.0 + 1000.0;
        if (std::abs(static_cast<double>(offset - b->offset)) > slack) samples_.clear();
    }
    samples_.push_back({t1, rtt, offset});
    if (samples_.size() > kMaxSamples) samples_.erase(samples_.begin());
}

const ClockSync::Sample* ClockSync::best(int64_t nowMs) const {
    const Sample* r = nullptr;
    double rb = 0;
    for (auto& s : samples_) {
        int64_t age = nowMs - s.at;
        if (age > kMaxAgeMs) continue;
        double b = errorBound(s.rtt, age);
        if (!r || b < rb) {
            r = &s;
            rb = b;
        }
    }
    return r;
}

std::optional<int64_t> ClockSync::offsetMs(int64_t nowMs) const {
    if (const Sample* b = best(nowMs)) return b->offset;
    return std::nullopt;
}

int64_t ClockSync::uncertaintyMs(int64_t nowMs) const {
    if (const Sample* b = best(nowMs)) return static_cast<int64_t>(std::ceil(errorBound(b->rtt, nowMs - b->at)));
    return -1;
}

int64_t ClockSync::anchor(std::optional<int64_t> peerAt, int64_t nowMs, int64_t fallbackLatencyMs) const {
    if (peerAt) {
        if (auto off = offsetMs(nowMs)) {
            int64_t local = *peerAt - *off;
            if (local >= nowMs - kMaxTransitMs) return std::min(local, nowMs);
        }
    }
    return nowMs - std::max<int64_t>(fallbackLatencyMs, 0);
}

} // namespace md::link
