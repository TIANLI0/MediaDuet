#pragma once

#include "md/link/ConnectionManager.h"
#include "md/link/Probe.h"
#include "md/util/Scheduler.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace md::platform {

struct ProbeOutcome {
    link::LanEndpoint ep;
    bool reachable = false;              // the TCP connect succeeded
    bool protocol = false;               // the answer used our framing (any MediaDuet version)
    std::optional<link::ProbeInfo> info; // probe.result (phones from 1.11 on)
    int64_t connectMs = -1;
};

// Probes LAN endpoints (spec §3.5) on a worker thread: non-blocking connects run side by side,
// then each open connection gets one probe. Only outgoing connections, so no firewall rule is
// needed. Results come back on the scheduler thread.
class LanProber {
public:
    using Done = std::function<void(std::vector<ProbeOutcome>)>;
    LanProber(IScheduler& sched, std::string selfId);
    ~LanProber(); // stops and joins the workers; pending results are dropped

    // Each endpoint gets `timeoutMs` to accept and as long again to answer; at most
    // `maxParallel` sockets are open at once.
    void probe(std::vector<link::LanEndpoint> eps, int timeoutMs, size_t maxParallel, Done done);

    // Private IPv4 networks of this PC's physical Wi-Fi / Ethernet adapters that have a
    // gateway, as (address, prefix length). Virtual adapters (VPN, TUN proxies, Hyper-V,
    // ZeroTier ...) are left out.
    static std::vector<std::pair<uint32_t, int>> localNetworks();

private:
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    IScheduler& sched_;
    std::string selfId_;
    bool wsa_ = false;
    std::shared_ptr<std::atomic<bool>> stop_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::vector<Worker> workers_;
};

} // namespace md::platform
