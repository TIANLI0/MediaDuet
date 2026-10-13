#include "FakePeer.h"

#include "md/link/Connection.h"

#include "TestStrings.h"

#include <doctest/doctest.h>

using namespace md;
using namespace md::test;

namespace {

proto::DeviceInfo clientInfo() {
    proto::DeviceInfo d;
    d.id = "0123456789abcdef0123456789abcdef";
    d.name = "Test PC";
    d.platform = "windows";
    return d;
}

struct Harness {
    ManualScheduler s;
    FakeServer srv{s};
    std::string sas;
    std::optional<Bytes> pairedKey;
    std::string closedCode;
    std::string closedReason;
    bool up = false;
    std::vector<proto::Envelope> got;

    link::ConnectionPtr make(link::TransportKind k = link::TransportKind::Lan) {
        link::Connection::Events ev;
        ev.onSas = [this](link::Connection&, const std::string& code, const proto::DeviceInfo&) { sas = code; };
        ev.onPaired = [this](link::Connection&, const proto::DeviceInfo&, const Bytes& k) { pairedKey = k; };
        ev.onClosed = [this](link::Connection&, const std::string& r, const std::string& c) {
            closedReason = r;
            closedCode = c;
        };
        ev.onUp = [this](link::Connection&, const link::SessionClient::Established&) { up = true; };
        ev.onEnvelope = [this](link::Connection&, proto::Envelope e) { got.push_back(std::move(e)); };
        return link::Connection::create(s, srv.accept(k), std::move(ev));
    }
};

} // namespace

TEST_CASE("pairing with SAS confirmation stores the same key on both sides") {
    Harness h;
    auto c = h.make();
    c->beginPairing(clientInfo());
    h.s.runPending();
    REQUIRE(h.sas.size() == 6);
    CHECK(h.sas == h.srv.lastSas);
    CHECK_FALSE(h.pairedKey);
    c->pairingDecision(true);
    h.s.runPending();
    REQUIRE(h.pairedKey);
    CHECK(h.srv.keys[clientInfo().id] == *h.pairedKey);
    CHECK(c->state() == link::Connection::State::Closed);
}

TEST_CASE("pairing rejected locally never stores a key") {
    Harness h;
    auto c = h.make();
    c->beginPairing(clientInfo());
    h.s.runPending();
    c->pairingDecision(false);
    h.s.runPending();
    CHECK_FALSE(h.pairedKey);
    CHECK(h.srv.keys.empty());
    CHECK(h.closedCode == "rejected");
}

TEST_CASE("unconfirmed pairing times out with code timeout, not rejected") {
    Harness h;
    auto c = h.make();
    c->beginPairing(clientInfo());
    h.s.runPending();
    REQUIRE(h.sas.size() == 6);
    h.s.advance(121'000);
    CHECK(h.closedCode == "timeout");
    CHECK_FALSE(h.pairedKey);
}

TEST_CASE("pairing refused when the phone is not in pairing mode") {
    Harness h;
    h.srv.pairingEnabled = false;
    auto c = h.make();
    c->beginPairing(clientInfo());
    h.s.runPending();
    CHECK(h.sas.empty());
    CHECK(h.closedCode == "pairing_disabled");
}

TEST_CASE("session handshake establishes an encrypted session") {
    Harness h;
    Bytes key = crypto::randomBytes(32);
    h.srv.keys[clientInfo().id] = key;
    auto c = h.make();
    c->beginSession(clientInfo(), h.srv.info.id, key, {"media.control"}, "");
    h.s.runPending();
    REQUIRE(h.up);
    CHECK(c->state() == link::Connection::State::Up);
    REQUIRE(c->session());
    CHECK(c->session()->peer.name == "Fake Phone");
    // Ping round trip over the secure channel.
    proto::Envelope ping;
    ping.type = "ping";
    ping.id = 1;
    ping.src = clientInfo().id;
    ping.sid = c->session()->sid;
    ping.body = {{"t", 5}};
    CHECK(c->sendEnvelope(ping));
    h.s.runPending();
    REQUIRE(h.got.size() == 1);
    CHECK(h.got[0].type == "pong");
    CHECK(h.got[0].re == 1u);
}

TEST_CASE("unknown client gets not_paired") {
    Harness h;
    auto c = h.make();
    c->beginSession(clientInfo(), h.srv.info.id, crypto::randomBytes(32), {}, "");
    h.s.runPending();
    CHECK_FALSE(h.up);
    CHECK(h.closedCode == "not_paired");
}

TEST_CASE("wrong pairing key fails authentication on both sides") {
    SUBCASE("client detects a server without the key") {
        Harness h;
        h.srv.keys[clientInfo().id] = crypto::randomBytes(32);
        auto c = h.make();
        c->beginSession(clientInfo(), h.srv.info.id, crypto::randomBytes(32), {}, "");
        h.s.runPending();
        CHECK_FALSE(h.up);
        CHECK(h.closedCode == "auth_failed");
    }
    SUBCASE("tampered hello.ack MAC") {
        Harness h;
        Bytes key = crypto::randomBytes(32);
        h.srv.keys[clientInfo().id] = key;
        h.srv.corruptHelloAckMac = true;
        auto c = h.make();
        c->beginSession(clientInfo(), h.srv.info.id, key, {}, "");
        h.s.runPending();
        CHECK_FALSE(h.up);
        CHECK(h.closedCode == "auth_failed");
    }
}

TEST_CASE("handshake times out when the peer is silent") {
    ManualScheduler s;
    struct Silent : link::ITransport {
        Listener* l = nullptr;
        ManualScheduler* s = nullptr;
        bool closed = false;
        link::TransportKind kind() const override { return link::TransportKind::Bluetooth; }
        std::string describe() const override { return "silent"; }
        void start(Listener* x) override { l = x; }
        void send(Bytes) override {}
        size_t pendingWrites() const override { return 0; }
        void close() override {
            if (closed) return;
            closed = true;
            s->post([this] { l->onTransportClosed("closed"); });
        }
    };
    auto t = std::make_shared<Silent>();
    t->s = &s;
    std::string code;
    link::Connection::Events ev;
    ev.onClosed = [&](link::Connection&, const std::string&, const std::string& c) { code = c; };
    auto c = link::Connection::create(s, t, ev);
    c->beginSession(clientInfo(), "fedcba9876543210fedcba9876543210", crypto::randomBytes(32), {}, "");
    s.advance(9'000);
    CHECK(code.empty());
    s.advance(2'000);
    CHECK(code == "timeout");
}
