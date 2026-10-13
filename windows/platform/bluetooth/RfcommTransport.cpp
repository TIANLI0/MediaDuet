#include "RfcommTransport.h"

#include "../Stores.h"
#include "../network/StreamTransport.h"

#include "md/util/Log.h"
#include "md/util/Text.h"

#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.System.Threading.h>

namespace md::platform {

using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Rfcomm;
using namespace winrt::Windows::Networking::Sockets;

winrt::guid rfcommServiceGuid() { return winrt::guid(std::wstring_view(kRfcommServiceUuid)); }

namespace {
constexpr std::chrono::seconds kConnectTimeout{15};
}

void connectRfcomm(IScheduler& sched, uint64_t btAddress, link::ITransportFactory::Done done) {
    [](IScheduler& sched, uint64_t addr, link::ITransportFactory::Done done) -> fire_and_forget {
        co_await resume_background();
        auto fail = [&](std::string why) { sched.post([done, why] { done(nullptr, why); }); };
        try {
            auto dev = co_await BluetoothDevice::FromBluetoothAddressAsync(addr);
            if (!dev) {
                fail("Bluetooth device not found (not paired with Windows?)");
                co_return;
            }
            auto sid = RfcommServiceId::FromUuid(rfcommServiceGuid());
            auto res = co_await dev.GetRfcommServicesForIdAsync(sid, BluetoothCacheMode::Cached);
            if (res.Services().Size() == 0) res = co_await dev.GetRfcommServicesForIdAsync(sid, BluetoothCacheMode::Uncached);
            if (res.Error() != BluetoothError::Success && res.Services().Size() == 0) {
                fail("SDP query failed (error " + std::to_string(static_cast<int>(res.Error())) + ")");
                co_return;
            }
            if (res.Services().Size() == 0) {
                fail("MediaDuet is not listening on the phone");
                co_return;
            }
            auto svc = res.Services().GetAt(0);
            StreamSocket sock;
            auto op = sock.ConnectAsync(svc.ConnectionHostName(), svc.ConnectionServiceName(),
                                        SocketProtectionLevel::BluetoothEncryptionAllowNullAuthentication);
            auto timer = Windows::System::Threading::ThreadPoolTimer::CreateTimer(
                [op](auto&&) {
                    try {
                        op.Cancel();
                    } catch (...) {
                    }
                },
                kConnectTimeout);
            co_await op;
            timer.Cancel();
            auto t = std::make_shared<StreamTransport>(sched, sock, link::TransportKind::Bluetooth,
                                                       "bt:" + formatBtAddress(addr), std::string());
            sched.post([done, t] { done(t, ""); });
        } catch (hresult_canceled const&) {
            fail("Bluetooth connect timed out");
        } catch (hresult_error const& e) {
            fail(text::toUtf8(e.message()));
        } catch (std::exception const& e) {
            fail(e.what());
        }
    }(sched, btAddress, std::move(done));
}

} // namespace md::platform
