#pragma once

#include "md/util/Scheduler.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace md::platform {

// Master volume of the default render endpoint (what the user hears).
// Event driven (IAudioEndpointVolumeCallback + default-device changes).
class VolumeControl {
public:
    struct State {
        bool available = false;
        float level = 0.f; // 0..1 scalar
        bool muted = false;
        int steps = 0;
        std::wstring deviceId;   // default render endpoint
        std::string deviceName;  // its friendly name
        bool bluetooth = false;  // a Bluetooth endpoint (A2DP / HFP / LE Audio): adds output latency
    };
    struct Output {
        std::wstring id;
        std::string name;
        bool isDefault = false;
    };
    std::vector<Output> outputs() const;
    VolumeControl(IScheduler& sched, std::function<void(const State&)> onChange);
    ~VolumeControl();

    bool start();
    State state() const;
    void setLevel(float level);
    void setMuted(bool muted);
    void step(int delta);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace md::platform
