#pragma once

#include "md/util/Scheduler.h"

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Radios.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace md::platform {

struct BtCandidate {
    std::string deviceId; // WinRT device id
    std::string name;
    uint64_t address = 0;
    bool hasService = false; // MediaDuet RFCOMM record present (app installed & listening)
    bool isPhone = false;
};

// Event-driven Bluetooth state: adapter radio on/off, ACL presence of paired
// phones, and on-demand discovery of paired phones offering MediaDuet.
class BluetoothWatcher {
public:
    struct Callbacks {
        std::function<void(bool adapterPresent, bool radioOn)> onRadio;
        std::function<void(uint64_t address, bool connected)> onPresence;
        std::function<void(std::vector<BtCandidate>)> onCandidates;
    };
    BluetoothWatcher(IScheduler& sched, Callbacks cb);
    ~BluetoothWatcher();

    void start();
    void watchDevices(const std::vector<uint64_t>& addresses);
    void discover();
    // Finds a bonded device with this Bluetooth name that offers the MediaDuet
    // RFCOMM service (LAN-paired phones cannot tell us their MAC). 0 = not found.
    void resolveByName(const std::string& name, std::function<void(uint64_t address)> done);
    bool adapterPresent() const { return adapter_; }
    bool radioOn() const { return radioOn_; }

private:
    IScheduler& sched_;
    Callbacks cb_;
    winrt::Windows::Devices::Radios::Radio radio_{nullptr};
    winrt::event_token radioToken_{};
    struct Watched {
        winrt::Windows::Devices::Bluetooth::BluetoothDevice dev{nullptr};
        winrt::event_token token{};
    };
    std::map<uint64_t, Watched> watched_;
    bool adapter_ = false;
    bool radioOn_ = false;
    bool discovering_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace md::platform
