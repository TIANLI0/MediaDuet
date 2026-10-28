#pragma once

#include "md/util/Scheduler.h"

#include <windows.h>

#include <deque>
#include <map>
#include <mutex>

namespace md::platform {

// IScheduler on top of the Win32 message loop of the thread that creates it.
// Idle cost is zero: one message-only window and a single coalesced WM_TIMER
// for the earliest deadline.
class Win32Scheduler final : public IScheduler {
public:
    Win32Scheduler();
    ~Win32Scheduler() override;

    int64_t nowMs() const override;
    int64_t wallMs() const override;
    void post(std::function<void()> fn) override;
    TimerId schedule(int64_t delayMs, std::function<void()> fn) override;
    void cancel(TimerId id) override;

    DWORD threadId() const { return thread_; }
    // Owner thread only; used after service workers have stopped, before tearing
    // down the message window, to release queued transport/connection callbacks.
    void flushPosted();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void drainPosted();
    void runTimers();
    void rearm();

    HWND hwnd_ = nullptr;
    DWORD thread_ = 0;
    std::mutex m_;
    std::deque<std::function<void()>> posted_;
    bool postPending_ = false;
    TimerId nextId_ = 0;
    std::multimap<int64_t, std::pair<TimerId, std::function<void()>>> timers_;
};

} // namespace md::platform
