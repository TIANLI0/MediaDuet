#pragma once

#include <cstdint>

namespace md::app {

// Process resource sampling for the diagnostics view (measured, not estimated).
class Diagnostics {
public:
    struct Sample {
        double cpuPercent = 0;   // average since the previous sample, all cores = 100%
        double workingSetMB = 0;
        double privateMB = 0;
        int threads = 0;
    };
    Sample sample();

private:
    uint64_t lastCpu_ = 0;
    uint64_t lastWall_ = 0;
};

} // namespace md::app
