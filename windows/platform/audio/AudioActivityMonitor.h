#pragma once

#include "md/util/Scheduler.h"

#include <windows.h>

#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace md::platform {

struct AudioActivityConfig {
    double sensitivity = 0.02;     // peak level (0..1) that counts as audible
    int64_t activationMs = 1500;   // continuous audibility before "active"
    int64_t releaseMs = 2500;      // silence before "inactive"
    std::vector<std::string> blacklist; // lower-case process names
    std::vector<std::string> whitelist; // if non-empty, only these count
    std::vector<DWORD> ignorePids;      // MediaDuet's own processes
};

// Detects which Windows processes are actually producing sound, including apps
// without a media session (games, video players, calls). Uses Core Audio session
// notifications; peak meters are sampled (250 ms) only while a session is active.
// No PCM is captured.
class AudioActivityMonitor {
public:
    using Fn = std::function<void(bool active, bool communication, std::vector<std::string> apps)>;
    AudioActivityMonitor(IScheduler& sched, Fn onChange);
    ~AudioActivityMonitor();

    bool start();
    void setConfig(AudioActivityConfig cfg);
    int activeSessions() const;

    struct AppVolume {
        bool available = false;
        float level = 1.f;
        bool muted = false;
    };
    // Per-app (Volume Mixer) volume of all render sessions of a process (lower-case exe name).
    AppVolume appVolume(const std::string& process) const;
    bool setAppVolume(const std::string& process, std::optional<float> level, std::optional<bool> muted);
    // Called (scheduler thread) when an app's volume/mute changes, with its process name.
    void setOnAppVolumeChanged(std::function<void(const std::string& process)> fn);
    bool active() const { return active_; }
    // When a stream last started on this PC (scheduler ms, -1 = never): any app's render stream,
    // system / notification sounds and microphone (capture) streams alike, regardless of the
    // blacklist and of how short or quiet it is. Headsets react to such starts (switching to the
    // PC, to the call profile) with play / pause commands of their own; see HostApp.
    int64_t lastStreamStartMs() const;

    // Internal: called from COM callbacks via the scheduler.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    bool active_ = false;
};

} // namespace md::platform
