#pragma once

#include "md/util/Scheduler.h"

#include <windows.h>

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <thread>

namespace md::app {

std::wstring currentUserSid();

// Named-pipe JSON-lines server for the WinUI process (docs/ipc.md).
// One client at a time, current-user-only DACL, overlapped I/O on one thread.
class IpcServer {
public:
    struct Callbacks {
        std::function<void()> onConnected;
        std::function<void()> onDisconnected;
        std::function<void(const std::string& line)> onLine;
    };
    // Another pipe than the UI's (the credential provider's, docs/unlock.md).
    struct Options {
        std::wstring name;
        std::wstring sddl;
        // Runs on the pipe thread once the client sent its first bytes (so it can be
        // impersonated); false disconnects it before any line is delivered.
        std::function<bool(HANDLE pipe)> verifyClient;
        // Optional server-side metadata capture, on the pipe thread. Bind it to
        // the individual line rather than mutable state of the next connection.
        std::function<void(std::string& line, HANDLE pipe)> prepareLine;
    };
    IpcServer(IScheduler& sched, Callbacks cb);
    IpcServer(IScheduler& sched, Callbacks cb, Options opts);
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
    Options opts_;
    std::thread thread_;
    HANDLE stopEvent_ = nullptr;
    HANDLE sendEvent_ = nullptr;
    std::mutex m_;
    std::deque<std::string> outbox_;
    std::atomic<bool> connected_{false};
    std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
};

} // namespace md::app
