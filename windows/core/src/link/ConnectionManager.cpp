#include "md/link/ConnectionManager.h"

#include "md/util/Log.h"

#include <algorithm>
#include <cmath>

namespace md::link {

namespace {
constexpr int64_t kStableLifetimeMs = 30'000;     // a link that lived this long resets back-off
constexpr int64_t kFlapWindowMs = 10 * 60'000;
constexpr int64_t kMaxStabilityMs = 5 * 60'000;
constexpr int kMaxAuthFailures = 3;
} // namespace

std::string LanEndpoint::str() const {
    bool v6 = host.find(':') != std::string::npos;
    return (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

const char* toString(ConnMode m) {
    switch (m) {
    case ConnMode::BluetoothOnly: return "bt_only";
    case ConnMode::LanOnly: return "lan_only";
    default: return "auto";
    }
}

ConnMode connModeFromString(std::string_view s) {
    if (s == "bt_only") return ConnMode::BluetoothOnly;
    if (s == "lan_only") return ConnMode::LanOnly;
    return ConnMode::Auto;
}

ConnectionManager::ConnectionManager(IScheduler& sched, ITransportFactory& factory, Listener& listener,
                                     proto::DeviceInfo self, std::vector<std::string> caps, Config cfg)
    : sched_(sched), factory_(factory), listener_(listener), self_(std::move(self)), caps_(std::move(caps)),
      cfg_(cfg) {}

ConnectionManager::~ConnectionManager() {
    *alive_ = false;
    for (auto& [id, p] : peers_) {
        p->bt.retry.cancel();
        p->lan.retry.cancel();
        p->bt.heartbeat.cancel();
        p->lan.heartbeat.cancel();
        p->promote.cancel();
        if (p->bt.conn) p->bt.conn->close("shutdown");
        if (p->lan.conn) p->lan.conn->close("shutdown");
        closePending(p->lan, "shutdown");
    }
}

ConnectionManager::Peer* ConnectionManager::find(const std::string& id) {
    auto it = peers_.find(id);
    return it == peers_.end() ? nullptr : it->second.get();
}

const ConnectionManager::Peer* ConnectionManager::activePeerPtr() const {
    for (auto& [id, p] : peers_)
        if (p->primary) return p.get();
    return nullptr;
}

bool ConnectionManager::allowed(TransportKind k) const {
    if (cfg_.mode == ConnMode::BluetoothOnly) return k == TransportKind::Bluetooth;
    if (cfg_.mode == ConnMode::LanOnly) return k == TransportKind::Lan;
    return true;
}

bool ConnectionManager::otherPeerActive(const Peer& p) const {
    for (auto& [id, q] : peers_) {
        if (q.get() == &p) continue;
        if (q->primary || (q->bt.conn && q->bt.conn->state() == Connection::State::Up) ||
            (q->lan.conn && q->lan.conn->state() == Connection::State::Up))
            return true;
    }
    return false;
}

std::vector<LanEndpoint> ConnectionManager::lanCandidates(const Peer& p) const {
    int64_t now = sched_.nowMs();
    std::vector<LanEndpoint> ranked;
    auto add = [&](const LanEndpoint& e) {
        if (e.port && !e.host.empty() && std::find(ranked.begin(), ranked.end(), e) == ranked.end()) ranked.push_back(e);
    };
    auto info = [&](const LanEndpoint& e) -> const Peer::EpInfo* {
        auto it = p.eps.find(e.str());
        return it == p.eps.end() ? nullptr : &it->second;
    };
    // The phone's own report of its current addresses comes first: on networks where it
    // changes address often (and DNS-SD does not get through) the last good / manual address
    // is usually the stale one. Peer-reported addresses (VPN, cellular, virtual interfaces
    // ...) are only trusted for a while unless one of them actually worked; a candidate that
    // never handshakes is raced and demoted (see below), so trying them first costs little.
    auto fresh = [&](const LanEndpoint& e) {
        const auto* i = info(e);
        return i && i->reportedAt >= 0 && now - i->reportedAt <= cfg_.peerEndpointTtlMs;
    };
    auto confirmed = [&](const LanEndpoint& e) {
        const auto* i = info(e);
        return i && i->lastUp >= 0 && now - i->lastUp <= cfg_.peerEndpointTtlMs;
    };
    for (auto& e : p.reported)
        if (fresh(e)) add(e);
    if (p.lastGood) add(*p.lastGood);
    for (auto& e : p.mdns) add(e);
    for (auto& e : p.rec.manualEndpoints) add(e);
    for (auto& e : p.reported)
        if (confirmed(e)) add(e);
    std::vector<LanEndpoint> ok, cooling;
    for (auto& e : ranked) {
        const auto* i = info(e);
        (i && i->coolUntil > now ? cooling : ok).push_back(e);
    }
    if (ok.empty()) return cooling;
    return ok;
}

void ConnectionManager::closePending(Slot& s, const std::string& why) {
    // Detach first: their onClosed callbacks are then ignored (unknown connection).
    auto losers = std::move(s.pending);
    s.pending.clear();
    for (auto& l : losers) l.conn->close(why);
}

void ConnectionManager::lanAdvance(Peer& p, Slot& s, int64_t delay) {
    if (s.conn || p.revoked || p.userDisconnected || !allowed(s.kind) || s.retry.active()) return;
    std::string peerId = p.rec.id;
    if (s.endpointCursor < s.cycle.size()) {
        s.retry.start(delay, [this, peerId] {
            if (Peer* q = find(peerId)) evaluate(*q);
        });
        return;
    }
    if (s.inFlight > 0) return; // the last connect of this cycle decides
    // Cycle exhausted. Handshakes that are still running do not count as failures
    // yet, but they do not hold back the next cycle either.
    s.endpointCursor = 0;
    s.cycle.clear();
    if (s.pending.empty()) ++s.failures;
    scheduleRetry(p, s);
}

void ConnectionManager::attemptLan(Peer& p) {
    Slot& s = p.lan;
    if (s.endpointCursor == 0 || s.endpointCursor >= s.cycle.size()) {
        s.cycle.clear();
        s.endpointCursor = 0;
        for (auto& e : lanCandidates(p))
            if (std::none_of(s.pending.begin(), s.pending.end(), [&](const Slot::Pending& x) { return x.ep == e; }))
                s.cycle.push_back(e);
        if (s.cycle.empty()) return; // everything is already in a handshake
    }
    LanEndpoint ep = s.cycle[s.endpointCursor++];
    ++s.inFlight;
    log::info("link", "connecting to {} over LAN {} ({}, attempt {}, candidate {}/{})", p.rec.name, ep.str(), ep.source,
              s.failures + 1, s.endpointCursor, s.cycle.size());
    std::weak_ptr<bool> alive = alive_;
    std::string peerId = p.rec.id;
    factory_.connectLan(ep, [this, alive, peerId, ep](TransportPtr t, std::string err) {
        if (alive.expired()) {
            if (t) t->close();
            return;
        }
        Peer* p = find(peerId);
        if (!p) {
            if (t) t->close();
            return;
        }
        Slot& s = p->lan;
        s.inFlight = std::max(0, s.inFlight - 1);
        if (!t) {
            log::info("link", "lan connect to {} ({}) failed: {}", p->rec.name, ep.str(), err);
            p->lastError = err;
            lanAdvance(*p, s, cfg_.nextEndpointDelayMs);
            listener_.onStatusChanged();
            return;
        }
        if (p->revoked || p->userDisconnected || !allowed(TransportKind::Lan) || otherPeerActive(*p) || s.conn) {
            t->close();
            return;
        }
        Connection::Events ev;
        ev.onUp = [this, alive, peerId](Connection& c, const SessionClient::Established& est) {
            if (alive.expired()) return;
            if (Peer* p = find(peerId)) onConnUp(*p, p->lan, c, est);
        };
        ev.onEnvelope = [this, alive, peerId](Connection& c, proto::Envelope e) {
            if (alive.expired()) return;
            if (Peer* p = find(peerId)) onConnEnvelope(*p, c, std::move(e));
        };
        ev.onBlob = [this, alive, peerId](Connection&, proto::BlobChunk b) {
            if (alive.expired()) return;
            listener_.onBlob(peerId, b);
        };
        ev.onClosed = [this, alive, peerId](Connection& c, const std::string& reason, const std::string& code) {
            if (alive.expired()) return;
            onConnClosed(peerId, TransportKind::Lan, c.id(), reason, code);
        };
        auto conn = Connection::create(sched_, std::move(t), std::move(ev));
        s.pending.push_back({conn, ep});
        log::info("link", "lan transport to {} open ({}), authenticating", p->rec.name, conn->describe());
        conn->beginSession(self_, p->rec.id, p->rec.key, caps_, p->session.sid());
        // Happy eyeballs: give this handshake a head start, then race the next candidate.
        lanAdvance(*p, s, cfg_.raceDelayMs);
        listener_.onStatusChanged();
    });
    listener_.onStatusChanged();
}

// ------------------------------------------------------------------ configuration

void ConnectionManager::setPeers(std::vector<PeerRecord> peers) {
    std::map<std::string, PeerRecord> incoming;
    for (auto& r : peers) incoming[r.id] = std::move(r);
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (!incoming.count(it->first)) {
            auto& p = *it->second;
            p.bt.retry.cancel();
            p.lan.retry.cancel();
            if (p.bt.conn) p.bt.conn->close("peer removed");
            if (p.lan.conn) p.lan.conn->close("peer removed");
            closePending(p.lan, "peer removed");
            bool wasUp = p.primary != nullptr;
            p.primary = nullptr;
            p.session.failAll("link_lost");
            if (wasUp) listener_.onLinkDown(it->first);
            it = peers_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& [id, rec] : incoming) {
        Peer* p = find(id);
        if (!p) {
            auto np = std::make_unique<Peer>(sched_, self_.id);
            np->bt.retry.bind(&sched_);
            np->lan.retry.bind(&sched_);
            np->bt.heartbeat.bind(&sched_);
            np->lan.heartbeat.bind(&sched_);
            np->bt.pongCheck.bind(&sched_);
            np->lan.pongCheck.bind(&sched_);
            np->promote.bind(&sched_);
            np->syncBurst.bind(&sched_);
            np->rec = rec;
            peers_[id] = std::move(np);
            continue;
        }
        bool keyChanged = p->rec.key != rec.key;
        p->rec = rec;
        if (keyChanged) {
            p->revoked = false;
            p->authFailures = 0;
            p->bt.failures = p->lan.failures = 0;
            if (p->bt.conn) p->bt.conn->close("re-paired");
            if (p->lan.conn) p->lan.conn->close("re-paired");
            closePending(p->lan, "re-paired");
        }
    }
    evaluateAll();
    listener_.onStatusChanged();
}

void ConnectionManager::setMode(ConnMode mode) {
    if (cfg_.mode == mode) return;
    cfg_.mode = mode;
    for (auto& [id, p] : peers_) {
        for (Slot* s : {&p->bt, &p->lan}) {
            if (!allowed(s->kind)) {
                s->retry.cancel();
                if (s->conn) s->conn->close("transport disabled by mode");
                closePending(*s, "transport disabled by mode");
            } else {
                s->failures = 0;
            }
        }
        choosePrimary(*p);
    }
    evaluateAll();
    listener_.onStatusChanged();
}

void ConnectionManager::setSwitchBackDelay(int64_t ms) { cfg_.switchBackDelayMs = std::clamp<int64_t>(ms, 0, 600'000); }

void ConnectionManager::setMediaActive(bool active) {
    if (mediaActive_ == active) return;
    mediaActive_ = active;
    for (auto& [id, p] : peers_) {
        for (Slot* s : {&p->bt, &p->lan})
            if (s->conn && s->conn->state() == Connection::State::Up) armHeartbeat(*p, *s);
        if (active) startSyncBurst(*p);
    }
}

// ------------------------------------------------------------------ platform events

void ConnectionManager::onBluetoothRadio(bool on) {
    if (btRadio_ == on) return;
    btRadio_ = on;
    log::info("link", "bluetooth radio {}", on ? "on" : "off");
    for (auto& [id, p] : peers_) {
        p->bt.retry.cancel();
        p->bt.failures = 0;
        if (!on && p->bt.conn) p->bt.conn->close("bluetooth turned off");
    }
    evaluateAll();
    listener_.onStatusChanged();
}

void ConnectionManager::onPeerBluetoothPresence(const std::string& peerId, bool connected) {
    Peer* p = find(peerId);
    if (!p) return;
    bool rising = connected && !p->btPresent;
    p->btPresent = connected;
    if (rising) {
        p->bt.retry.cancel();
        p->bt.failures = 0;
        evaluate(*p);
    }
}

void ConnectionManager::onLanEndpoint(const std::string& peerId, const LanEndpoint& ep, bool available) {
    Peer* p = find(peerId);
    if (!p) return;
    auto it = std::find(p->mdns.begin(), p->mdns.end(), ep);
    if (available) {
        if (it == p->mdns.end()) p->mdns.insert(p->mdns.begin(), ep);
        // A (re-)announcement means the phone app is (back) up: an announced address is
        // not in handshake cool-down, and a pending back-off restarts from the best
        // candidate (a restarted app re-advertises).
        p->eps[ep.str()].coolUntil = -1;
        Slot& s = p->lan;
        int64_t now = sched_.nowMs();
        if (!s.conn && s.inFlight == 0 && (p->announceResetAt < 0 || now - p->announceResetAt >= 5'000)) {
            p->announceResetAt = now;
            s.retry.cancel();
            s.failures = 0;
            s.endpointCursor = 0;
            s.cycle.clear();
            evaluate(*p);
        }
    } else if (it != p->mdns.end()) {
        p->mdns.erase(it);
    }
}

void ConnectionManager::onNetworkChanged() {
    for (auto& [id, p] : peers_) {
        p->lan.retry.cancel();
        p->lan.failures = 0;
        p->lan.endpointCursor = 0;
        p->lan.cycle.clear();
        // Probe the existing LAN link quickly; an address change usually kills it silently.
        if (p->lan.conn && p->lan.conn->state() == Connection::State::Up) {
            p->lan.conn->sendEnvelope(p->session.make(proto::types::Ping, {{"t", sched_.nowMs()}}));
        }
    }
    evaluateAll();
}

void ConnectionManager::connectNow(const std::string& peerId) {
    Peer* p = find(peerId);
    if (!p) return;
    p->userDisconnected = false;
    p->revoked = false;
    for (Slot* s : {&p->bt, &p->lan}) {
        s->retry.cancel();
        s->failures = 0;
    }
    evaluate(*p);
    listener_.onStatusChanged();
}

void ConnectionManager::disconnect(const std::string& peerId) {
    Peer* p = find(peerId);
    if (!p) return;
    p->userDisconnected = true;
    for (Slot* s : {&p->bt, &p->lan}) {
        s->retry.cancel();
        if (s->conn) {
            if (s->conn->state() == Connection::State::Up)
                s->conn->sendEnvelope(p->session.make(proto::types::Bye, {{"reason", "user_disconnect"}}));
            s->conn->close("disconnected by user");
        }
        closePending(*s, "disconnected by user");
    }
    listener_.onStatusChanged();
}

// ------------------------------------------------------------------ connection logic

void ConnectionManager::evaluateAll() {
    // Prefer the peer that is already active; otherwise everyone may try.
    for (auto& [id, p] : peers_) evaluate(*p);
}

void ConnectionManager::evaluate(Peer& p) {
    if (p.revoked || p.userDisconnected || p.rec.key.empty()) return;
    if (otherPeerActive(p)) return;
    for (Slot* s : {&p.bt, &p.lan}) {
        if (!allowed(s->kind) || s->conn || s->connecting || s->retry.active()) continue;
        if (s->kind == TransportKind::Bluetooth) {
            if (!btRadio_ || p.rec.btAddress == 0) continue;
            attempt(p, s->kind);
        } else {
            if (s->inFlight > 0 || lanCandidates(p).empty()) continue;
            attemptLan(p);
        }
    }
}

void ConnectionManager::attempt(Peer& p, TransportKind k) {
    Slot& s = slot(p, k);
    s.connecting = true;
    std::weak_ptr<bool> alive = alive_;
    std::string peerId = p.rec.id;
    auto done = [this, alive, peerId, k](TransportPtr t, std::string err) {
        if (alive.expired()) {
            if (t) t->close();
            return;
        }
        Peer* p = find(peerId);
        if (!p) {
            if (t) t->close();
            return;
        }
        Slot& s = slot(*p, k);
        s.connecting = false;
        if (!t) {
            log::info("link", "{} connect to {} failed: {}", toString(k), p->rec.name, err);
            p->lastError = err;
            ++s.failures;
            scheduleRetry(*p, s);
            listener_.onStatusChanged();
            return;
        }
        if (p->revoked || p->userDisconnected || !allowed(k) || otherPeerActive(*p) || s.conn ||
            (k == TransportKind::Bluetooth && !btRadio_)) {
            t->close();
            return;
        }
        Connection::Events ev;
        ev.onUp = [this, alive, peerId, k](Connection& c, const SessionClient::Established& est) {
            if (alive.expired()) return;
            if (Peer* p = find(peerId)) onConnUp(*p, slot(*p, k), c, est);
        };
        ev.onEnvelope = [this, alive, peerId](Connection& c, proto::Envelope e) {
            if (alive.expired()) return;
            if (Peer* p = find(peerId)) onConnEnvelope(*p, c, std::move(e));
        };
        ev.onBlob = [this, alive, peerId](Connection&, proto::BlobChunk b) {
            if (alive.expired()) return;
            listener_.onBlob(peerId, b);
        };
        ev.onClosed = [this, alive, peerId, k](Connection& c, const std::string& reason, const std::string& code) {
            if (alive.expired()) return;
            onConnClosed(peerId, k, c.id(), reason, code);
        };
        auto conn = Connection::create(sched_, std::move(t), std::move(ev));
        s.conn = conn;
        log::info("link", "{} transport to {} open ({}), authenticating", toString(k), p->rec.name, conn->describe());
        conn->beginSession(self_, p->rec.id, p->rec.key, caps_, p->session.sid());
        listener_.onStatusChanged();
    };
    if (k == TransportKind::Lan) {
        s.connecting = false;
        return attemptLan(p);
    }
    log::info("link", "connecting to {} over Bluetooth (attempt {})", p.rec.name, s.failures + 1);
    factory_.connectBluetooth(p.rec, std::move(done));
    listener_.onStatusChanged();
}

void ConnectionManager::scheduleRetry(Peer& p, Slot& s) {
    if (p.revoked || p.userDisconnected || !allowed(s.kind)) return;
    int64_t base = s.kind == TransportKind::Bluetooth ? cfg_.btBackoffBaseMs : cfg_.lanBackoffBaseMs;
    int64_t cap = s.kind == TransportKind::Bluetooth ? cfg_.btBackoffMaxMs : cfg_.lanBackoffMaxMs;
    if (p.authFailures >= kMaxAuthFailures) base = cap;
    int shift = std::clamp(s.failures - 1, 0, 16);
    int64_t delay = std::min(cap, base << shift);
    std::uniform_real_distribution<double> jitter(0.8, 1.2);
    delay = static_cast<int64_t>(static_cast<double>(delay) * jitter(rng_));
    std::string peerId = p.rec.id;
    TransportKind k = s.kind;
    s.retry.start(delay, [this, peerId, k] {
        if (Peer* p = find(peerId)) {
            (void)k;
            evaluate(*p);
        }
    });
}

void ConnectionManager::onConnUp(Peer& p, Slot& s, Connection& c, const SessionClient::Established& est) {
    if (s.kind == TransportKind::Lan) {
        auto it = std::find_if(s.pending.begin(), s.pending.end(), [&](const Slot::Pending& x) { return x.conn.get() == &c; });
        if (it == s.pending.end()) return; // a loser that finished anyway; already closed
        s.conn = it->conn;
        s.currentEp = it->ep;
        s.pending.erase(it);
        closePending(s, "another LAN address won");
        s.retry.cancel();
    }
    s.upSince = sched_.nowMs();
    p.authFailures = 0;
    if (s.kind == TransportKind::Lan && s.currentEp) {
        auto& info = p.eps[s.currentEp->str()];
        info.lastUp = s.upSince;
        info.coolUntil = -1;
        p.lastGood = s.currentEp;
    }
    s.endpointCursor = 0;
    s.cycle.clear();
    p.caps = est.caps;
    p.lastError.clear();
    if (!est.peer.name.empty()) p.rec.name = est.peer.name;
    if (!est.peer.app.empty()) p.app = est.peer.app;
    bool hadPrimary = p.primary != nullptr;
    bool sessionChanged = false;
    if (p.session.sid() != est.sid) {
        sessionChanged = !p.session.sid().empty();
        // The server does not know our previous session: everything tied to it is gone.
        Slot& other = (&s == &p.bt) ? p.lan : p.bt;
        if (other.conn && other.conn.get() != &c) other.conn->close("session replaced");
        p.primary = nullptr;
        p.session.reset(est.sid);
        p.clock.reset(); // possibly another process (or boot) on the other side
    }
    log::info("link", "{} link to {} up (sid {}…, {})", toString(s.kind), p.rec.name, est.sid.substr(0, 8),
              est.resumed ? "resumed" : "new session");
    armHeartbeat(p, s);
    choosePrimary(p);
    if (!hadPrimary || sessionChanged) listener_.onLinkUp(p.rec.id, est.resumed && !sessionChanged);
    // Other peers stop trying while this one is active.
    for (auto& [id, q] : peers_)
        if (q.get() != &p) {
            q->bt.retry.cancel();
            q->lan.retry.cancel();
        }
    listener_.onStatusChanged();
}

int64_t ConnectionManager::requiredLanStability(Peer& p) {
    int64_t now = sched_.nowMs();
    p.lanDrops.erase(std::remove_if(p.lanDrops.begin(), p.lanDrops.end(),
                                    [&](int64_t t) { return now - t > kFlapWindowMs; }),
                     p.lanDrops.end());
    // No recent LAN failures (e.g. at start-up): LAN wins immediately.
    if (p.lanDrops.empty()) return 0;
    int shift = std::min<int>(static_cast<int>(p.lanDrops.size()) - 1, 5);
    return std::min(kMaxStabilityMs, cfg_.switchBackDelayMs << shift);
}

void ConnectionManager::choosePrimary(Peer& p) {
    auto up = [](const Slot& s) { return s.conn && s.conn->state() == Connection::State::Up; };
    Connection* bt = up(p.bt) ? p.bt.conn.get() : nullptr;
    Connection* lan = up(p.lan) ? p.lan.conn.get() : nullptr;
    Connection* want = nullptr;
    switch (cfg_.mode) {
    case ConnMode::BluetoothOnly: want = bt; break;
    case ConnMode::LanOnly: want = lan; break;
    case ConnMode::Auto:
        // LAN first: Bluetooth RFCOMM adds noticeably more latency (measured on real
        // phones), so it is the fallback; a LAN link that came back after a drop has to
        // stay up for the switch-back delay (longer when it keeps dropping) first.
        if (lan && p.primary == bt && bt) {
            int64_t need = requiredLanStability(p);
            int64_t age = sched_.nowMs() - p.lan.upSince;
            if (age >= need) {
                want = lan;
            } else {
                want = bt;
                std::string peerId = p.rec.id;
                p.promote.start(need - age, [this, peerId] {
                    if (Peer* q = find(peerId)) choosePrimary(*q);
                });
            }
        } else {
            want = lan ? lan : bt;
        }
        break;
    }
    if (want != p.primary) setPrimary(p, want);
}

void ConnectionManager::setPrimary(Peer& p, Connection* c) {
    Connection* old = p.primary;
    bool oldAlive = old && old->state() == Connection::State::Up;
    p.primary = c;
    if (c) {
        log::info("link", "primary transport for {} is now {}", p.rec.name, toString(c->kind()));
        c->sendEnvelope(p.session.make(proto::types::LinkPrimary, proto::json::object()));
        if (!oldAlive) {
            // Same ids: the peer's de-dup window guarantees at-most-once execution.
            for (auto& e : p.session.resendable(cfg_.resendMaxAgeMs)) c->sendEnvelope(e);
        }
        startSyncBurst(p);
    }
    for (Slot* s : {&p.bt, &p.lan})
        if (s->conn && s->conn->state() == Connection::State::Up) armHeartbeat(p, *s);
    listener_.onStatusChanged();
}

int64_t ConnectionManager::pingInterval(const Peer& p, const Slot& s) const {
    if (s.conn.get() != p.primary) return cfg_.pingSecondaryMs;
    return mediaActive_ ? cfg_.pingActiveMs : cfg_.pingIdleMs;
}

void ConnectionManager::armHeartbeat(Peer& p, Slot& s) {
    int64_t interval = pingInterval(p, s);
    std::string peerId = p.rec.id;
    TransportKind k = s.kind;
    uint64_t connId = s.conn->id();
    s.heartbeat.start(interval, [this, peerId, k, connId, interval] {
        Peer* p = find(peerId);
        if (!p) return;
        Slot& s = slot(*p, k);
        if (!s.conn || s.conn->id() != connId || s.conn->state() != Connection::State::Up) return;
        if (sched_.nowMs() - s.conn->lastReceiveMs() > 3 * interval + 5'000) {
            log::warn("link", "{} link to {} silent, closing", toString(k), p->rec.name);
            s.conn->close("heartbeat timeout");
            return;
        }
        s.conn->sendEnvelope(p->session.make(proto::types::Ping, {{"t", sched_.nowMs()}}));
        ++p->msgsOut;
        armPongCheck(*p, s);
        armHeartbeat(*p, s);
    });
}

void ConnectionManager::startSyncBurst(Peer& p) {
    if (cfg_.syncBurstPings <= 0) return;
    p.syncPingsLeft = cfg_.syncBurstPings;
    armSyncBurst(p);
}

void ConnectionManager::armSyncBurst(Peer& p) {
    // Liveness stays with the heartbeat; these pings only feed the clock estimate, so a few in a row
    // let one of them catch a quiet moment on the link.
    std::string peerId = p.rec.id;
    p.syncBurst.start(cfg_.syncBurstGapMs, [this, peerId] {
        Peer* p = find(peerId);
        if (!p || !p->primary || p->primary->state() != Connection::State::Up || p->syncPingsLeft <= 0) return;
        p->primary->sendEnvelope(p->session.make(proto::types::Ping, {{"t", sched_.nowMs()}}));
        ++p->msgsOut;
        if (--p->syncPingsLeft > 0) armSyncBurst(*p);
    });
}

void ConnectionManager::armPongCheck(Peer& p, Slot& s) {
    // Every ping is answered within milliseconds on a live link. No answer at all within
    // kPongWaitMs: probe once more and give up after a second silent wait. A phone that
    // changed its IP is noticed in ~2 x kPongWaitMs instead of after three heartbeats.
    std::string peerId = p.rec.id;
    TransportKind k = s.kind;
    uint64_t connId = s.conn->id();
    int64_t sentAt = sched_.nowMs();
    s.pongCheck.start(kPongWaitMs, [this, peerId, k, connId, sentAt] {
        Peer* p = find(peerId);
        if (!p) return;
        Slot& s = slot(*p, k);
        if (!s.conn || s.conn->id() != connId || s.conn->state() != Connection::State::Up) return;
        if (s.conn->lastReceiveMs() >= sentAt) return; // answered
        probe(*p, s, "no answer to a heartbeat");
    });
}

void ConnectionManager::probe(Peer& p, Slot& s, const std::string& why) {
    if (!s.conn || s.conn->state() != Connection::State::Up) return;
    std::string peerId = p.rec.id;
    TransportKind k = s.kind;
    uint64_t connId = s.conn->id();
    int64_t sentAt = sched_.nowMs();
    log::info("link", "probing the {} link to {} ({})", toString(k), p.rec.name, why);
    s.conn->sendEnvelope(p.session.make(proto::types::Ping, {{"t", sentAt}}));
    ++p.msgsOut;
    s.pongCheck.start(kPongWaitMs, [this, peerId, k, connId, sentAt] {
        Peer* p = find(peerId);
        if (!p) return;
        Slot& s = slot(*p, k);
        if (!s.conn || s.conn->id() != connId || s.conn->state() != Connection::State::Up) return;
        if (s.conn->lastReceiveMs() >= sentAt) return; // alive after all
        log::warn("link", "{} link to {} does not answer, closing", toString(k), p->rec.name);
        s.conn->close("no answer to probe");
    });
}

Connection* ConnectionManager::connById(Peer& p, uint64_t id) {
    for (Slot* s : {&p.bt, &p.lan})
        if (s->conn && s->conn->id() == id && s->conn->state() == Connection::State::Up) return s->conn.get();
    return nullptr;
}

void ConnectionManager::onConnEnvelope(Peer& p, Connection& c, proto::Envelope e) {
    if (e.sid != p.session.sid()) {
        log::debug("link", "dropping {} from stale session", e.type);
        return;
    }
    if (!p.session.acceptIncoming(e)) {
        if (e.type == proto::types::MediaCommand)
            if (auto* r = p.session.cachedResult(e.id)) c.sendEnvelope(*r);
        return;
    }
    ++p.msgsIn;
    const std::string& t = e.type;
    if (t == proto::types::Ping) {
        // rt: our clock as we answer, for the peer's clock estimate (§11.1).
        c.sendEnvelope(p.session.make(proto::types::Pong, {{"t", e.body.value("t", int64_t(0))}, {"rt", sched_.nowMs()}}, e.id));
        ++p.msgsOut;
        return;
    }
    if (t == proto::types::Pong) {
        int64_t sent = e.body.value("t", int64_t(-1));
        int64_t now = sched_.nowMs();
        if (sent >= 0 && sent <= now && now - sent < 120'000) {
            int64_t rtt = now - sent;
            p.rttMs = p.rttMs < 0 ? rtt : (p.rttMs * 3 + rtt) / 4;
            if (auto rt = e.body.find("rt"); rt != e.body.end() && rt->is_number_integer())
                p.clock.addSample(sent, rt->get<int64_t>(), now);
            listener_.onStatusChanged();
        }
        return;
    }
    if (t == proto::types::MediaCommandResult) {
        p.session.completeCommand(e);
        return;
    }
    if (t == proto::types::Bye) {
        std::string reason = e.body.value("reason", std::string());
        log::info("link", "{} said bye ({})", p.rec.name, reason);
        Slot& s = (&c == p.bt.conn.get()) ? p.bt : p.lan;
        if (reason == "user_disconnect") s.peerAskedBackoff = true;
        c.close("peer disconnected: " + reason);
        return;
    }
    if (t == proto::types::TransportInfo) {
        if (auto lan = e.body.find("lan"); lan != e.body.end() && lan->is_array()) {
            std::vector<LanEndpoint> eps;
            for (auto& x : *lan) {
                if (!x.is_object() || eps.size() >= 8) continue;
                LanEndpoint ep{x.value("host", std::string()), static_cast<uint16_t>(x.value("port", 0)), "peer"};
                if (ep.host.empty() || ep.host.size() > 64 || ep.port == 0) continue;
                eps.push_back(ep);
            }
            int64_t now = sched_.nowMs();
            bool anyNew = false;
            for (auto& ep : eps) {
                anyNew |= std::find(p.reported.begin(), p.reported.end(), ep) == p.reported.end();
                auto& info = p.eps[ep.str()];
                info.reportedAt = now;
                info.coolUntil = -1; // the phone says it has this address right now
            }
            // The report is the phone's complete current list: addresses it no longer has are
            // dropped at once instead of lingering for the TTL.
            for (auto& old : p.reported)
                if (std::find(eps.begin(), eps.end(), old) == eps.end()) p.eps[old.str()].reportedAt = -1;
            p.reported = eps;
            listener_.onPeerEndpoints(p.rec.id, eps);
            Slot& ls = p.lan;
            if (ls.conn && ls.currentEp && !eps.empty() &&
                std::find(eps.begin(), eps.end(), *ls.currentEp) == eps.end()) {
                // Our LAN link uses an address the phone no longer reports (its IP changed):
                // find out now instead of waiting for heartbeats.
                probe(p, ls, "the phone's address changed");
            }
            if (!ls.conn && anyNew && ls.inFlight == 0) {
                // A new address: no back-off, start from the best candidate (= the report).
                log::info("link", "{} reports new LAN address(es); connecting now", p.rec.name);
                ls.retry.cancel();
                ls.failures = 0;
                ls.endpointCursor = 0;
                ls.cycle.clear();
            }
            evaluate(p);
        }
        listener_.onEnvelope(p.rec.id, e, c.id()); // e.g. btAddress for LAN-paired phones
        return;
    }
    if (t == proto::types::DeviceInfo) {
        if (auto d = proto::deviceInfoFromJson(e.body); d && d->id == p.rec.id) {
            p.rec.name = d->name;
            if (!d->app.empty()) p.app = d->app;
            listener_.onStatusChanged();
        }
    }
    listener_.onEnvelope(p.rec.id, e, c.id());
}

void ConnectionManager::onConnClosed(const std::string& peerId, TransportKind k, uint64_t connId,
                                     const std::string& reason, const std::string& code) {
    Peer* p = find(peerId);
    if (!p) return;
    Slot& s = slot(*p, k);
    if (k == TransportKind::Lan) {
        auto it = std::find_if(s.pending.begin(), s.pending.end(),
                               [&](const Slot::Pending& x) { return x.conn->id() == connId; });
        if (it != s.pending.end()) {
            // The transport opened but the handshake never completed (timeout,
            // protocol error, closed by the peer): demote this address so the next
            // cycles prefer the others, and move on.
            LanEndpoint ep = it->ep;
            s.pending.erase(it);
            log::info("link", "lan handshake with {} at {} failed: {}", p->rec.name, ep.str(), reason);
            if (code == "not_paired" || code == "auth_failed") {
                // A peer-level answer: handled like any closed link below.
                s.conn = nullptr;
            } else {
                p->eps[ep.str()].coolUntil = sched_.nowMs() + cfg_.endpointCooldownMs;
                p->lastError = reason;
                lanAdvance(*p, s, cfg_.nextEndpointDelayMs);
                evaluateAll();
                listener_.onStatusChanged();
                return;
            }
        } else if (!s.conn || s.conn->id() != connId) {
            return;
        }
    } else if (!s.conn || s.conn->id() != connId) {
        return;
    }
    bool wasUp = s.upSince > 0;
    int64_t lived = wasUp ? sched_.nowMs() - s.upSince : 0;
    bool wasPrimary = p->primary == s.conn.get();
    s.conn.reset();
    s.heartbeat.cancel();
    s.upSince = 0;
    log::info("link", "{} link to {} closed: {}", toString(k), p->rec.name, reason);

    if (code == "not_paired") {
        p->revoked = true;
        p->lastError = "the phone no longer trusts this PC";
        Slot& other = (&s == &p->bt) ? p->lan : p->bt;
        if (other.conn) other.conn->close("pairing revoked by peer");
        p->primary = nullptr;
        p->session.failAll("not_paired");
        listener_.onPeerRevoked(peerId);
        listener_.onLinkDown(peerId);
        listener_.onStatusChanged();
        return;
    }
    if (code == "auth_failed") ++p->authFailures;
    if (!reason.empty() && reason != "disconnected by user") p->lastError = reason;
    if (k == TransportKind::Lan && wasUp) p->lanDrops.push_back(sched_.nowMs());
    if (s.peerAskedBackoff) {
        // The phone user disconnected on purpose: retry rarely (presence events still reset this).
        s.failures = 8;
        s.peerAskedBackoff = false;
    } else if (wasUp && lived >= kStableLifetimeMs) {
        s.failures = 0;
    } else {
        ++s.failures;
    }
    // Next cycle starts from the best-ranked endpoint (normally the one that just worked).
    s.endpointCursor = 0;
    s.cycle.clear();
    s.currentEp.reset();

    if (wasPrimary) {
        p->primary = nullptr;
        choosePrimary(*p);
        if (!p->primary) listener_.onLinkDown(peerId);
    }
    scheduleRetry(*p, s);
    evaluateAll();
    listener_.onStatusChanged();
}

// ------------------------------------------------------------------ sending

uint64_t ConnectionManager::send(const std::string& type, proto::json body, std::optional<uint64_t> re) {
    for (auto& [id, p] : peers_) {
        if (!p->primary) continue;
        auto e = p->session.make(type, std::move(body), re);
        if (!p->primary->sendEnvelope(e)) return 0;
        ++p->msgsOut;
        return e.id;
    }
    return 0;
}

uint64_t ConnectionManager::sendVia(uint64_t connId, const std::string& type, proto::json body,
                                    std::optional<uint64_t> re) {
    for (auto& [id, p] : peers_) {
        if (!p->primary) continue;
        Connection* c = connById(*p, connId);
        if (!c) c = p->primary;
        auto e = p->session.make(type, std::move(body), re);
        if (!c->sendEnvelope(e)) return 0;
        ++p->msgsOut;
        return e.id;
    }
    return 0;
}

uint64_t ConnectionManager::sendCommand(const media::MediaCommand& cmd, LinkSession::ResultFn fn) {
    for (auto& [id, p] : peers_) {
        if (!p->primary) continue;
        auto e = p->session.make(proto::types::MediaCommand, media::toWire(cmd));
        p->session.track(e, std::move(fn));
        p->primary->sendEnvelope(e);
        ++p->msgsOut;
        return e.id;
    }
    if (fn) fn({false, "no_link", "not connected to the phone", 0});
    return 0;
}

bool ConnectionManager::sendBlob(Bytes data, uint64_t preferConnId) {
    for (auto& [id, p] : peers_) {
        if (!p->primary) continue;
        Connection* c = nullptr;
        if (p->lan.conn && p->lan.conn->state() == Connection::State::Up) c = p->lan.conn.get();
        if (!c && preferConnId) c = connById(*p, preferConnId);
        if (!c) c = p->primary;
        return c->queueBlob(std::move(data));
    }
    return false;
}

void ConnectionManager::cacheResult(uint64_t requestId, const proto::Envelope& result) {
    for (auto& [id, p] : peers_)
        if (p->primary) p->session.cacheResult(requestId, result);
}

// ------------------------------------------------------------------ status

bool ConnectionManager::isUp() const { return activePeerPtr() != nullptr; }

std::optional<std::string> ConnectionManager::activePeer() const {
    if (auto* p = activePeerPtr()) return p->rec.id;
    return std::nullopt;
}

const std::vector<std::string>* ConnectionManager::peerCaps() const {
    auto* p = activePeerPtr();
    return p ? &p->caps : nullptr;
}

int64_t ConnectionManager::remoteAnchor(std::optional<int64_t> peerAt) const {
    int64_t now = sched_.nowMs();
    const Peer* p = activePeerPtr();
    if (!p) return now;
    return p->clock.anchor(peerAt, now, p->rttMs > 0 ? p->rttMs / 2 : 0);
}

std::string ConnectionManager::remoteLanHost() const {
    for (auto& [id, p] : peers_)
        if (p->lan.conn && p->lan.conn->state() == Connection::State::Up) return p->lan.conn->remoteHost();
    return {};
}

std::optional<ConnectionManager::AudioRoute> ConnectionManager::audioRoute() const {
    const Peer* p = activePeerPtr();
    if (!p) return std::nullopt;
    if (p->lan.conn && p->lan.conn->state() == Connection::State::Up && p->lan.conn->session()) {
        return AudioRoute{p->lan.conn->id(), p->lan.conn->session()->keys.keyC2S, p->lan.conn->remoteHost()};
    }
    auto eps = lanCandidates(*p);
    if (eps.empty() || !p->primary || !p->primary->session()) return std::nullopt;
    return AudioRoute{p->primary->id(), p->primary->session()->keys.keyC2S, eps.front().host};
}

LinkStatus ConnectionManager::status() const {
    LinkStatus st;
    if (peers_.empty()) return st;
    const Peer* p = activePeerPtr();
    if (!p) {
        // Report the most relevant peer: one that is connecting, else the first.
        for (auto& [id, q] : peers_)
            if (!p || q->bt.connecting || lanBusy(q->lan) || (q->bt.conn || q->lan.conn)) p = q.get();
    }
    st.peerId = p->rec.id;
    st.peerName = p->rec.name;
    st.peerPlatform = p->rec.platform;
    st.peerApp = p->app;
    st.lastError = p->lastError;
    st.rttMs = p->rttMs;
    st.clockErrorMs = p->clock.uncertaintyMs(sched_.nowMs());
    st.msgsIn = p->msgsIn;
    st.msgsOut = p->msgsOut;
    st.peerCaps = p->caps;
    for (const Slot* s : {&p->bt, &p->lan})
        if (s->conn) {
            st.bytesIn += s->conn->bytesIn();
            st.bytesOut += s->conn->bytesOut();
        }
    if (p->primary) {
        st.state = LinkStatus::State::Connected;
        st.primary = p->primary->kind();
        st.sinceMs = p->primary->upSinceMs();
        const Slot& other = p->primary->kind() == TransportKind::Bluetooth ? p->lan : p->bt;
        if (other.conn && other.conn->state() == Connection::State::Up) st.secondary = other.kind;
    } else if (p->bt.connecting || lanBusy(p->lan) || p->bt.conn || p->lan.conn) {
        st.state = LinkStatus::State::Connecting;
    } else {
        st.state = LinkStatus::State::Disconnected;
    }
    return st;
}

} // namespace md::link
