#include "BluetoothWatcher.h"

#include "RfcommTransport.h"

#include "md/util/Log.h"
#include "md/util/Text.h"

#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

namespace md::platform {

using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Rfcomm;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Devices::Radios;

BluetoothWatcher::BluetoothWatcher(IScheduler& sched, Callbacks cb) : sched_(sched), cb_(std::move(cb)) {}

BluetoothWatcher::~BluetoothWatcher() {
    *alive_ = false;
    try {
        if (radio_) radio_.StateChanged(radioToken_);
        for (auto& [a, w] : watched_)
            if (w.dev) w.dev.ConnectionStatusChanged(w.token);
    } catch (...) {
    }
}

void BluetoothWatcher::start() {
    std::weak_ptr<bool> alive = alive_;
    [](BluetoothWatcher* self, std::weak_ptr<bool> alive) -> fire_and_forget {
        co_await resume_background();
        bool present = false, on = false;
        Radio radio{nullptr};
        try {
            auto adapter = co_await BluetoothAdapter::GetDefaultAsync();
            present = adapter && adapter.IsClassicSupported();
            if (adapter) {
                radio = co_await adapter.GetRadioAsync();
                if (radio) on = radio.State() == RadioState::On;
            }
        } catch (hresult_error const& e) {
            log::warn("bt", "Bluetooth adapter query failed: {}", text::toUtf8(e.message()));
        }
        self->sched_.post([self, alive, present, on, radio] {
            if (alive.expired()) return;
            self->adapter_ = present;
            self->radioOn_ = present && on;
            log::info("bt", "adapter {} radio {}", present ? "present" : "absent", on ? "on" : "off");
            if (radio) {
                self->radio_ = radio;
                self->radioToken_ = radio.StateChanged([self, alive](Radio const& r, auto&&) {
                    bool nowOn = false;
                    try {
                        nowOn = r.State() == RadioState::On;
                    } catch (...) {
                    }
                    self->sched_.post([self, alive, nowOn] {
                        if (alive.expired() || self->radioOn_ == nowOn) return;
                        self->radioOn_ = nowOn;
                        log::info("bt", "radio turned {}", nowOn ? "on" : "off");
                        if (self->cb_.onRadio) self->cb_.onRadio(self->adapter_, nowOn);
                    });
                });
            }
            if (self->cb_.onRadio) self->cb_.onRadio(self->adapter_, self->radioOn_);
        });
    }(this, alive);
}

void BluetoothWatcher::watchDevices(const std::vector<uint64_t>& addrs) {
    // Drop devices that are no longer of interest.
    for (auto it = watched_.begin(); it != watched_.end();) {
        if (std::find(addrs.begin(), addrs.end(), it->first) == addrs.end()) {
            try {
                if (it->second.dev) it->second.dev.ConnectionStatusChanged(it->second.token);
            } catch (...) {
            }
            it = watched_.erase(it);
        } else {
            ++it;
        }
    }
    std::weak_ptr<bool> alive = alive_;
    for (uint64_t a : addrs) {
        if (a == 0 || watched_.count(a)) continue;
        watched_[a] = {};
        [](BluetoothWatcher* self, std::weak_ptr<bool> alive, uint64_t addr) -> fire_and_forget {
            co_await resume_background();
            BluetoothDevice dev{nullptr};
            try {
                dev = co_await BluetoothDevice::FromBluetoothAddressAsync(addr);
            } catch (...) {
            }
            if (!dev) co_return;
            bool connected = dev.ConnectionStatus() == BluetoothConnectionStatus::Connected;
            self->sched_.post([self, alive, addr, dev, connected] {
                if (alive.expired()) return;
                auto it = self->watched_.find(addr);
                if (it == self->watched_.end()) return;
                it->second.dev = dev;
                it->second.token = dev.ConnectionStatusChanged([self, alive, addr](BluetoothDevice const& d, auto&&) {
                    bool c = false;
                    try {
                        c = d.ConnectionStatus() == BluetoothConnectionStatus::Connected;
                    } catch (...) {
                    }
                    self->sched_.post([self, alive, addr, c] {
                        if (!alive.expired() && self->cb_.onPresence) self->cb_.onPresence(addr, c);
                    });
                });
                if (self->cb_.onPresence) self->cb_.onPresence(addr, connected);
            });
        }(this, alive, a);
    }
}

void BluetoothWatcher::resolveByName(const std::string& name, std::function<void(uint64_t)> done) {
    std::weak_ptr<bool> alive = alive_;
    [](BluetoothWatcher* self, std::weak_ptr<bool> alive, std::wstring want,
       std::function<void(uint64_t)> done) -> fire_and_forget {
        co_await resume_background();
        uint64_t found = 0;
        try {
            auto infos = co_await DeviceInformation::FindAllAsync(BluetoothDevice::GetDeviceSelectorFromPairingState(true));
            for (auto const& info : infos) {
                BluetoothDevice dev{nullptr};
                try {
                    dev = co_await BluetoothDevice::FromIdAsync(info.Id());
                } catch (...) {
                }
                if (!dev || _wcsicmp(dev.Name().c_str(), want.c_str()) != 0) continue;
                auto sid = RfcommServiceId::FromUuid(rfcommServiceGuid());
                auto res = co_await dev.GetRfcommServicesForIdAsync(sid, BluetoothCacheMode::Cached);
                if (res.Services().Size() == 0)
                    res = co_await dev.GetRfcommServicesForIdAsync(sid, BluetoothCacheMode::Uncached);
                if (res.Services().Size() > 0) {
                    found = dev.BluetoothAddress();
                    break;
                }
            }
        } catch (hresult_error const& e) {
            log::warn("bt", "resolve by name failed: {}", text::toUtf8(e.message()));
        }
        self->sched_.post([alive, done, found] {
            if (!alive.expired()) done(found);
        });
    }(this, alive, text::toWide(name), std::move(done));
}

void BluetoothWatcher::discover() {
    if (discovering_) return;
    discovering_ = true;
    std::weak_ptr<bool> alive = alive_;
    [](BluetoothWatcher* self, std::weak_ptr<bool> alive) -> fire_and_forget {
        co_await resume_background();
        std::vector<BtCandidate> out;
        try {
            auto infos = co_await DeviceInformation::FindAllAsync(BluetoothDevice::GetDeviceSelectorFromPairingState(true));
            for (auto const& info : infos) {
                BluetoothDevice dev{nullptr};
                try {
                    dev = co_await BluetoothDevice::FromIdAsync(info.Id());
                } catch (...) {
                }
                if (!dev) continue;
                BtCandidate c;
                c.deviceId = text::toUtf8(info.Id());
                c.name = text::toUtf8(dev.Name());
                c.address = dev.BluetoothAddress();
                auto major = dev.ClassOfDevice().MajorClass();
                c.isPhone = major == BluetoothMajorClass::Phone;
                // Only phones (and unknown-class devices) are worth an SDP query.
                if (!c.isPhone && major != BluetoothMajorClass::Miscellaneous)
                    continue;
                try {
                    auto res = co_await dev.GetRfcommServicesForIdAsync(RfcommServiceId::FromUuid(rfcommServiceGuid()),
                                                                        BluetoothCacheMode::Uncached);
                    c.hasService = res.Services().Size() > 0;
                } catch (...) {
                }
                out.push_back(std::move(c));
            }
        } catch (hresult_error const& e) {
            log::warn("bt", "discovery failed: {}", text::toUtf8(e.message()));
        }
        self->sched_.post([self, alive, out] {
            if (alive.expired()) return;
            self->discovering_ = false;
            if (self->cb_.onCandidates) self->cb_.onCandidates(out);
        });
    }(this, alive);
}

} // namespace md::platform
