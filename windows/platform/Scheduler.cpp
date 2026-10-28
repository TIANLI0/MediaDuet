#include "Scheduler.h"

#include "md/util/Log.h"

#include <stdexcept>

namespace md::platform {

namespace {
constexpr UINT kMsgPosted = WM_APP + 0x101;
constexpr UINT_PTR kTimerId = 1;
constexpr wchar_t kClass[] = L"MediaDuet.Scheduler";
} // namespace

Win32Scheduler::Win32Scheduler() : thread_(GetCurrentThreadId()) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hwnd_) throw std::runtime_error("cannot create scheduler window");
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
}

Win32Scheduler::~Win32Scheduler() {
    if (hwnd_) {
        KillTimer(hwnd_, kTimerId);
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
}

// Interrupt time: the time base of GetTickCount64 (since boot, including sleep), but precise to well
// under a millisecond instead of 15.6 ms ticks; ping timestamps and snapshot anchors need that (§11.1).
int64_t Win32Scheduler::nowMs() const {
    using Fn = VOID(WINAPI*)(PULONGLONG);
    static const Fn precise = [] {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        return kb ? reinterpret_cast<Fn>(GetProcAddress(kb, "QueryInterruptTimePrecise")) : nullptr;
    }();
    if (!precise) return static_cast<int64_t>(GetTickCount64());
    ULONGLONG t = 0;
    precise(&t);
    return static_cast<int64_t>(t / 10'000);
}

int64_t Win32Scheduler::wallMs() const {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u{ft.dwLowDateTime, ft.dwHighDateTime};
    return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

void Win32Scheduler::post(std::function<void()> fn) {
    bool notify = false;
    {
        std::lock_guard lock(m_);
        posted_.push_back(std::move(fn));
        if (!postPending_) {
            postPending_ = true;
            notify = true;
        }
    }
    if (notify) PostMessageW(hwnd_, kMsgPosted, 0, 0);
}

TimerId Win32Scheduler::schedule(int64_t delayMs, std::function<void()> fn) {
    TimerId id = ++nextId_;
    timers_.emplace(nowMs() + (delayMs < 0 ? 0 : delayMs), std::make_pair(id, std::move(fn)));
    rearm();
    return id;
}

void Win32Scheduler::cancel(TimerId id) {
    for (auto it = timers_.begin(); it != timers_.end(); ++it) {
        if (it->second.first == id) {
            timers_.erase(it);
            rearm();
            return;
        }
    }
}

void Win32Scheduler::rearm() {
    if (timers_.empty()) {
        KillTimer(hwnd_, kTimerId);
        return;
    }
    int64_t delay = timers_.begin()->first - nowMs();
    if (delay < USER_TIMER_MINIMUM) delay = USER_TIMER_MINIMUM;
    if (delay > 0x7FFFFFF0) delay = 0x7FFFFFF0;
    SetTimer(hwnd_, kTimerId, static_cast<UINT>(delay), nullptr);
}

void Win32Scheduler::drainPosted() {
    std::deque<std::function<void()>> batch;
    {
        std::lock_guard lock(m_);
        batch.swap(posted_);
        postPending_ = false;
    }
    for (auto& f : batch) {
        try {
            f();
        } catch (const std::exception& e) {
            log::error("sched", "posted task threw: {}", e.what());
        } catch (...) {
            log::error("sched", "posted task threw an unknown exception");
        }
    }
}

void Win32Scheduler::flushPosted() {
    for (;;) {
        { std::lock_guard lock(m_); if (posted_.empty()) return; }
        drainPosted();
    }
}

void Win32Scheduler::runTimers() {
    int64_t now = nowMs();
    // Collect due timers first: callbacks may schedule or cancel others.
    while (!timers_.empty() && timers_.begin()->first <= now) {
        auto fn = std::move(timers_.begin()->second.second);
        timers_.erase(timers_.begin());
        try {
            fn();
        } catch (const std::exception& e) {
            log::error("sched", "timer threw: {}", e.what());
        } catch (...) {
            log::error("sched", "timer threw an unknown exception");
        }
    }
    rearm();
}

LRESULT CALLBACK Win32Scheduler::WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<Win32Scheduler*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) {
        if (msg == kMsgPosted) {
            self->drainPosted();
            return 0;
        }
        if (msg == WM_TIMER && w == kTimerId) {
            self->runTimers();
            return 0;
        }
    }
    return DefWindowProcW(h, msg, w, l);
}

} // namespace md::platform
