#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "LanProbe.h"

#include "md/util/Log.h"
#include "md/util/Text.h"

#include <algorithm>
#include <cwctype>

namespace md::platform {

namespace {

int64_t tickMs() { return static_cast<int64_t>(GetTickCount64()); }

bool toSockaddr(const link::LanEndpoint& ep, sockaddr_storage& addr, int& len) {
    addr = {};
    auto* v4 = reinterpret_cast<sockaddr_in*>(&addr);
    if (inet_pton(AF_INET, ep.host.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(ep.port);
        len = sizeof(sockaddr_in);
        return true;
    }
    auto* v6 = reinterpret_cast<sockaddr_in6*>(&addr);
    if (inet_pton(AF_INET6, ep.host.c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(ep.port);
        len = sizeof(sockaddr_in6);
        return true;
    }
    // A DNS-SD host name (rare: announcements normally carry addresses).
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* res = nullptr;
    if (getaddrinfo(ep.host.c_str(), std::to_string(ep.port).c_str(), &hints, &res) != 0 || !res) return false;
    memcpy(&addr, res->ai_addr, std::min<size_t>(res->ai_addrlen, sizeof(addr)));
    len = static_cast<int>(res->ai_addrlen);
    freeaddrinfo(res);
    return true;
}

struct Attempt {
    SOCKET sock = INVALID_SOCKET;
    size_t idx = 0;
    bool connected = false;
    int64_t started = 0;
    int64_t deadline = 0;
    link::ProbeReader reader;
};

void runProbe(const std::vector<link::LanEndpoint>& eps, int timeoutMs, size_t maxParallel, const Bytes& request,
              const std::atomic<bool>& stop, std::vector<ProbeOutcome>& out) {
    out.resize(eps.size());
    for (size_t i = 0; i < eps.size(); ++i) out[i].ep = eps[i];
    std::vector<Attempt> active;
    active.reserve(maxParallel);
    size_t next = 0;
    while (!stop.load()) {
        while (active.size() < maxParallel && next < eps.size()) {
            size_t i = next++;
            sockaddr_storage addr;
            int len = 0;
            if (!toSockaddr(eps[i], addr, len)) continue;
            SOCKET s = ::socket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) continue;
            u_long nonBlocking = 1;
            ioctlsocket(s, FIONBIO, &nonBlocking);
            BOOL noDelay = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
            if (::connect(s, reinterpret_cast<sockaddr*>(&addr), len) == SOCKET_ERROR &&
                WSAGetLastError() != WSAEWOULDBLOCK) {
                closesocket(s);
                continue;
            }
            Attempt a;
            a.sock = s;
            a.idx = i;
            a.started = tickMs();
            a.deadline = a.started + timeoutMs;
            active.push_back(std::move(a));
        }
        if (active.empty()) break;
        std::vector<WSAPOLLFD> fds(active.size());
        for (size_t k = 0; k < active.size(); ++k) {
            fds[k].fd = active[k].sock;
            fds[k].events = active[k].connected ? POLLRDNORM : POLLWRNORM;
        }
        if (WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), 50) == SOCKET_ERROR) break;
        int64_t now = tickMs();
        for (size_t k = active.size(); k-- > 0;) {
            Attempt& a = active[k];
            short re = fds[k].revents;
            bool finished = false;
            if (!a.connected) {
                if (re & (POLLERR | POLLHUP | POLLNVAL)) {
                    finished = true; // refused / unreachable
                } else if (re & POLLWRNORM) {
                    int err = 0, errLen = sizeof(err);
                    getsockopt(a.sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &errLen);
                    if (err != 0) {
                        finished = true;
                    } else {
                        a.connected = true;
                        out[a.idx].reachable = true;
                        out[a.idx].connectMs = now - a.started;
                        // A few hundred bytes into an empty send buffer: never partial.
                        if (::send(a.sock, reinterpret_cast<const char*>(request.data()), static_cast<int>(request.size()),
                                   0) != static_cast<int>(request.size()))
                            finished = true;
                        a.deadline = now + timeoutMs;
                    }
                } else if (now >= a.deadline) {
                    finished = true;
                }
            } else if (re & (POLLRDNORM | POLLHUP | POLLERR)) {
                char buf[4096];
                int n = recv(a.sock, buf, sizeof(buf), 0);
                if (n > 0) finished = a.reader.feed(ByteView(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n)));
                else if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) finished = true;
            } else if (now >= a.deadline) {
                finished = true;
            }
            if (finished) {
                if (a.connected) {
                    out[a.idx].protocol = a.reader.speaksProtocol();
                    out[a.idx].info = a.reader.info();
                }
                closesocket(a.sock);
                active.erase(active.begin() + static_cast<std::ptrdiff_t>(k));
            }
        }
    }
    for (auto& a : active) closesocket(a.sock);
}

bool isVirtualAdapter(const IP_ADAPTER_ADDRESSES* a) {
    std::wstring s = std::wstring(a->Description ? a->Description : L"") + L"|" +
                     std::wstring(a->FriendlyName ? a->FriendlyName : L"");
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    static const wchar_t* kVirtual[] = {L"hyper-v", L"vethernet", L"vmware", L"virtualbox", L"zerotier", L"tap-",
                                        L"tap adapter", L"wintun", L"wireguard", L"tailscale", L"openvpn", L"npcap",
                                        L"loopback", L"bluetooth", L"docker", L"wsl", L"mihomo", L"clash", L"sing-box",
                                        L"vpn", L"virtual"};
    for (auto* k : kVirtual)
        if (s.find(k) != std::wstring::npos) return true;
    return false;
}

} // namespace

LanProber::LanProber(IScheduler& sched, std::string selfId) : sched_(sched), selfId_(std::move(selfId)) {
    WSADATA wsa;
    wsa_ = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

LanProber::~LanProber() {
    *alive_ = false;
    stop_->store(true);
    for (auto& w : workers_)
        if (w.thread.joinable()) w.thread.join();
    if (wsa_) WSACleanup();
}

void LanProber::probe(std::vector<link::LanEndpoint> eps, int timeoutMs, size_t maxParallel, Done done) {
    // Reap finished workers.
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (it->finished->load()) {
            if (it->thread.joinable()) it->thread.join();
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }
    if (eps.empty() || !wsa_) {
        sched_.post([done = std::move(done)] { done({}); });
        return;
    }
    Bytes request = link::encodeProbeRequest(selfId_, sched_.wallMs());
    auto finished = std::make_shared<std::atomic<bool>>(false);
    std::weak_ptr<bool> alive = alive_;
    auto stop = stop_;
    IScheduler* sched = &sched_;
    maxParallel = std::clamp<size_t>(maxParallel, 1, 256);
    Worker w;
    w.finished = finished;
    w.thread = std::thread([eps = std::move(eps), timeoutMs, maxParallel, request = std::move(request), stop, finished,
                            alive, sched, done = std::move(done)]() mutable {
        std::vector<ProbeOutcome> out;
        runProbe(eps, timeoutMs, maxParallel, request, *stop, out);
        if (!stop->load())
            sched->post([alive, done = std::move(done), out = std::move(out)]() mutable {
                if (!alive.expired()) done(std::move(out));
            });
        finished->store(true);
    });
    workers_.push_back(std::move(w));
}

std::vector<std::pair<uint32_t, int>> LanProber::localNetworks() {
    std::vector<std::pair<uint32_t, int>> out;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf;
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int i = 0; i < 3 && rc == ERROR_BUFFER_OVERFLOW; ++i) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211) continue;
        if (!a->FirstGatewayAddress || isVirtualAdapter(a)) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            uint32_t addr = ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
            if (!link::isPrivateIpv4(addr)) continue;
            out.emplace_back(addr, static_cast<int>(u->OnLinkPrefixLength));
        }
    }
    return out;
}

} // namespace md::platform
