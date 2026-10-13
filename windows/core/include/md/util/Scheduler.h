#pragma once

#include <cstdint>
#include <functional>

namespace md {

using TimerId = uint64_t;

// Single-threaded executor abstraction. All core logic runs on the scheduler's
// thread; platform callbacks marshal back with post(). Production uses a Win32
// message-loop scheduler, tests use ManualScheduler with virtual time.
class IScheduler {
public:
    virtual ~IScheduler() = default;
    // Monotonic milliseconds (never jumps with wall-clock changes).
    virtual int64_t nowMs() const = 0;
    // Wall clock, milliseconds since the Unix epoch (informational only).
    virtual int64_t wallMs() const = 0;
    // Thread-safe: run fn on the scheduler thread as soon as possible.
    virtual void post(std::function<void()> fn) = 0;
    // Scheduler thread only.
    virtual TimerId schedule(int64_t delayMs, std::function<void()> fn) = 0;
    virtual void cancel(TimerId id) = 0;
};

// RAII holder that cancels its timer when replaced or destroyed.
class ScopedTimer {
public:
    ScopedTimer() = default;
    explicit ScopedTimer(IScheduler* s) : s_(s) {}
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
    ~ScopedTimer() { cancel(); }

    void bind(IScheduler* s) { s_ = s; }
    void start(int64_t delayMs, std::function<void()> fn) {
        cancel();
        id_ = s_->schedule(delayMs, [this, f = std::move(fn)] {
            id_ = 0;
            f();
        });
    }
    void cancel() {
        if (id_ && s_) s_->cancel(id_);
        id_ = 0;
    }
    bool active() const { return id_ != 0; }

private:
    IScheduler* s_ = nullptr;
    TimerId id_ = 0;
};

} // namespace md
