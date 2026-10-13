#pragma once

#include "md/link/ConnectionManager.h"
#include "md/util/Scheduler.h"

#include <winrt/Windows.Devices.Enumeration.h>

#include <functional>
#include <map>
#include <string>

namespace md::platform {

struct DnssdService {
    std::string instanceId; // DeviceInformation id
    std::string deviceId;   // TXT id=
    std::string name;       // TXT n= (or instance name)
    link::LanEndpoint endpoint;
};

// Event-driven DNS-SD browser for _mediaduet._tcp using the system DeviceWatcher
// (no raw multicast sockets, no polling). Callbacks on the scheduler thread.
class DnssdBrowser {
public:
    using Callback = std::function<void(const DnssdService& svc, bool available)>;
    DnssdBrowser(IScheduler& sched, Callback cb);
    ~DnssdBrowser();
    void start();
    void stop();
    void restart();
    bool running() const { return running_; }
    std::vector<DnssdService> current() const;

private:
    void onInfo(winrt::Windows::Devices::Enumeration::DeviceInformation const& info);
    void onUpdate(winrt::Windows::Devices::Enumeration::DeviceInformationUpdate const& upd);
    void onRemoved(const std::wstring& id);
    void evaluate(const std::wstring& id);

    IScheduler& sched_;
    Callback cb_;
    winrt::Windows::Devices::Enumeration::DeviceWatcher watcher_{nullptr};
    winrt::event_token added_{}, updated_{}, removed_{}, stopped_{};
    std::map<std::wstring, DnssdService> services_; // scheduler thread
    std::map<std::wstring, std::map<std::wstring, winrt::Windows::Foundation::IInspectable>> props_;
    bool running_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace md::platform
