#pragma once

#include "network/DnssdBrowser.h"
#include "network/LanProbe.h"

#include "md/link/ConnectionManager.h"
#include "md/util/Scheduler.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace md::app {

// A MediaDuet phone on the local network, as the devices page shows it.
struct LanDevice {
    enum class Reach { Checking, Ok, Unreachable };
    std::string id;   // deviceId
    std::string name;
    link::LanEndpoint endpoint; // the one to pair with: answered our probe, else the first announced
    bool pairing = false;       // the phone's pairing window is open
    Reach reach = Reach::Checking;
    bool viaScan = false;       // found by the subnet scan, not (only) by DNS-SD
};

// LAN discovery of MediaDuet phones (docs/architecture.md, "LAN discovery"):
//  * DNS-SD (DnssdBrowser), refreshed while someone looks for phones and while a paired phone
//    is missing, restarted when the system watcher dies;
//  * every result is probed (spec §3.5): reachable or not, pairing window open or not;
//  * a subnet scan of the PC's own /24 when multicast does not get through: at once and then
//    every minute while the devices page is open, rarely and only on the /24 a missing paired
//    phone was last seen on otherwise.
class LanDiscovery {
public:
    struct Callbacks {
        // An address of a phone appeared / disappeared (DNS-SD or scan). The host passes those
        // of paired phones to the ConnectionManager.
        std::function<void(const std::string& deviceId, const link::LanEndpoint& ep, bool available)> endpoint;
        // The list for the devices page changed.
        std::function<void()> changed;
        // Paired phones without any link right now, with the LAN endpoints they were last
        // reachable on (for the targeted scan).
        std::function<std::vector<std::pair<std::string, std::vector<link::LanEndpoint>>>()> missing;
    };

    LanDiscovery(IScheduler& sched, std::string selfId, Callbacks cb);
    ~LanDiscovery();

    // browse: keep DNS-SD running (paired phones over LAN, or someone searching).
    // search: the user is looking for phones to pair (devices page open).
    void configure(bool browse, bool search);
    void onNetworkChanged();
    // A link came up: the background scan starts over from its shortest interval next time.
    void onLinkUp();

    std::vector<LanDevice> devices() const;
    std::vector<platform::DnssdService> dnssdServices() const { return dnssd_.current(); }

private:
    struct Entry {
        std::string id, name;
        std::vector<link::LanEndpoint> announced; // DNS-SD, every address of every instance
        bool txtPairing = false;
        std::optional<link::LanEndpoint> scanned;  // last subnet scan hit
        std::optional<link::LanEndpoint> answered; // answered the last probe
        bool probePairing = false;
        bool probeKnown = false; // the last probe got a probe.result
        LanDevice::Reach reach = LanDevice::Reach::Checking;
        int failures = 0;
    };

    void onService(const platform::DnssdService& s, bool available);
    void rebuildAnnounced();
    void scheduleProbe(int64_t delayMs);
    void probeAll();
    void onProbed(std::vector<platform::ProbeOutcome> out);
    // Scans the PC's private /24s; `only` limits it to these /24 networks (empty = all).
    void scan(const std::vector<uint32_t>& only);
    void onScanned(std::vector<platform::ProbeOutcome> out, std::vector<uint32_t> nets);
    void tick();
    void armSearch();
    Entry& entry(const std::string& id);
    void prune();

    IScheduler& sched_;
    std::string selfId_;
    Callbacks cb_;
    platform::DnssdBrowser dnssd_;
    platform::LanProber prober_;
    std::map<std::string, Entry> entries_;
    bool browse_ = false, search_ = false;
    bool probing_ = false, probeAgain_ = false, scanning_ = false;
    ScopedTimer probeTimer_, searchTimer_, scanTimer_, tickTimer_;
    int64_t missingSince_ = -1;
    int64_t nextBgRefreshAt_ = 0, nextBgScanAt_ = 0;
    int bgScans_ = 0;
    int searchTicks_ = 0;
};

} // namespace md::app
