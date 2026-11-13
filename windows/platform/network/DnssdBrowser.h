#pragma once

#include "md/link/ConnectionManager.h"
#include "md/util/Scheduler.h"

#include <winrt/Windows.Devices.Enumeration.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace md::platform {

struct DnssdService {
    std::string instanceId; // DeviceInformation id
    std::string deviceId;   // TXT id=
    std::string name;       // TXT n= (or instance name)
    // Every address the phone announced (IPv4 first; at most a few), all on the same port.
    std::vector<link::LanEndpoint> endpoints;
    bool pairing = false;   // TXT pair=1: the phone's pairing window is open
    const link::LanEndpoint& endpoint() const { return endpoints.front(); }
};

// Event-driven DNS-SD browser for _mediaduet._tcp using the system DeviceWatcher (no raw
// multicast sockets). A watcher that stops on its own (sleep / resume, network stack reset)
// is restarted with a back-off. Callbacks on the scheduler thread.
class DnssdBrowser {
public:
    using Callback = std::function<void(const DnssdService& svc, bool available)>;
    DnssdBrowser(IScheduler& sched, Callback cb);
    ~DnssdBrowser();
    void start();
    void stop();
    // Network changed: every service is lost, browsing starts over.
    void restart();
    // Asks the network again (a new watcher sends a fresh query) without reporting the known
    // services as lost; those the new watcher does not report again are dropped once its
    // first enumeration is complete.
    void refresh();
    // Restarts a watcher that aborted without telling us (cheap; call it now and then).
    void ensureAlive();
    bool running() const { return running_; }
    std::vector<DnssdService> current() const;

private:
    void createWatcher();
    void destroyWatcher();
    void onInfo(uint64_t gen, winrt::Windows::Devices::Enumeration::DeviceInformation const& info);
    void onUpdate(uint64_t gen, winrt::Windows::Devices::Enumeration::DeviceInformationUpdate const& upd);
    void onRemoved(uint64_t gen, const std::wstring& id);
    void onStopped(uint64_t gen);
    void sweep(uint64_t gen);
    void evaluate(const std::wstring& id);

    IScheduler& sched_;
    Callback cb_;
    winrt::Windows::Devices::Enumeration::DeviceWatcher watcher_{nullptr};
    winrt::event_token added_{}, updated_{}, removed_{}, stopped_{}, completed_{};
    std::map<std::wstring, DnssdService> services_; // scheduler thread
    std::map<std::wstring, std::map<std::wstring, winrt::Windows::Foundation::IInspectable>> props_;
    std::set<std::wstring> unconfirmed_; // known before refresh(), not reported again yet
    std::map<std::wstring, int64_t> lastReport_; // last callback per service (repeats are dropped)
    uint64_t gen_ = 0;                   // watcher generation; events of older watchers are ignored
    bool running_ = false;
    int failures_ = 0;
    ScopedTimer restartTimer_, sweepTimer_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace md::platform
