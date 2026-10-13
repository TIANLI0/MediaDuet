#include "LanTransport.h"

#include "StreamTransport.h"
#include "../bluetooth/RfcommTransport.h"

#include "md/util/Text.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.System.Threading.h>

namespace md::platform {

using namespace winrt;
using namespace winrt::Windows::Networking;
using namespace winrt::Windows::Networking::Sockets;

namespace {
constexpr std::chrono::seconds kConnectTimeout{6};
}

void connectTcp(IScheduler& sched, const link::LanEndpoint& ep, link::ITransportFactory::Done done) {
    [](IScheduler& sched, link::LanEndpoint ep, link::ITransportFactory::Done done) -> fire_and_forget {
        co_await resume_background();
        auto fail = [&](std::string why) { sched.post([done, why] { done(nullptr, why); }); };
        try {
            StreamSocket sock;
            sock.Control().NoDelay(true);
            sock.Control().KeepAlive(true);
            auto op = sock.ConnectAsync(HostName(text::toWide(ep.host)), std::to_wstring(ep.port));
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
            auto t = std::make_shared<StreamTransport>(sched, sock, link::TransportKind::Lan, "lan:" + ep.str(),
                                                       ep.host);
            sched.post([done, t] { done(t, ""); });
        } catch (hresult_canceled const&) {
            fail("LAN connect to " + ep.str() + " timed out");
        } catch (hresult_error const& e) {
            fail(ep.str() + ": " + text::toUtf8(e.message()));
        }
    }(sched, ep, std::move(done));
}

void TransportFactory::connectBluetooth(const link::PeerRecord& peer, Done done) {
    connectRfcomm(sched_, peer.btAddress, std::move(done));
}

void TransportFactory::connectLan(const link::LanEndpoint& ep, Done done) { connectTcp(sched_, ep, std::move(done)); }

} // namespace md::platform
