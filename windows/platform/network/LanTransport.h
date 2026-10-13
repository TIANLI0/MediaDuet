#pragma once

#include "md/link/ConnectionManager.h"
#include "md/util/Scheduler.h"

namespace md::platform {

inline constexpr uint16_t kDefaultLanPort = 47321;

void connectTcp(IScheduler& sched, const link::LanEndpoint& ep, link::ITransportFactory::Done done);

// Production transport factory: RFCOMM + TCP.
class TransportFactory final : public link::ITransportFactory {
public:
    explicit TransportFactory(IScheduler& sched) : sched_(sched) {}
    void connectBluetooth(const link::PeerRecord& peer, Done done) override;
    void connectLan(const link::LanEndpoint& ep, Done done) override;

private:
    IScheduler& sched_;
};

} // namespace md::platform
