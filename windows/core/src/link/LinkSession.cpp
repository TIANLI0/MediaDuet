#include "md/link/LinkSession.h"

namespace md::link {

namespace {
constexpr size_t kResultCache = 256;
}

LinkSession::LinkSession(IScheduler& sched, std::string selfId) : sched_(sched), self_(std::move(selfId)) {}

void LinkSession::reset(const std::string& sid) {
    failAll("link_lost");
    sid_ = sid;
    // Ids restart per session (spec §6: per sender, per link session).
    nextId_ = 1;
    dedup_.reset();
    results_.clear();
    resultOrder_.clear();
}

proto::Envelope LinkSession::make(std::string type, proto::json body, std::optional<uint64_t> re) {
    proto::Envelope e;
    e.type = std::move(type);
    e.id = nextId_++;
    e.ts = sched_.wallMs();
    e.src = self_;
    e.sid = sid_;
    e.re = re;
    e.body = std::move(body);
    return e;
}

void LinkSession::track(const proto::Envelope& cmd, ResultFn fn, int64_t timeoutMs) {
    uint64_t id = cmd.id;
    TimerId t = sched_.schedule(timeoutMs, [this, id] {
        auto it = pending_.find(id);
        if (it == pending_.end()) return;
        it->second.timer = 0;
        finishPending(id, {false, "timeout", "no response from peer", sched_.nowMs() - it->second.sentAt});
    });
    pending_[id] = Pending{cmd, sched_.nowMs(), std::move(fn), t};
}

std::vector<proto::Envelope> LinkSession::resendable(int64_t maxAgeMs) const {
    std::vector<proto::Envelope> out;
    int64_t now = sched_.nowMs();
    for (auto& [id, p] : pending_)
        if (now - p.sentAt <= maxAgeMs) out.push_back(p.env);
    return out;
}

void LinkSession::finishPending(uint64_t id, CommandOutcome outcome) {
    auto it = pending_.find(id);
    if (it == pending_.end()) return;
    auto fn = std::move(it->second.fn);
    if (it->second.timer) sched_.cancel(it->second.timer);
    pending_.erase(it);
    if (fn) fn(outcome);
}

void LinkSession::failAll(const std::string& code) {
    std::vector<uint64_t> ids;
    for (auto& [id, p] : pending_) ids.push_back(id);
    for (auto id : ids) finishPending(id, {false, code, "link lost before the command completed", 0});
}

bool LinkSession::completeCommand(const proto::Envelope& result) {
    if (!result.re) return false;
    auto it = pending_.find(*result.re);
    if (it == pending_.end()) return false;
    CommandOutcome o;
    o.ok = result.body.value("ok", false);
    if (auto e = result.body.find("error"); e != result.body.end() && e->is_object()) {
        o.code = e->value("code", std::string());
        o.message = e->value("message", std::string());
    }
    o.latencyMs = sched_.nowMs() - it->second.sentAt;
    finishPending(*result.re, std::move(o));
    return true;
}

void LinkSession::cacheResult(uint64_t requestId, const proto::Envelope& result) {
    if (results_.emplace(requestId, result).second) {
        resultOrder_.push_back(requestId);
        while (resultOrder_.size() > kResultCache) {
            results_.erase(resultOrder_.front());
            resultOrder_.pop_front();
        }
    }
}

const proto::Envelope* LinkSession::cachedResult(uint64_t requestId) const {
    auto it = results_.find(requestId);
    return it == results_.end() ? nullptr : &it->second;
}

} // namespace md::link
