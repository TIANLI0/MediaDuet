#pragma once

#include "md/link/ConnectionManager.h"
#include "md/util/Scheduler.h"

#include <winrt/base.h>

namespace md::platform {

// MediaDuet RFCOMM service (protocol/specification.md §1.1).
inline constexpr wchar_t kRfcommServiceUuid[] = L"{5f3a8c2e-7d41-4b9a-9c61-2e8d0f6b4a17}";
winrt::guid rfcommServiceGuid();

// Connects to the phone's MediaDuet RFCOMM service (Windows is the client).
// `done` runs on the scheduler thread.
void connectRfcomm(IScheduler& sched, uint64_t btAddress, link::ITransportFactory::Done done);

} // namespace md::platform
