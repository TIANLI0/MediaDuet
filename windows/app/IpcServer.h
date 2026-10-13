#pragma once

#include "md/util/Scheduler.h"

#include <windows.h>

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace md::app {

// Named-pipe JSON-lines server for the WinUI process (docs/ipc.md).
// One client at a time, current-user-only DACL, overlapped I/O on one thread.
class IpcServer {
public:
    struct Callbacks {
        std::function<void()> onConnected;
        std::function<void()> onDisconnected;
        std::function<void(std::string line)> onLine;
    };
    IpcServer(IScheduler& sched, Callbacks cb);
    ~IpcServer();

    static std::wstring pipeName();
    bool start();
    void stop();
    // Thread-safe; dropped if no client is connected.
    void send(std::string line);
    bool connected() const { return connected_; }

private:
    void run();

    IScheduler& sched_;
    Callbacks cb_;
    std::thread thread_;
    HANDLE stopEvent_ = nullptr;
    HANDLE sendEvent_ = nullptr;
    std::mutex m_;
    std::deque<std::string> outbox_;
    std::atomic<bool> connected_{false};
};

} // namespace md::app
