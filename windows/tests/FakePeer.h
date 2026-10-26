#pragma once

// Test-only fakes: an in-memory transport and a spec-conformant fake *server*
// (the Android role) so that client logic can be exercised end to end without
// hardware. Never used by production code.

#include "md/crypto/Crypto.h"
#include "md/link/ConnectionManager.h"
#include "md/link/Handshake.h"
#include "md/link/LinkSession.h"
#include "md/link/SecureChannel.h"
#include "md/protocol/Frame.h"
#include "md/protocol/Messages.h"
#include "md/util/ManualScheduler.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace md::test {

class FakeServer;

// Client end of an in-memory stream. Bytes travel through the scheduler (async).
class FakeTransport : public link::ITransport, public std::enable_shared_from_this<FakeTransport> {
public:
    FakeTransport(ManualScheduler& s, link::TransportKind k, FakeServer* server) : s_(s), kind_(k), server_(server) {}
    link::TransportKind kind() const override { return kind_; }
    std::string describe() const override { return kind_ == link::TransportKind::Bluetooth ? "fake-bt" : "fake-lan"; }
    std::string remoteHost() const override { return kind_ == link::TransportKind::Lan ? "192.0.2.10" : ""; }
    void start(Listener* l) override { listener_ = l; }
    void send(Bytes data) override;
    size_t pendingWrites() const override { return 0; }
    void close() override { closeWith("closed locally"); }

    // Server side helpers.
    void deliverToClient(Bytes data) {
        auto self = shared_from_this();
        s_.post([self, d = std::move(data)] {
            if (!self->closed_ && self->listener_) self->listener_->onTransportData(d);
        });
    }
    void closeWith(const std::string& reason);
    bool closed() const { return closed_; }
    void setServer(FakeServer* s) { server_ = s; }

private:
    ManualScheduler& s_;
    link::TransportKind kind_;
    FakeServer* server_;
    Listener* listener_ = nullptr;
    bool closed_ = false;
};

class FakeServer {
public:
    struct Conn {
        std::shared_ptr<FakeTransport> t;
        proto::FrameDecoder dec;
        enum class St { Hello, AwaitAuth, Up, Pairing, Dead } st = St::Hello;
        std::optional<crypto::EcdhKey> eph;
        Bytes nonce, clientNonce, clientEpk, th, pairKey, pairTh;
        std::string clientId;
        std::string sid;
        std::unique_ptr<link::SecureChannel> chan;
        bool pairingUserAccepted = false;
        bool clientConfirmed = false;
        bool qr = false; // QR pairing (§3.5)
    };
    struct Session {
        std::string sid;
        link::DedupWindow dedup;
        uint64_t nextId = 1;
        Conn* primary = nullptr;
        std::map<uint64_t, proto::Envelope> results;
    };

    ManualScheduler& s;
    proto::DeviceInfo info;
    std::map<std::string, Bytes> keys; // client id -> pairing key
    bool pairingEnabled = true;
    bool autoAcceptPairing = true;
    bool corruptHelloAckMac = false;
    bool forgetSessions = false;
    int64_t clockOffsetMs = 1'000'000; // the phone's monotonic clock relative to ours
    bool pongWithRt = true;            // false: an older phone without `rt` in pongs
    std::map<std::string, Session> sessions;
    std::vector<std::unique_ptr<Conn>> conns;
    std::map<uint64_t, int> executions; // media.command id -> times executed
    std::vector<std::pair<link::TransportKind, std::string>> received; // (transport, type)
    std::string lastSas;
    // QR pairing (§3.5): the scanned code's (PC id, secret); empty = not armed.
    std::string qrId;
    Bytes qrToken;

    explicit FakeServer(ManualScheduler& sched) : s(sched) {
        info.id = "fedcba9876543210fedcba9876543210";
        info.name = "Fake Phone";
        info.platform = "android";
    }

    std::shared_ptr<FakeTransport> accept(link::TransportKind k) {
        auto c = std::make_unique<Conn>();
        c->t = std::make_shared<FakeTransport>(s, k, this);
        auto t = c->t;
        conns.push_back(std::move(c));
        return t;
    }

    Conn* find(FakeTransport* t) {
        for (auto& c : conns)
            if (c->t.get() == t) return c.get();
        return nullptr;
    }

    void onClientClosed(FakeTransport* t) {
        if (auto* c = find(t)) {
            c->st = Conn::St::Dead;
            for (auto& [sid, ss] : sessions)
                if (ss.primary == c) ss.primary = nullptr;
        }
    }

    // Simulates a link failure initiated by the server/OS.
    void kill(link::TransportKind k) {
        for (auto& c : conns)
            if (c->t->kind() == k && c->st != Conn::St::Dead) {
                c->st = Conn::St::Dead;
                c->t->closeWith("link lost");
            }
    }

    int liveConns(link::TransportKind k) const {
        int n = 0;
        for (auto& c : conns)
            if (c->t->kind() == k && c->st == Conn::St::Up && !c->t->closed()) ++n;
        return n;
    }

    Conn* primaryConn() {
        for (auto& [sid, ss] : sessions)
            if (ss.primary && ss.primary->st == Conn::St::Up) return ss.primary;
        return nullptr;
    }

    void sendHandshake(Conn& c, const std::string& type, proto::json body) {
        proto::Envelope e;
        e.type = type;
        e.id = 1;
        e.src = info.id;
        e.body = std::move(body);
        auto str = e.serialize();
        c.t->deliverToClient(proto::encodeFrame(proto::FrameType::Handshake, view(str)));
    }

    void sendSecure(Conn& c, const std::string& type, proto::json body, std::optional<uint64_t> re = {}) {
        auto& ss = sessions[c.sid];
        proto::Envelope e;
        e.type = type;
        e.id = ss.nextId++;
        e.src = info.id;
        e.sid = c.sid;
        e.re = re;
        e.body = std::move(body);
        auto str = e.serialize();
        c.t->deliverToClient(c.chan->sealFrame(proto::Channel::Control, view(str)));
    }

    // Sends an unsolicited message on the session's primary transport.
    bool push(const std::string& type, proto::json body) {
        Conn* c = primaryConn();
        if (!c) return false;
        sendSecure(*c, type, std::move(body));
        return true;
    }

    void onData(FakeTransport* t, ByteView data) {
        Conn* c = find(t);
        if (!c || c->st == Conn::St::Dead) return;
        c->dec.feed(data);
        while (auto f = c->dec.next()) handleFrame(*c, std::move(*f));
    }

    void fail(Conn& c, const std::string& code) {
        sendHandshake(c, proto::types::Error, proto::errorBody(code));
        c.st = Conn::St::Dead;
        auto t = c.t;
        s.post([t] { t->closeWith("server closed"); });
    }

    void handleFrame(Conn& c, proto::Frame f) {
        if (f.type == proto::FrameType::Handshake) {
            auto e = proto::Envelope::parse(toString(f.body));
            if (!e) return fail(c, "bad_message");
            received.push_back({c.t->kind(), e->type});
            if (e->type == proto::types::Hello) return onHello(c, *e);
            if (e->type == proto::types::Auth) return onAuth(c, *e);
            if (e->type == proto::types::PairRequest) return onPairRequest(c, *e);
            if (e->type == proto::types::PairConfirm) return onPairConfirm(c, *e);
            if (e->type == proto::types::PairResult) {
                c.st = Conn::St::Dead;
                return;
            }
            return fail(c, "bad_message");
        }
        if (c.st != Conn::St::Up) return fail(c, "protocol");
        auto rec = c.chan->openFrameBody(f.body);
        if (!rec) return fail(c, "auth_failed");
        if (rec->channel != proto::Channel::Control) return;
        auto e = proto::Envelope::parse(toString(rec->data));
        if (!e) return;
        received.push_back({c.t->kind(), e->type});
        auto& ss = sessions[c.sid];
        if (!ss.dedup.accept(e->id)) {
            if (e->type == proto::types::MediaCommand && ss.results.count(e->id))
                sendSecure(c, proto::types::MediaCommandResult, ss.results[e->id].body, e->id);
            return;
        }
        if (e->type == proto::types::Ping) {
            proto::json body = {{"t", e->body["t"]}};
            if (pongWithRt) body["rt"] = s.nowMs() + clockOffsetMs;
            sendSecure(c, proto::types::Pong, body, e->id);
        } else if (e->type == proto::types::LinkPrimary) {
            ss.primary = &c;
        } else if (e->type == proto::types::MediaCommand) {
            ++executions[e->id];
            proto::json body = {{"ok", true}};
            proto::Envelope r;
            r.body = body;
            ss.results[e->id] = r;
            sendSecure(c, proto::types::MediaCommandResult, body, e->id);
        }
    }

    void onHello(Conn& c, const proto::Envelope& e) {
        auto it = keys.find(e.src);
        if (it == keys.end()) return fail(c, "not_paired");
        c.clientId = e.src;
        c.clientNonce = *proto::getB64(e.body, "nonce", 32);
        c.clientEpk = *proto::getB64(e.body, "epk", 65);
        c.eph = crypto::EcdhKey::generate();
        c.nonce = crypto::randomBytes(32);
        std::string resume = e.body.value("resume", std::string());
        bool resumed = !forgetSessions && !resume.empty() && sessions.count(resume);
        c.sid = resumed ? resume : crypto::randomHex(16);
        if (!resumed) sessions[c.sid].sid = c.sid;
        c.th = link::ks::sessionTranscript(c.clientNonce, c.nonce, c.clientEpk, c.eph->publicKey(), c.clientId,
                                           info.id);
        Bytes mac = link::ks::sessionMac(it->second, "server", c.th);
        if (corruptHelloAckMac) mac[0] ^= 0xff;
        sendHandshake(c, proto::types::HelloAck,
                      {{"device", proto::toJson(info)},
                       {"protocol", 1},
                       {"caps", {"media.source", "media.control"}},
                       {"epk", base64Encode(c.eph->publicKey())},
                       {"nonce", base64Encode(c.nonce)},
                       {"sid", c.sid},
                       {"resumed", resumed},
                       {"mac", base64Encode(mac)}});
        c.st = Conn::St::AwaitAuth;
    }

    void onAuth(Conn& c, const proto::Envelope& e) {
        if (c.st != Conn::St::AwaitAuth) return fail(c, "protocol");
        auto mac = proto::getB64(e.body, "mac", 32);
        const Bytes& key = keys[c.clientId];
        if (!mac || !crypto::constantTimeEqual(*mac, link::ks::sessionMac(key, "client", c.th)))
            return fail(c, "auth_failed");
        Bytes z = c.eph->agree(c.clientEpk);
        auto k = link::ks::sessionKeys(key, z, c.th);
        c.chan = std::make_unique<link::SecureChannel>(k.keyS2C, k.ivS2C, k.keyC2S, k.ivC2S);
        c.st = Conn::St::Up;
        sendSecure(c, proto::types::SessionReady, {{"sid", c.sid}});
    }

    void onPairRequest(Conn& c, const proto::Envelope& e) {
        c.qr = e.body.value("qr", false);
        if (!pairingEnabled || (c.qr && (qrToken.empty() || e.src != qrId))) {
            sendHandshake(c, proto::types::PairResult, {{"ok", false}, {"reason", "pairing_disabled"}});
            c.st = Conn::St::Dead;
            return;
        }
        auto dev = proto::deviceInfoFromJson(e.body["device"]);
        c.clientId = dev->id;
        c.clientNonce = *proto::getB64(e.body, "nonce", 32);
        c.clientEpk = *proto::getB64(e.body, "epk", 65);
        c.eph = crypto::EcdhKey::generate();
        c.nonce = crypto::randomBytes(32);
        sendHandshake(c, proto::types::PairResponse,
                      {{"device", proto::toJson(info)},
                       {"epk", base64Encode(c.eph->publicKey())},
                       {"nonce", base64Encode(c.nonce)}});
        Bytes z = c.eph->agree(c.clientEpk);
        c.pairTh = link::ks::pairTranscript(c.clientNonce, c.nonce, c.clientEpk, c.eph->publicKey(), c.clientId,
                                            info.id);
        auto sec = link::ks::pairing(z, c.pairTh);
        c.pairKey = sec.pairingKey;
        lastSas = sec.sas;
        c.st = Conn::St::Pairing;
        if (autoAcceptPairing && !c.qr) {
            c.pairingUserAccepted = true;
            sendHandshake(c, proto::types::PairConfirm,
                          {{"mac", base64Encode(link::ks::confirmMac(c.pairKey, 'S', c.pairTh))}});
        }
    }

    void onPairConfirm(Conn& c, const proto::Envelope& e) {
        auto mac = proto::getB64(e.body, "mac", 32);
        Bytes expected = c.qr ? link::ks::qrConfirmMac(qrToken, 'C', c.pairTh) : link::ks::confirmMac(c.pairKey, 'C', c.pairTh);
        if (!mac || !crypto::constantTimeEqual(*mac, expected)) {
            sendHandshake(c, proto::types::PairResult, {{"ok", false}, {"reason", "mac_mismatch"}});
            return;
        }
        if (c.qr) {
            sendHandshake(c, proto::types::PairConfirm, {{"mac", base64Encode(link::ks::qrConfirmMac(qrToken, 'S', c.pairTh))}});
            keys[c.clientId] = c.pairKey;
            qrToken.clear();
            sendHandshake(c, proto::types::PairResult, {{"ok", true}});
            return;
        }
        if (c.pairingUserAccepted) {
            keys[c.clientId] = c.pairKey;
            sendHandshake(c, proto::types::PairResult, {{"ok", true}});
        }
    }
};

inline void FakeTransport::send(Bytes data) {
    if (closed_) return;
    auto self = shared_from_this();
    s_.post([self, d = std::move(data)] {
        if (!self->closed_ && self->server_) self->server_->onData(self.get(), d);
    });
}

inline void FakeTransport::closeWith(const std::string& reason) {
    if (closed_) return;
    closed_ = true;
    auto self = shared_from_this();
    if (server_) server_->onClientClosed(this);
    s_.post([self, reason] {
        if (self->listener_) self->listener_->onTransportClosed(reason);
    });
}

// Factory that "connects" to the fake server if the given transport is reachable.
class FakeFactory : public link::ITransportFactory {
public:
    FakeFactory(ManualScheduler& s, FakeServer& srv) : s_(s), srv_(srv) {}
    bool btReachable = true;
    bool lanReachable = true;
    int64_t btDelayMs = 300;
    int64_t lanDelayMs = 50;
    int btAttempts = 0, lanAttempts = 0;

    void connectBluetooth(const link::PeerRecord&, Done done) override {
        ++btAttempts;
        bool ok = btReachable;
        s_.schedule(btDelayMs, [this, ok, done] {
            if (ok) done(srv_.accept(link::TransportKind::Bluetooth), "");
            else done(nullptr, "bt unreachable");
        });
    }
    // Per-host LAN behaviour (default: lanReachable ? "ok" : "refuse"). "silent" accepts
    // the TCP connection but never answers (another interface / service on that address).
    std::map<std::string, std::string> lanHostMode;
    std::vector<std::string> lanHosts; // every LAN attempt, in order

    void connectLan(const link::LanEndpoint& ep, Done done) override {
        ++lanAttempts;
        lanHosts.push_back(ep.host);
        auto it = lanHostMode.find(ep.host);
        std::string mode = it != lanHostMode.end() ? it->second : (lanReachable ? "ok" : "refuse");
        s_.schedule(lanDelayMs, [this, mode, done] {
            if (mode == "ok") done(srv_.accept(link::TransportKind::Lan), "");
            else if (mode == "silent") done(std::make_shared<FakeTransport>(s_, link::TransportKind::Lan, nullptr), "");
            else done(nullptr, "lan unreachable");
        });
    }
    int attemptsTo(const std::string& host) const {
        return static_cast<int>(std::count(lanHosts.begin(), lanHosts.end(), host));
    }

private:
    ManualScheduler& s_;
    FakeServer& srv_;
};

} // namespace md::test
