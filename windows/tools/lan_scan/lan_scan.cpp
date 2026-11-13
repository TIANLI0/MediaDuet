// Developer tool: LAN discovery as the host does it, from the command line. Not shipped.
//
//   MediaDuetLanScan host[:port] ...   probe these endpoints (spec §3.5)
//   MediaDuetLanScan --scan            probe the PC's private /24 networks on port 47321
//   MediaDuetLanScan --dnssd [sec]     browse DNS-SD for sec seconds (default 8), refreshing once
//
// Prints the local networks first. Console only, no windows.

#include "Scheduler.h"
#include "network/DnssdBrowser.h"
#include "network/LanProbe.h"
#include "network/LanTransport.h"

#include "md/link/Probe.h"
#include "md/util/Text.h"

#include <windows.h>

#include <winrt/base.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace md;

namespace {

void printOutcomes(const std::vector<platform::ProbeOutcome>& out, bool onlyHits) {
    int hits = 0;
    for (auto& o : out) {
        if (onlyHits && !o.reachable) continue;
        ++hits;
        std::printf("  %-22s %s", o.ep.str().c_str(),
                    !o.reachable ? "unreachable" : o.info ? "MediaDuet" : o.protocol ? "MediaDuet (before 1.11)" : "open, not MediaDuet");
        if (o.reachable) std::printf("  connect %lld ms", static_cast<long long>(o.connectMs));
        if (o.info) std::printf("  id %s  name \"%s\"  %s  pairing %s", o.info->id.c_str(), o.info->name.c_str(),
                                o.info->platform.c_str(), o.info->pairing ? "open" : "closed");
        std::printf("\n");
    }
    if (onlyHits) std::printf("  %d open of %zu\n", hits, out.size());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    platform::Win32Scheduler sched;
    const std::string self = "00000000000000000000000000000001"; // not a real device id

    std::printf("local networks:\n");
    for (auto& [addr, prefix] : platform::LanProber::localNetworks())
        std::printf("  %s/%d (scan %zu hosts)\n", link::formatIpv4(addr).c_str(), prefix, link::scanHosts(addr, prefix).size());

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(text::toUtf8(argv[i]));
    platform::LanProber prober(sched, self);
    int rc = 0;
    auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    };

    std::unique_ptr<platform::DnssdBrowser> dnssd;
    if (!args.empty() && args[0] == "--dnssd") {
        int seconds = args.size() > 1 ? std::max(2, std::atoi(args[1].c_str())) : 8;
        dnssd = std::make_unique<platform::DnssdBrowser>(sched, [&](const platform::DnssdService& s, bool available) {
            std::printf("[%6lld ms] %s %s  id %s  pairing %s ", static_cast<long long>(elapsed()), available ? "+" : "-",
                        s.name.c_str(), s.deviceId.c_str(), s.pairing ? "open" : "closed");
            for (auto& e : s.endpoints) std::printf(" %s", e.str().c_str());
            std::printf("\n");
        });
        dnssd->start();
        sched.schedule(seconds * 500, [&] {
            std::printf("[%6lld ms] refresh\n", static_cast<long long>(elapsed()));
            dnssd->refresh();
        });
        sched.schedule(seconds * 1000, [&] {
            std::vector<link::LanEndpoint> eps;
            for (auto& s : dnssd->current()) eps.insert(eps.end(), s.endpoints.begin(), s.endpoints.end());
            std::printf("[%6lld ms] %zu service(s); probing their addresses\n", static_cast<long long>(elapsed()),
                        dnssd->current().size());
            prober.probe(eps, 1500, 32, [&](std::vector<platform::ProbeOutcome> out) {
                printOutcomes(out, false);
                PostQuitMessage(0);
            });
        });
    } else if (!args.empty() && args[0] == "--scan") {
        std::vector<link::LanEndpoint> eps;
        for (auto& [addr, prefix] : platform::LanProber::localNetworks())
            for (uint32_t h : link::scanHosts(addr, prefix))
                eps.push_back({link::formatIpv4(h), platform::kDefaultLanPort, "scan"});
        std::printf("scanning %zu addresses...\n", eps.size());
        prober.probe(eps, 1200, 128, [&](std::vector<platform::ProbeOutcome> out) {
            std::printf("done in %lld ms\n", static_cast<long long>(elapsed()));
            printOutcomes(out, true);
            PostQuitMessage(0);
        });
    } else if (!args.empty()) {
        std::vector<link::LanEndpoint> eps;
        for (auto& a : args) {
            auto colon = a.rfind(':');
            bool v6 = a.find(':') != colon;
            link::LanEndpoint e{a, platform::kDefaultLanPort, "manual"};
            if (colon != std::string::npos && !v6) {
                e.host = a.substr(0, colon);
                e.port = static_cast<uint16_t>(std::atoi(a.c_str() + colon + 1));
            }
            eps.push_back(e);
        }
        prober.probe(eps, 1500, 32, [&](std::vector<platform::ProbeOutcome> out) {
            std::printf("done in %lld ms\n", static_cast<long long>(elapsed()));
            printOutcomes(out, false);
            PostQuitMessage(0);
        });
    } else {
        std::printf("usage: MediaDuetLanScan host[:port]... | --scan | --dnssd [seconds]\n");
        return 2;
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return rc;
}
