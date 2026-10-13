#pragma once

// Deterministic scheduler with virtual time. Used by unit tests only.

#include "md/util/Scheduler.h"

#include <map>
#include <mutex>
#include <vector>

namespace md {

class ManualScheduler final : public IScheduler {
public:
    int64_t nowMs() const override { return now_; }
    int64_t wallMs() const override { return 1'760'000'000'000 + now_; }

    void post(std::function<void()> fn) override {
        std::lock_guard lock(m_);
        posted_.push_back(std::move(fn));
    }
    TimerId schedule(int64_t delayMs, std::function<void()> fn) override {
        TimerId id = ++nextId_;
        timers_.emplace(std::make_pair(now_ + (delayMs < 0 ? 0 : delayMs), id), std::move(fn));
        return id;
    }
    void cancel(TimerId id) override {
        for (auto it = timers_.begin(); it != timers_.end(); ++it)
            if (it->first.second == id) {
                timers_.erase(it);
                return;
            }
    }

    // Runs posted tasks and due timers until idle.
    void runPending() {
        for (;;) {
            std::vector<std::function<void()>> batch;
            {
                std::lock_guard lock(m_);
                batch.swap(posted_);
            }
            bool ran = !batch.empty();
            for (auto& f : batch) f();
            auto it = timers_.begin();
            if (it != timers_.end() && it->first.first <= now_) {
                auto fn = std::move(it->second);
                timers_.erase(it);
                fn();
                ran = true;
            }
            if (!ran) return;
        }
    }
    // Advances virtual time, firing timers in order.
    void advance(int64_t ms) {
        int64_t target = now_ + ms;
        runPending();
        while (!timers_.empty() && timers_.begin()->first.first <= target) {
            now_ = std::max(now_, timers_.begin()->first.first);
            auto fn = std::move(timers_.begin()->second);
            timers_.erase(timers_.begin());
            fn();
            runPending();
        }
        now_ = target;
        runPending();
    }
    size_t pendingTimers() const { return timers_.size(); }

private:
    int64_t now_ = 0;
    TimerId nextId_ = 0;
    std::map<std::pair<int64_t, TimerId>, std::function<void()>> timers_;
    std::mutex m_;
    std::vector<std::function<void()>> posted_;
};

} // namespace md
