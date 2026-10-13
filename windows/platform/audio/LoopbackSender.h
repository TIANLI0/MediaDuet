#pragma once

#include "md/util/Bytes.h"
#include "md/util/Scheduler.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace md::platform {

// EXPERIMENTAL: streams this PC's mixed output (WASAPI loopback) to the phone as
// PCM s16le 48 kHz stereo over an encrypted TCP stream (spec §9). Threads, buffers
// and the audio client exist only while streaming. Protected (DRM) audio is
// rendered silent by Windows itself and is never captured.
class LoopbackSender {
public:
    struct Status {
        std::string state = "off"; // off | connecting | streaming | error
        std::string error;
        uint64_t framesSent = 0;
        uint64_t framesDropped = 0;
    };
    LoopbackSender(IScheduler& sched, std::function<void()> changed);
    ~LoopbackSender();

    void start(const std::string& host, uint16_t port, Bytes streamId, Bytes audioKey, Bytes audioIv);
    void stop(const std::string& reason = {});
    Status status() const;
    bool running() const { return thread_.joinable(); }

private:
    void run(std::string host, uint16_t port, Bytes streamId, Bytes key, Bytes iv);
    void report(std::string state, std::string error = {});

    IScheduler& sched_;
    std::function<void()> changed_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> sent_{0}, dropped_{0};
    std::string state_ = "off";
    std::string error_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace md::platform
