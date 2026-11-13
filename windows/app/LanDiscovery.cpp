#include "LanDiscovery.h"

#include "network/LanTransport.h"

#include "md/link/Probe.h"
#include "md/util/Log.h"

#include <algorithm>
#include <set>

namespace md::app {

namespace {
constexpr int64_t kSearchTickMs = 5'000;      // re-probe the listed phones while the page is open
constexpr int kSearchRefreshTicks = 6;        // DNS-SD query again every 30 s while searching
constexpr int kSearchScanTicks = 12;          // subnet scan every 60 s while searching
constexpr int64_t kFirstScanMs = 1'500;       // DNS-SD usually answers before that
constexpr int64_t kTickMs = 30'000;           // background: watcher health, missing phones
constexpr int64_t kBgRefreshMs = 2 * 60'000;  // DNS-SD query again while a paired phone is missing
constexpr int64_t kMissingBeforeScanMs = 30'000;
constexpr int64_t kBgScanBackoffMin[] = {5, 10, 20, 30};
constexpr int kProbeTimeoutMs = 1'500;        // per step; a sleeping phone's Wi-Fi may take a few hundred ms
constexpr int kScanTimeoutMs = 1'200;
constexpr size_t kScanParallel = 128;
constexpr size_t kMaxScanNetworks = 2;

void addUnique(std::vector<link::LanEndpoint>& v, const link::LanEndpoint& e) {
    if (std::find(v.begin(), v.end(), e) == v.end()) v.push_back(e);
}
} // namespace

LanDiscovery::LanDiscovery(IScheduler& sched, std::string selfId, Callbacks cb)
    : sched_(sched), selfId_(std::move(selfId)), cb_(std::move(cb)),
      dnssd_(sched, [this](const platform::DnssdService& s, bool available) { onService(s, available); }),
      prober_(sched, selfId_) {
    probeTimer_.bind(&sched_);
    searchTimer_.bind(&sched_);
    scanTimer_.bind(&sched_);
    tickTimer_.bind(&sched_);
}

LanDiscovery::~LanDiscovery() {
    probeTimer_.cancel();
    searchTimer_.cancel();
    scanTimer_.cancel();
    tickTimer_.cancel();
}

void LanDiscovery::configure(bool browse, bool search) {
    browse = browse || search;
    bool startSearch = search && !search_;
    bool stopSearch = !search && search_;
    browse_ = browse;
    search_ = search;
    if (browse && !dnssd_.running()) dnssd_.start();
    else if (startSearch) dnssd_.refresh(); // a fresh query for whoever just opened the page
    if (!browse && dnssd_.running()) dnssd_.stop();
    if (browse && !tickTimer_.active()) tickTimer_.start(kTickMs, [this] { tick(); });
    if (!browse) {
        tickTimer_.cancel();
        missingSince_ = -1;
    }
    if (startSearch) {
        searchTicks_ = 0;
        for (auto& [id, e] : entries_) e.reach = LanDevice::Reach::Checking;
        armSearch();
        scheduleProbe(300);
        scanTimer_.start(kFirstScanMs, [this] { scan({}); });
    }
    if (stopSearch) {
        searchTimer_.cancel();
        scanTimer_.cancel();
        probeTimer_.cancel();
        // Scan results are only kept while someone looks at them (or a paired phone was found:
        // the ConnectionManager has its own copy and remembers what worked).
        for (auto& [id, e] : entries_)
            if (e.scanned) {
                if (cb_.endpoint) cb_.endpoint(id, *e.scanned, false);
                e.scanned.reset();
            }
        prune();
    }
    if (cb_.changed) cb_.changed();
}

void LanDiscovery::armSearch() {
    searchTimer_.start(kSearchTickMs, [this] {
        if (!search_) return;
        ++searchTicks_;
        if (searchTicks_ % kSearchRefreshTicks == 0) dnssd_.refresh();
        if (searchTicks_ % kSearchScanTicks == 0) scan({});
        probeAll();
        armSearch();
    });
}

void LanDiscovery::onNetworkChanged() {
    if (dnssd_.running()) dnssd_.restart();
    bgScans_ = 0;
    nextBgScanAt_ = sched_.nowMs() + 10'000; // let DHCP settle
    if (search_) {
        scanTimer_.start(3'000, [this] { scan({}); });
        scheduleProbe(2'000);
    }
}

void LanDiscovery::onLinkUp() {
    bgScans_ = 0;
    nextBgScanAt_ = 0;
    missingSince_ = -1;
}

LanDiscovery::Entry& LanDiscovery::entry(const std::string& id) {
    auto& e = entries_[id];
    e.id = id;
    return e;
}

void LanDiscovery::prune() {
    for (auto it = entries_.begin(); it != entries_.end();)
        if (it->second.announced.empty() && !it->second.scanned) it = entries_.erase(it);
        else ++it;
}

void LanDiscovery::onService(const platform::DnssdService& s, bool available) {
    rebuildAnnounced();
    if (cb_.endpoint && s.deviceId != selfId_) {
        auto it = entries_.find(s.deviceId);
        for (auto& ep : s.endpoints) {
            // Another instance of the same phone may still announce this address.
            bool still = it != entries_.end() &&
                         std::find(it->second.announced.begin(), it->second.announced.end(), ep) != it->second.announced.end();
            if (available || !still) cb_.endpoint(s.deviceId, ep, available);
        }
    }
    if (available) scheduleProbe(200);
    if (cb_.changed) cb_.changed();
}

void LanDiscovery::rebuildAnnounced() {
    for (auto& [id, e] : entries_) {
        e.announced.clear();
        e.txtPairing = false;
    }
    for (auto& s : dnssd_.current()) {
        if (s.deviceId == selfId_) continue;
        auto& e = entry(s.deviceId);
        if (!s.name.empty() && (!e.probeKnown || e.name.empty())) e.name = s.name;
        for (auto& ep : s.endpoints) addUnique(e.announced, ep);
        e.txtPairing = e.txtPairing || s.pairing;
    }
    prune();
}

void LanDiscovery::scheduleProbe(int64_t delayMs) {
    if (!search_ || probeTimer_.active()) return;
    probeTimer_.start(delayMs, [this] { probeAll(); });
}

void LanDiscovery::probeAll() {
    if (!search_) return;
    if (probing_) {
        probeAgain_ = true;
        return;
    }
    std::vector<link::LanEndpoint> eps;
    for (auto& [id, e] : entries_) {
        for (auto& ep : e.announced) addUnique(eps, ep);
        if (e.scanned) addUnique(eps, *e.scanned);
    }
    if (eps.empty()) return;
    probing_ = true;
    prober_.probe(std::move(eps), kProbeTimeoutMs, 32, [this](std::vector<platform::ProbeOutcome> out) {
        onProbed(std::move(out));
    });
}

void LanDiscovery::onProbed(std::vector<platform::ProbeOutcome> out) {
    probing_ = false;
    auto outcome = [&](const link::LanEndpoint& ep) -> const platform::ProbeOutcome* {
        for (auto& o : out)
            if (o.ep == ep) return &o;
        return nullptr;
    };
    for (auto& [id, e] : entries_) {
        std::vector<link::LanEndpoint> eps = e.announced;
        if (e.scanned) addUnique(eps, *e.scanned);
        const platform::ProbeOutcome* best = nullptr;
        bool probed = false;
        for (auto& ep : eps) {
            const auto* o = outcome(ep);
            if (!o) continue;
            probed = true;
            // Only an answer in our framing counts: a TUN-mode proxy or VPN on the PC accepts
            // TCP connections to any address by itself.
            if (!o->protocol) continue;
            if (o->info && o->info->id != id) continue; // another phone has that address now
            if (!best || (o->info && !best->info)) best = o;
        }
        if (best) {
            e.reach = LanDevice::Reach::Ok;
            e.failures = 0;
            e.answered = best->ep;
            e.probeKnown = best->info.has_value();
            e.probePairing = best->info && best->info->pairing;
            if (best->info && !best->info->name.empty()) e.name = best->info->name;
        } else if (probed) {
            // A phone that answered before gets a second chance (Wi-Fi power save, a busy moment).
            if (e.reach != LanDevice::Reach::Ok || ++e.failures >= 2) {
                e.reach = LanDevice::Reach::Unreachable;
                e.answered.reset();
                e.probeKnown = false;
            }
        }
    }
    if (cb_.changed) cb_.changed();
    if (probeAgain_) {
        probeAgain_ = false;
        scheduleProbe(100);
    }
}

void LanDiscovery::scan(const std::vector<uint32_t>& only) {
    if (scanning_) return;
    std::vector<link::LanEndpoint> eps;
    std::vector<uint32_t> nets;
    for (auto& [addr, prefix] : platform::LanProber::localNetworks()) {
        uint32_t n = link::net24(addr);
        if (!only.empty() && std::find(only.begin(), only.end(), n) == only.end()) continue;
        if (std::find(nets.begin(), nets.end(), n) != nets.end() || nets.size() >= kMaxScanNetworks) continue;
        auto hosts = link::scanHosts(addr, prefix);
        if (hosts.empty()) continue;
        nets.push_back(n);
        for (uint32_t h : hosts) eps.push_back({link::formatIpv4(h), platform::kDefaultLanPort, "scan"});
    }
    if (eps.empty()) return;
    scanning_ = true;
    log::info("lan", "scanning {} addresses on {} network(s) for MediaDuet phones", eps.size(), nets.size());
    prober_.probe(std::move(eps), kScanTimeoutMs, kScanParallel,
                  [this, nets](std::vector<platform::ProbeOutcome> out) { onScanned(std::move(out), nets); });
}

void LanDiscovery::onScanned(std::vector<platform::ProbeOutcome> out, std::vector<uint32_t> nets) {
    scanning_ = false;
    std::set<std::string> found;
    for (auto& o : out) {
        if (!o.info || o.info->id == selfId_ || !proto::isValidDeviceId(o.info->id)) continue;
        if (!found.insert(o.info->id).second) continue; // the first address that answered
        auto& e = entry(o.info->id);
        bool moved = !e.scanned || !(*e.scanned == o.ep);
        if (e.scanned && moved && cb_.endpoint) cb_.endpoint(e.id, *e.scanned, false);
        e.scanned = o.ep;
        if (!o.info->name.empty()) e.name = o.info->name;
        e.reach = LanDevice::Reach::Ok;
        e.failures = 0;
        e.answered = o.ep;
        e.probeKnown = true;
        e.probePairing = o.info->pairing;
        if (moved) {
            log::info("lan", "scan found {} at {}", e.name, o.ep.str());
            if (cb_.endpoint) cb_.endpoint(e.id, o.ep, true);
        }
    }
    // Earlier hits on the scanned networks that did not answer this time are gone.
    for (auto& [id, e] : entries_) {
        if (!e.scanned || found.count(id)) continue;
        auto a = link::parseIpv4(e.scanned->host);
        if (!a || std::find(nets.begin(), nets.end(), link::net24(*a)) == nets.end()) continue;
        if (cb_.endpoint) cb_.endpoint(id, *e.scanned, false);
        e.scanned.reset();
    }
    prune();
    if (cb_.changed) cb_.changed();
}

void LanDiscovery::tick() {
    tickTimer_.start(kTickMs, [this] { tick(); });
    dnssd_.ensureAlive();
    int64_t now = sched_.nowMs();
    std::vector<std::pair<std::string, std::vector<link::LanEndpoint>>> missing;
    if (cb_.missing) missing = cb_.missing();
    if (missing.empty()) {
        missingSince_ = -1;
        return;
    }
    if (missingSince_ < 0) {
        missingSince_ = now;
        nextBgRefreshAt_ = now + kBgRefreshMs;
    }
    // A paired phone is gone: ask again now and then (an announcement may have been missed
    // while this PC slept), and as a last resort look for it on the /24 it was last seen on.
    if (now >= nextBgRefreshAt_) {
        dnssd_.refresh();
        nextBgRefreshAt_ = now + kBgRefreshMs;
    }
    if (search_ || now - missingSince_ < kMissingBeforeScanMs || now < nextBgScanAt_) return;
    std::vector<uint32_t> nets;
    for (auto& [id, eps] : missing)
        for (auto& ep : eps)
            if (auto a = link::parseIpv4(ep.host); a && link::isPrivateIpv4(*a))
                if (std::find(nets.begin(), nets.end(), link::net24(*a)) == nets.end()) nets.push_back(link::net24(*a));
    if (nets.empty()) return;
    nextBgScanAt_ = now + kBgScanBackoffMin[std::min(bgScans_, 3)] * 60'000;
    ++bgScans_;
    scan(nets);
}

std::vector<LanDevice> LanDiscovery::devices() const {
    std::vector<LanDevice> out;
    for (auto& [id, e] : entries_) {
        if (e.announced.empty() && !e.scanned) continue;
        LanDevice d;
        d.id = id;
        d.name = e.name.empty() ? id.substr(0, 8) : e.name;
        d.endpoint = e.answered ? *e.answered : !e.announced.empty() ? e.announced.front() : *e.scanned;
        d.pairing = e.probeKnown ? e.probePairing : e.txtPairing;
        d.reach = e.reach;
        d.viaScan = e.announced.empty();
        out.push_back(std::move(d));
    }
    // Phones waiting to pair first, then the reachable ones.
    std::stable_sort(out.begin(), out.end(), [](const LanDevice& a, const LanDevice& b) {
        if (a.pairing != b.pairing) return a.pairing;
        bool ar = a.reach != LanDevice::Reach::Unreachable, br = b.reach != LanDevice::Reach::Unreachable;
        if (ar != br) return ar;
        return a.name < b.name;
    });
    return out;
}

} // namespace md::app
