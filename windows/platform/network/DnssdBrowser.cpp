#include "DnssdBrowser.h"

#include "md/util/Log.h"
#include "md/util/Text.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>

namespace md::platform {

using namespace winrt;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;

namespace {

const wchar_t* kAqs = L"System.Devices.AepService.ProtocolId:=\"{4526e8c1-8aac-4153-9b16-55e86ada0e54}\" AND "
                      L"System.Devices.Dnssd.Domain:=\"local\" AND System.Devices.Dnssd.ServiceName:=\"_mediaduet._tcp\"";
const wchar_t* kHost = L"System.Devices.Dnssd.HostName";
const wchar_t* kPort = L"System.Devices.Dnssd.PortNumber";
const wchar_t* kTxt = L"System.Devices.Dnssd.TextAttributes";
const wchar_t* kIp = L"System.Devices.IpAddress";
const wchar_t* kInstance = L"System.Devices.Dnssd.InstanceName";

constexpr size_t kMaxEndpoints = 4;   // untrusted multicast input
constexpr size_t kMaxServices = 64;
constexpr int64_t kSweepFallbackMs = 10'000; // if the new watcher never completes its enumeration
constexpr int64_t kRepeatGapMs = 3'000;       // the same unchanged report again within this: ignored

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = s.substr(i + 1, 2);
            char* end = nullptr;
            long v = strtol(hex.c_str(), &end, 16);
            if (end == hex.c_str() + 2) {
                out += static_cast<char>(v);
                i += 2;
                continue;
            }
        }
        out += s[i] == '+' ? ' ' : s[i];
    }
    return out;
}

} // namespace

DnssdBrowser::DnssdBrowser(IScheduler& sched, Callback cb) : sched_(sched), cb_(std::move(cb)) {
    restartTimer_.bind(&sched_);
    sweepTimer_.bind(&sched_);
}

DnssdBrowser::~DnssdBrowser() {
    *alive_ = false;
    restartTimer_.cancel();
    sweepTimer_.cancel();
    destroyWatcher();
}

void DnssdBrowser::start() {
    if (running_) return;
    running_ = true;
    failures_ = 0;
    createWatcher();
    if (watcher_) log::info("dnssd", "browsing _mediaduet._tcp");
}

void DnssdBrowser::stop() {
    if (!running_ && !watcher_) return;
    running_ = false;
    restartTimer_.cancel();
    sweepTimer_.cancel();
    destroyWatcher();
    unconfirmed_.clear();
    lastReport_.clear();
    auto lost = std::move(services_);
    services_.clear();
    props_.clear();
    for (auto& [id, svc] : lost) cb_(svc, false);
}

void DnssdBrowser::restart() {
    stop();
    start();
}

void DnssdBrowser::refresh() {
    if (!running_) return start();
    restartTimer_.cancel();
    destroyWatcher();
    props_.clear();
    for (auto& [id, svc] : services_) unconfirmed_.insert(id);
    createWatcher();
    if (!watcher_) return;
    uint64_t gen = gen_;
    sweepTimer_.start(kSweepFallbackMs, [this, gen] { sweep(gen); });
}

void DnssdBrowser::ensureAlive() {
    if (!running_ || !watcher_) return;
    DeviceWatcherStatus st = DeviceWatcherStatus::Aborted;
    try {
        st = watcher_.Status();
    } catch (...) {
    }
    // An aborted watcher raises no further events (not necessarily Stopped either).
    if (st == DeviceWatcherStatus::Aborted || st == DeviceWatcherStatus::Stopped) onStopped(gen_);
}

std::vector<DnssdService> DnssdBrowser::current() const {
    std::vector<DnssdService> out;
    for (auto& [id, s] : services_) out.push_back(s);
    return out;
}

void DnssdBrowser::createWatcher() {
    uint64_t gen = ++gen_;
    try {
        watcher_ = DeviceInformation::CreateWatcher(kAqs, {kHost, kPort, kTxt, kIp, kInstance},
                                                    DeviceInformationKind::AssociationEndpointService);
        std::weak_ptr<bool> alive = alive_;
        added_ = watcher_.Added([this, alive, gen](DeviceWatcher const&, DeviceInformation const& info) {
            sched_.post([this, alive, gen, info] {
                if (!alive.expired()) onInfo(gen, info);
            });
        });
        updated_ = watcher_.Updated([this, alive, gen](DeviceWatcher const&, DeviceInformationUpdate const& upd) {
            sched_.post([this, alive, gen, upd] {
                if (!alive.expired()) onUpdate(gen, upd);
            });
        });
        removed_ = watcher_.Removed([this, alive, gen](DeviceWatcher const&, DeviceInformationUpdate const& upd) {
            std::wstring id(upd.Id());
            sched_.post([this, alive, gen, id] {
                if (!alive.expired()) onRemoved(gen, id);
            });
        });
        completed_ = watcher_.EnumerationCompleted([this, alive, gen](DeviceWatcher const&, IInspectable const&) {
            sched_.post([this, alive, gen] {
                if (!alive.expired()) sweep(gen);
            });
        });
        stopped_ = watcher_.Stopped([this, alive, gen](DeviceWatcher const&, IInspectable const&) {
            sched_.post([this, alive, gen] {
                if (!alive.expired()) onStopped(gen);
            });
        });
        watcher_.Start();
    } catch (hresult_error const& e) {
        log::warn("dnssd", "cannot start DNS-SD watcher: {}", text::toUtf8(e.message()));
        destroyWatcher();
        onStopped(gen);
    }
}

void DnssdBrowser::destroyWatcher() {
    if (!watcher_) return;
    try {
        // Unhook first: an intentional stop must not look like the watcher dying.
        watcher_.Added(added_);
        watcher_.Updated(updated_);
        watcher_.Removed(removed_);
        watcher_.EnumerationCompleted(completed_);
        watcher_.Stopped(stopped_);
        auto st = watcher_.Status();
        if (st == DeviceWatcherStatus::Started || st == DeviceWatcherStatus::EnumerationCompleted) watcher_.Stop();
    } catch (...) {
    }
    watcher_ = nullptr;
}

void DnssdBrowser::onStopped(uint64_t gen) {
    if (gen != gen_ || !running_) return;
    // The watcher died by itself (Aborted after sleep / resume, a network stack reset): keep
    // the services we know and try again, backing off if it keeps failing.
    destroyWatcher();
    int64_t delay = std::min<int64_t>(60'000, 2'000LL << std::min(failures_, 5));
    ++failures_;
    log::warn("dnssd", "DNS-SD watcher stopped unexpectedly; restarting in {} s", delay / 1000);
    restartTimer_.start(delay, [this] {
        if (!running_ || watcher_) return;
        for (auto& [id, svc] : services_) unconfirmed_.insert(id);
        props_.clear();
        createWatcher();
        if (watcher_) {
            uint64_t g = gen_;
            sweepTimer_.start(kSweepFallbackMs, [this, g] { sweep(g); });
        }
    });
}

void DnssdBrowser::sweep(uint64_t gen) {
    if (gen != gen_) return;
    failures_ = 0; // the watcher works
    sweepTimer_.cancel();
    if (unconfirmed_.empty()) return;
    auto gone = std::move(unconfirmed_);
    unconfirmed_.clear();
    for (auto& id : gone) {
        auto it = services_.find(id);
        if (it == services_.end()) continue;
        auto svc = it->second;
        services_.erase(it);
        props_.erase(id);
        lastReport_.erase(id);
        log::info("dnssd", "{} is no longer announced", svc.name);
        cb_(svc, false);
    }
}

void DnssdBrowser::onInfo(uint64_t gen, DeviceInformation const& info) {
    if (gen != gen_) return;
    std::wstring id(info.Id());
    if (!props_.count(id) && props_.size() >= kMaxServices) return;
    auto& bag = props_[id];
    for (auto const& kv : info.Properties()) bag[std::wstring(kv.Key())] = kv.Value();
    evaluate(id);
}

void DnssdBrowser::onUpdate(uint64_t gen, DeviceInformationUpdate const& upd) {
    if (gen != gen_) return;
    std::wstring id(upd.Id());
    if (!props_.count(id) && props_.size() >= kMaxServices) return;
    auto& bag = props_[id];
    for (auto const& kv : upd.Properties()) bag[std::wstring(kv.Key())] = kv.Value();
    evaluate(id);
}

void DnssdBrowser::evaluate(const std::wstring& id) {
    auto& bag = props_[id];
    DnssdService svc;
    svc.instanceId = text::toUtf8(id);
    auto getStr = [&](const wchar_t* k) -> std::wstring {
        auto it = bag.find(k);
        if (it == bag.end() || !it->second) return {};
        if (auto s = it->second.try_as<IReference<hstring>>()) return std::wstring(s.Value());
        return {};
    };
    uint16_t port = 0;
    if (auto it = bag.find(kPort); it != bag.end() && it->second)
        if (auto v = it->second.try_as<IReference<uint16_t>>()) port = v.Value();
    if (auto it = bag.find(kTxt); it != bag.end() && it->second) {
        if (auto arr = it->second.try_as<IReferenceArray<hstring>>()) {
            auto items = arr.Value();
            for (auto& h : items) {
                std::string kv = text::toUtf8(h);
                auto eq = kv.find('=');
                if (eq == std::string::npos) continue;
                auto k = kv.substr(0, eq), v = kv.substr(eq + 1);
                if (k == "id") svc.deviceId = v;
                else if (k == "n") svc.name = text::truncate(urlDecode(v), 64);
                else if (k == "pair") svc.pairing = v == "1";
            }
        }
    }
    std::vector<std::string> v4, v6;
    if (auto it = bag.find(kIp); it != bag.end() && it->second) {
        if (auto arr = it->second.try_as<IReferenceArray<hstring>>()) {
            auto ips = arr.Value();
            for (auto& ip : ips) {
                std::string s = text::toUtf8(ip);
                if (s.empty()) continue;
                if (s.find(':') == std::string::npos) {
                    if (std::find(v4.begin(), v4.end(), s) == v4.end()) v4.push_back(s);
                } else if (s.rfind("fe80", 0) != 0) {
                    v6.push_back(s);
                }
            }
        }
    }
    // IPv4 first (the phone's server and its transport.info are IPv4); IPv6 only without it.
    std::vector<std::string> hosts = !v4.empty() ? v4 : v6;
    if (hosts.empty()) {
        auto h = text::toUtf8(getStr(kHost));
        if (!h.empty()) hosts.push_back(h);
    }
    for (auto& h : hosts) {
        if (svc.endpoints.size() >= kMaxEndpoints) break;
        svc.endpoints.push_back({h, port, "mdns"});
    }
    if (svc.name.empty()) svc.name = text::truncate(text::toUtf8(getStr(kInstance)), 64);
    if (svc.deviceId.size() != 32 || port == 0 || svc.endpoints.empty()) return;

    bool wasUnconfirmed = unconfirmed_.erase(id) > 0;
    int64_t now = sched_.nowMs();
    auto old = services_.find(id);
    if (old != services_.end() && old->second.endpoints == svc.endpoints && old->second.deviceId == svc.deviceId) {
        bool changed = old->second.pairing != svc.pairing || old->second.name != svc.name;
        old->second = svc;
        auto& last = lastReport_[id];
        // Still there after a refresh(), or the watcher repeating itself (it sends Added and
        // several Updated in a row): nothing to report. A re-announcement on the running
        // watcher (the phone app restarted) or a changed TXT (its pairing window opened) is
        // reported, and lets the link retry now.
        if (!changed && (wasUnconfirmed || now - last < kRepeatGapMs)) {
            if (wasUnconfirmed) last = now;
            return;
        }
        last = now;
        cb_(svc, true);
        return;
    }
    if (old != services_.end()) cb_(old->second, false);
    lastReport_[id] = now;
    services_[id] = svc;
    log::info("dnssd", "found MediaDuet phone {} at {}{}", svc.name, svc.endpoint().str(),
              svc.endpoints.size() > 1 ? " (+" + std::to_string(svc.endpoints.size() - 1) + " more)" : "");
    cb_(svc, true);
}

void DnssdBrowser::onRemoved(uint64_t gen, const std::wstring& id) {
    if (gen != gen_) return;
    props_.erase(id);
    unconfirmed_.erase(id);
    lastReport_.erase(id);
    auto it = services_.find(id);
    if (it == services_.end()) return;
    auto svc = it->second;
    services_.erase(it);
    cb_(svc, false);
}

} // namespace md::platform
