#pragma once

#include "md/util/Scheduler.h"

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Media.Audio.h>

#include <functional>
#include <map>
#include <string>

namespace md::platform {

// Windows as a Bluetooth A2DP *sink* for the phone via AudioPlaybackConnection
// (Windows 10 2004+). The phone's audio then plays on this PC's current output.
// RFCOMM control and A2DP audio are separate Bluetooth profiles.
class A2dpSink {
public:
    struct Status {
        bool available = false; // at least one eligible (paired A2DP source) device
        std::string state = "off"; // off | connecting | on | error
        std::string device;
        std::string error;
    };
    A2dpSink(IScheduler& sched, std::function<void()> changed);
    ~A2dpSink();

    void start();
    // Opens the connection to the eligible device whose name matches `deviceName`
    // (falls back to the only eligible device).
    void enable(const std::string& deviceName);
    void disable();
    const Status& status() const { return st_; }
    bool active() const { return st_.state == "on" || st_.state == "connecting"; }

private:
    void setState(std::string state, std::string error = {});

    IScheduler& sched_;
    std::function<void()> changed_;
    winrt::Windows::Devices::Enumeration::DeviceWatcher watcher_{nullptr};
    std::map<std::wstring, std::string> devices_; // id -> name
    winrt::Windows::Media::Audio::AudioPlaybackConnection conn_{nullptr};
    winrt::event_token connTok_{};
    Status st_;
    uint64_t attempt_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace md::platform
