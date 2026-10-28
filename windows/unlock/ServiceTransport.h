#pragma once
#include "md/link/ConnectionManager.h"
#include <atomic>
#include <thread>
#include <future>
#include <mutex>

namespace md::unlock {
// Native Winsock transports work in session 0 without a user profile or WinRT UI.
class ServiceTransportFactory final : public link::ITransportFactory {
public:
    explicit ServiceTransportFactory(IScheduler& sched) : sched_(sched) {}
    ~ServiceTransportFactory();
    void stop();
    void connectBluetooth(const link::PeerRecord& peer, Done done) override;
    void connectLan(const link::LanEndpoint& endpoint, Done done) override;
private:
    void connect(std::string host, uint16_t port, uint64_t bluetooth, Done done);
    IScheduler& sched_;
    std::shared_ptr<std::atomic_bool> running_ = std::make_shared<std::atomic_bool>(true);
    std::vector<std::future<void>> workers_;
    std::mutex transportsMutex_;
    std::vector<std::weak_ptr<link::ITransport>> transports_;
};
class ServiceDiscovery {
public:
    using Found = std::function<void(std::string, link::LanEndpoint)>;
    ServiceDiscovery(IScheduler& sched, Found found);
    ~ServiceDiscovery();
private:
    struct State;
    State* state_;
};
}
