#pragma once

#include "md/protocol/Envelope.h"
#include "md/util/Scheduler.h"

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>

namespace md::link {

// Remembers the highest peer id and the last `capacity` ids (spec §7).
class DedupWindow {
public:
    explicit DedupWindow(size_t capacity = 1024) : cap_(capacity) {}
    // Returns true if the id is new (and records it).
    bool accept(uint64_t id);
    void reset();
    uint64_t highest() const { return highest_; }

private:
    size_t cap_;
    uint64_t highest_ = 0;
    std::set<uint64_t> seen_;
};

struct CommandOutcome {
    bool ok = false;
    std::string code;
    std::string message;
    int64_t latencyMs = 0;
};

// Logical session state shared by all transports of one peer (spec §7).
class LinkSession {
public:
    using ResultFn = std::function<void(const CommandOutcome&)>;

    LinkSession(IScheduler& sched, std::string selfId);

    // Adopts a new sid, failing pending commands and clearing peer state.
    void reset(const std::string& sid);
    const std::string& sid() const { return sid_; }

    proto::Envelope make(std::string type, proto::json body, std::optional<uint64_t> re = std::nullopt);

    // Outgoing commands awaiting media.command.result.
    void track(const proto::Envelope& cmd, ResultFn fn, int64_t timeoutMs = 5000);
    // Commands younger than maxAgeMs that should be re-sent after a transport switch.
    std::vector<proto::Envelope> resendable(int64_t maxAgeMs = 5000) const;
    void failAll(const std::string& code);
    // Returns true if `re` matched a pending command.
    bool completeCommand(const proto::Envelope& result);

    // Incoming de-duplication.
    bool acceptIncoming(const proto::Envelope& e) { return dedup_.accept(e.id); }
    void cacheResult(uint64_t requestId, const proto::Envelope& result);
    const proto::Envelope* cachedResult(uint64_t requestId) const;

    size_t pendingCount() const { return pending_.size(); }

private:
    struct Pending {
        proto::Envelope env;
        int64_t sentAt;
        ResultFn fn;
        TimerId timer;
    };
    void finishPending(uint64_t id, CommandOutcome outcome);

    IScheduler& sched_;
    std::string self_;
    std::string sid_;
    uint64_t nextId_ = 1;
    DedupWindow dedup_;
    std::map<uint64_t, Pending> pending_;
    std::unordered_map<uint64_t, proto::Envelope> results_;
    std::deque<uint64_t> resultOrder_;
};

} // namespace md::link
