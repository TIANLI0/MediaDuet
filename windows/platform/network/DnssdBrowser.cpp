#include "DnssdBrowser.h"

#include "md/util/Log.h"
#include "md/util/Text.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

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

DnssdBrowser::DnssdBrowser(IScheduler& sched, Callback cb) : sched_(sched), cb_(std::move(cb)) {}

DnssdBrowser::~DnssdBrowser() {
    *alive_ = false;
    stop();
}

void DnssdBrowser::start() {
    if (running_) return;
    try {
        watcher_ = DeviceInformation::CreateWatcher(kAqs, {kHost, kPort, kTxt, kIp, kInstance},
                                                    DeviceInformationKind::AssociationEndpointService);
        std::weak_ptr<bool> alive = alive_;
        added_ = watcher_.Added([this, alive](DeviceWatcher const&, DeviceInformation const& info) {
            sched_.post([this, alive, info] {
                if (!alive.expired()) onInfo(info);
            });
        });
        updated_ = watcher_.Updated([this, alive](DeviceWatcher const&, DeviceInformationUpdate const& upd) {
            sched_.post([this, alive, upd] {
                if (!alive.expired()) onUpdate(upd);
            });
        });
        removed_ = watcher_.Removed([this, alive](DeviceWatcher const&, DeviceInformationUpdate const& upd) {
            std::wstring id(upd.Id());
            sched_.post([this, alive, id] {
                if (!alive.expired()) onRemoved(id);
            });
        });
        watcher_.Start();
        running_ = true;
        log::info("dnssd", "browsing _mediaduet._tcp");
    } catch (hresult_error const& e) {
        log::warn("dnssd", "cannot start DNS-SD watcher: {}", text::toUtf8(e.message()));
        watcher_ = nullptr;
    }
}

void DnssdBrowser::stop() {
    if (!watcher_) return;
    try {
        watcher_.Added(added_);
        watcher_.Updated(updated_);
        watcher_.Removed(removed_);
        auto st = watcher_.Status();
        if (st == DeviceWatcherStatus::Started || st == DeviceWatcherStatus::EnumerationCompleted) watcher_.Stop();
    } catch (...) {
    }
    watcher_ = nullptr;
    running_ = false;
    for (auto& [id, svc] : services_) cb_(svc, false);
    services_.clear();
    props_.clear();
}

void DnssdBrowser::restart() {
    stop();
    start();
}

std::vector<DnssdService> DnssdBrowser::current() const {
    std::vector<DnssdService> out;
    for (auto& [id, s] : services_) out.push_back(s);
    return out;
}

void DnssdBrowser::onInfo(DeviceInformation const& info) {
    std::wstring id(info.Id());
    auto& bag = props_[id];
    for (auto const& kv : info.Properties()) bag[std::wstring(kv.Key())] = kv.Value();
    evaluate(id);
}

void DnssdBrowser::onUpdate(DeviceInformationUpdate const& upd) {
    std::wstring id(upd.Id());
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
    if (auto it = bag.find(kPort); it != bag.end() && it->second)
        if (auto v = it->second.try_as<IReference<uint16_t>>()) svc.endpoint.port = v.Value();
    if (auto it = bag.find(kTxt); it != bag.end() && it->second) {
        if (auto arr = it->second.try_as<IReferenceArray<hstring>>()) {
            auto items = arr.Value();
            for (auto& h : items) {
                std::string kv = text::toUtf8(h);
                auto eq = kv.find('=');
                if (eq == std::string::npos) continue;
                auto k = kv.substr(0, eq), v = kv.substr(eq + 1);
                if (k == "id") svc.deviceId = v;
                else if (k == "n") svc.name = urlDecode(v);
            }
        }
    }
    if (auto it = bag.find(kIp); it != bag.end() && it->second) {
        if (auto arr = it->second.try_as<IReferenceArray<hstring>>()) {
            auto ips = arr.Value();
            for (auto& ip : ips) {
                std::string s = text::toUtf8(ip);
                if (s.find(':') == std::string::npos) {
                    svc.endpoint.host = s; // prefer IPv4
                    break;
                }
                if (svc.endpoint.host.empty() && s.rfind("fe80", 0) != 0) svc.endpoint.host = s;
            }
        }
    }
    if (svc.endpoint.host.empty()) svc.endpoint.host = text::toUtf8(getStr(kHost));
    if (svc.name.empty()) svc.name = text::toUtf8(getStr(kInstance));
    svc.endpoint.source = "mdns";
    if (svc.deviceId.size() != 32 || svc.endpoint.port == 0 || svc.endpoint.host.empty()) return;
    auto old = services_.find(id);
    if (old != services_.end() && old->second.endpoint == svc.endpoint && old->second.deviceId == svc.deviceId) {
        // Re-announcement (e.g. the phone app restarted): lets the link retry now
        // instead of waiting for its back-off.
        cb_(svc, true);
        return;
    }
    if (old != services_.end()) cb_(old->second, false);
    services_[id] = svc;
    log::info("dnssd", "found MediaDuet phone {} at {}", svc.name, svc.endpoint.str());
    cb_(svc, true);
}

void DnssdBrowser::onRemoved(const std::wstring& id) {
    props_.erase(id);
    auto it = services_.find(id);
    if (it == services_.end()) return;
    auto svc = it->second;
    services_.erase(it);
    cb_(svc, false);
}

} // namespace md::platform
