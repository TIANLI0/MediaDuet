#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2bth.h>
#include <windns.h>
#include "ServiceTransport.h"
#include "md/util/Text.h"
#include <deque>
#include <map>
#include <mutex>

namespace md::unlock {
namespace {
class SocketTransport final : public link::ITransport, public std::enable_shared_from_this<SocketTransport> {
public:
    SocketTransport(IScheduler& sched, SOCKET socket, link::TransportKind kind, std::string host)
        : sched_(sched), socket_(socket), kind_(kind), host_(std::move(host)) {}
    ~SocketTransport() override { close(); if (worker_.joinable()) worker_.join(); else closesocket(socket_); }
    link::TransportKind kind() const override { return kind_; }
    std::string describe() const override { return kind_ == link::TransportKind::Lan ? "boot-lan:" + host_ : "boot-bluetooth"; }
    std::string remoteHost() const override { return host_; }
    size_t pendingWrites() const override { return pending_; }
    void send(Bytes bytes) override {
        std::lock_guard lock(mutex_);
        if (!closed_ && pending_ < 512) { ++pending_; queue_.push_back(std::move(bytes)); }
    }
    void close() override { closed_ = true; }
    void wait() { close(); if (worker_.joinable()) worker_.join(); }
    void start(Listener* listener) override {
        listener_ = listener;
        worker_ = std::thread([self = shared_from_this()] { self->run(); });
    }
private:
    void run() {
        Bytes writing; size_t offset = 0;
        while (!closed_) {
            if (writing.empty()) {
                std::lock_guard lock(mutex_);
                if (!queue_.empty()) { writing = std::move(queue_.front()); queue_.pop_front(); offset = 0; }
            }
            fd_set rd, wr, err; FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&err);
            FD_SET(socket_, &rd); FD_SET(socket_, &err); if (!writing.empty()) FD_SET(socket_, &wr);
            timeval timeout{0, 100000};
            if (select(0, &rd, &wr, &err, &timeout) == SOCKET_ERROR || FD_ISSET(socket_, &err)) break;
            if (FD_ISSET(socket_, &rd)) {
                Bytes data(16384);
                int n = recv(socket_, reinterpret_cast<char*>(data.data()), static_cast<int>(data.size()), 0);
                if (n == 0) break;
                if (n < 0) { if (WSAGetLastError() != WSAEWOULDBLOCK) break; }
                else {
                    data.resize(n);
                    sched_.post([self = shared_from_this(), data = std::move(data)] {
                        if (self->listener_) self->listener_->onTransportData(data);
                    });
                }
            }
            if (!writing.empty() && FD_ISSET(socket_, &wr)) {
                int n = ::send(socket_, reinterpret_cast<const char*>(writing.data() + offset),
                               static_cast<int>(writing.size() - offset), 0);
                if (n < 0) { if (WSAGetLastError() != WSAEWOULDBLOCK) break; }
                else if ((offset += n) == writing.size()) {
                    secureZero(writing); writing.clear(); --pending_;
                    sched_.post([self = shared_from_this()] { if (self->listener_) self->listener_->onTransportWritable(); });
                }
            }
        }
        secureZero(writing); closed_ = true; closesocket(socket_);
        sched_.post([self = shared_from_this()] {
            if (auto listener = self->listener_) { self->listener_ = nullptr; listener->onTransportClosed("boot transport closed"); }
        });
    }
    IScheduler& sched_;
    SOCKET socket_;
    link::TransportKind kind_;
    std::string host_;
    Listener* listener_ = nullptr;
    std::atomic_bool closed_{false};
    std::atomic_size_t pending_{0};
    std::mutex mutex_;
    std::deque<Bytes> queue_;
    std::thread worker_;
};
bool connectSocket(SOCKET socket, const sockaddr* address, int length, const std::atomic_bool& running, int ticks = 120) {
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking)) return false;
    if (::connect(socket, address, length) == 0) return true;
    if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
    for (int i = 0; i < ticks && running; ++i) {
        fd_set wr, err; FD_ZERO(&wr); FD_ZERO(&err); FD_SET(socket, &wr); FD_SET(socket, &err);
        timeval timeout{0, 100000};
        int result = select(0, nullptr, &wr, &err, &timeout);
        if (result == SOCKET_ERROR || FD_ISSET(socket, &err)) return false;
        if (FD_ISSET(socket, &wr)) { int error = 0, size = sizeof(error); return getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) == 0 && !error; }
    }
    return false;
}
}
ServiceTransportFactory::~ServiceTransportFactory() { stop(); }
void ServiceTransportFactory::stop() {
    *running_ = false;
    for (auto& worker : workers_) if (worker.valid()) worker.wait();
    std::vector<std::shared_ptr<SocketTransport>> transports;
    { std::lock_guard lock(transportsMutex_);
      for (const auto& weak : transports_) if (auto t = weak.lock()) transports.push_back(std::static_pointer_cast<SocketTransport>(t));
    }
    for (auto& t : transports) t->close();
    for (auto& t : transports) t->wait();
}
void ServiceTransportFactory::connectBluetooth(const link::PeerRecord& peer, Done done) { connect({}, 0, peer.btAddress, std::move(done)); }
void ServiceTransportFactory::connectLan(const link::LanEndpoint& ep, Done done) { connect(ep.host, ep.port, 0, std::move(done)); }
void ServiceTransportFactory::connect(std::string host, uint16_t port, uint64_t bluetooth, Done done) {
    auto active = running_;
    std::erase_if(workers_, [](auto& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
    workers_.push_back(std::async(std::launch::async, [this, active, host = std::move(host), port, bluetooth, done = std::move(done)] {
        SOCKET connected = INVALID_SOCKET;
        auto kind = bluetooth ? link::TransportKind::Bluetooth : link::TransportKind::Lan;
        if (bluetooth) {
            SOCKET socket = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
            if (socket != INVALID_SOCKET) {
                SOCKADDR_BTH address{}; address.addressFamily = AF_BTH; address.btAddr = bluetooth;
                address.serviceClassId = {0x5f3a8c2e, 0x7d41, 0x4b9a, {0x9c, 0x61, 0x2e, 0x8d, 0x0f, 0x6b, 0x4a, 0x17}};
                if (connectSocket(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address), *active)) connected = socket;
                else closesocket(socket);
            }
        } else {
            // DNS-SD supplies numeric addresses; cached endpoints may use a hostname.
            ADDRINFOEXW hints{}; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
            ADDRINFOEXW* result = nullptr;
            OVERLAPPED resolve{}; resolve.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HANDLE cancel = nullptr; timeval timeout{3, 0};
            auto hostname = text::toWide(host), service = std::to_wstring(port);
            int status = GetAddrInfoExW(hostname.c_str(), service.c_str(), NS_DNS, nullptr, &hints, &result,
                                        &timeout, &resolve, nullptr, &cancel);
            if (status == WSA_IO_PENDING) {
                for (int i = 0; i < 30 && *active; ++i) if (WaitForSingleObject(resolve.hEvent, 100) == WAIT_OBJECT_0) break;
                if (WaitForSingleObject(resolve.hEvent, 0) != WAIT_OBJECT_0) GetAddrInfoExCancel(&cancel);
                WaitForSingleObject(resolve.hEvent, INFINITE);
                status = GetAddrInfoExOverlappedResult(&resolve);
            }
            CloseHandle(resolve.hEvent);
            if (!status) {
                for (auto p = result; p && *active; p = p->ai_next) {
                    SOCKET socket = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                    if (socket == INVALID_SOCKET) continue;
                    if (connectSocket(socket, p->ai_addr, static_cast<int>(p->ai_addrlen), *active, 30)) { connected = socket; break; }
                    closesocket(socket);
                }
            }
            if (result) FreeAddrInfoExW(result);
        }
        if (!*active) { if (connected != INVALID_SOCKET) closesocket(connected); return; }
        link::TransportPtr transport;
        if (connected != INVALID_SOCKET) transport = std::make_shared<SocketTransport>(sched_, connected, kind, host);
        if (transport) {
            std::lock_guard lock(transportsMutex_);
            std::erase_if(transports_, [](const auto& weak) { return weak.expired(); });
            transports_.push_back(transport);
        }
        sched_.post([active, done, transport] { if (*active) done(transport, transport ? "" : "phone unavailable"); else if (transport) transport->close(); });
    }));
}

struct ServiceDiscovery::State {
    IScheduler* sched;
    std::mutex mutex;
    Found found;
    DNS_SERVICE_CANCEL cancel{};
    bool pending = false;
    std::map<std::wstring, std::string> ids, addresses;
    std::map<std::wstring, std::pair<std::wstring, uint16_t>> services;
    static void WINAPI callback(DWORD status, void* context, PDNS_RECORD records) {
        auto self = static_cast<State*>(context);
        std::lock_guard lock(self->mutex);
        if (status == ERROR_SUCCESS && self->found) {
            for (auto r = records; r; r = r->pNext) {
                if (!r->pName) continue;
                std::wstring name = r->pName;
                if (r->wType == DNS_TYPE_TEXT) {
                    for (DWORD i = 0; i < r->Data.TXT.dwStringCount; ++i) {
                        std::wstring txt = r->Data.TXT.pStringArray[i];
                        if (txt.starts_with(L"id=")) self->ids[name] = text::toUtf8(txt.substr(3));
                    }
                } else if (r->wType == DNS_TYPE_SRV && r->Data.SRV.pNameTarget) {
                    self->services[name] = {r->Data.SRV.pNameTarget, r->Data.SRV.wPort};
                } else if (r->wType == DNS_TYPE_A) {
                    wchar_t address[64];
                    if (InetNtopW(AF_INET, &r->Data.A.IpAddress, address, 64)) self->addresses[name] = text::toUtf8(address);
                }
            }
            for (const auto& [name, service] : self->services) {
                auto id = self->ids.find(name); auto address = self->addresses.find(service.first);
                if (id != self->ids.end() && proto::isValidDeviceId(id->second)) {
                    auto found = self->found;
                    auto host = address != self->addresses.end() ? address->second : text::toUtf8(service.first);
                    self->sched->post([found, device = id->second, ep = link::LanEndpoint{host, service.second, "mdns"}] { found(device, ep); });
                }
            }
            // Bound untrusted multicast input.
            if (self->services.size() > 128 || self->ids.size() > 128 || self->addresses.size() > 256) {
                self->services.clear(); self->ids.clear(); self->addresses.clear();
            }
        }
        if (records) DnsRecordListFree(records, DnsFreeRecordList);
    }
};
ServiceDiscovery::ServiceDiscovery(IScheduler& sched, Found found) : state_(new State) {
    state_->sched = &sched; state_->found = std::move(found);
    DNS_SERVICE_BROWSE_REQUEST request{};
    request.Version = DNS_QUERY_REQUEST_VERSION1; request.QueryName = L"_mediaduet._tcp.local";
    request.pBrowseCallback = State::callback; request.pQueryContext = state_;
    state_->pending = DnsServiceBrowse(&request, &state_->cancel) == DNS_REQUEST_PENDING;
}
ServiceDiscovery::~ServiceDiscovery() {
    { std::lock_guard lock(state_->mutex); state_->found = {}; }
    if (state_->pending) DnsServiceBrowseCancel(&state_->cancel);
    // DNS cancellation is asynchronous. Retain the small callback context until
    // process exit so a final callback cannot reference a destroyed scheduler.
    // There is exactly one discovery operation for this service process.
}
}
