#pragma once

#include "md/link/Transport.h"
#include "md/util/Scheduler.h"

#include <winrt/Windows.Networking.Sockets.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

namespace md::platform {

// ITransport over a connected WinRT StreamSocket (Bluetooth RFCOMM or TCP).
// I/O runs on the thread pool; listener callbacks are marshalled to the scheduler.
class StreamTransport final : public link::ITransport, public std::enable_shared_from_this<StreamTransport> {
public:
    StreamTransport(IScheduler& sched, winrt::Windows::Networking::Sockets::StreamSocket socket,
                    link::TransportKind kind, std::string description, std::string remoteHost);
    ~StreamTransport() override;

    link::TransportKind kind() const override { return kind_; }
    std::string describe() const override { return desc_; }
    std::string remoteHost() const override { return host_; }
    void start(Listener* listener) override;
    void send(Bytes data) override;
    size_t pendingWrites() const override { return pending_.load(); }
    void close() override;

private:
    void readLoop();
    void pumpWrites();
    void reportClosed(const std::string& reason);

    IScheduler& sched_;
    winrt::Windows::Networking::Sockets::StreamSocket socket_;
    link::TransportKind kind_;
    std::string desc_;
    std::string host_;
    Listener* listener_ = nullptr; // scheduler thread only
    std::mutex m_;
    std::deque<Bytes> queue_;
    bool writing_ = false;
    std::atomic<size_t> pending_{0};
    std::atomic<bool> closed_{false};
    std::atomic<bool> reported_{false};
};

} // namespace md::platform
