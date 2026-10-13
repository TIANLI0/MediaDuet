#include "FakePeer.h"

#include "TestStrings.h"

#include <doctest/doctest.h>

using namespace md;
using namespace md::test;
using link::TransportKind;

namespace {

struct Recorder : link::ConnectionManager::Listener {
    int ups = 0, downs = 0, revoked = 0;
    bool lastResumed = false;
    std::vector<proto::Envelope> envs;
    void onLinkUp(const std::string&, bool resumed) override {
        ++ups;
        lastResumed = resumed;
    }
    void onLinkDown(const std::string&) override { ++downs; }
    void onEnvelope(const std::string&, const proto::Envelope& e, uint64_t) override { envs.push_back(e); }
    void onBlob(const std::string&, const proto::BlobChunk&) override {}
    void onPeerRevoked(const std::string&) override { ++revoked; }
    void onStatusChanged() override {}
};

struct World {
    ManualScheduler s;
    FakeServer srv{s};
    FakeFactory fac{s, srv};
    Recorder rec;
    std::unique_ptr<link::ConnectionManager> cm;
    Bytes key = crypto::randomBytes(32);

    link::PeerRecord peer;

    explicit World(link::ConnMode mode = link::ConnMode::Auto, bool withLan = true, bool autoStart = true) {
        proto::DeviceInfo self;
        self.id = "0123456789abcdef0123456789abcdef";
        self.name = "PC";
        self.platform = "windows";
        srv.keys[self.id] = key;
        link::ConnectionManager::Config cfg;
        cfg.mode = mode;
        cm = std::make_unique<link::ConnectionManager>(s, fac, rec, self, std::vector<std::string>{"media.control"},
                                                       cfg);
        cm->setJitterSeed(42);
        link::PeerRecord p;
        p.id = srv.info.id;
        p.name = "Phone";
        p.platform = "android";
        p.btAddress = 0x112233445566;
        if (withLan) p.manualEndpoints.push_back({"192.0.2.10", 47321, "manual"});
        p.key = key;
        peer = p;
        if (autoStart) start();
    }
    void start() { cm->setPeers({peer}); }
    std::optional<TransportKind> primary() { return cm->status().primary; }
};

} // namespace

TEST_CASE("Bluetooth is preferred when both transports are available") {
    World w;
    w.s.advance(1000);
    CHECK(w.rec.ups == 1);
    CHECK(w.primary() == TransportKind::Bluetooth);
    CHECK(w.cm->status().secondary == TransportKind::Lan);
    // The server sends unsolicited messages on the primary the client designated.
    REQUIRE(w.srv.primaryConn());
    CHECK(w.srv.primaryConn()->t->kind() == TransportKind::Bluetooth);
}

TEST_CASE("failover to LAN, then stable switch-back to Bluetooth") {
    World w;
    w.cm->setSwitchBackDelay(10'000);
    w.s.advance(1000);
    REQUIRE(w.primary() == TransportKind::Bluetooth);

    w.fac.btReachable = false;
    w.srv.kill(TransportKind::Bluetooth);
    w.s.advance(100);
    CHECK(w.primary() == TransportKind::Lan);
    CHECK(w.rec.downs == 0); // LAN kept the session alive: no visible outage
    CHECK(w.srv.primaryConn()->t->kind() == TransportKind::Lan);

    // Bluetooth comes back; it must be stable for the switch-back delay first.
    w.fac.btReachable = true;
    w.cm->onPeerBluetoothPresence(w.srv.info.id, true);
    w.s.advance(1000);
    CHECK(w.srv.liveConns(TransportKind::Bluetooth) == 1);
    CHECK(w.primary() == TransportKind::Lan);
    w.s.advance(9'500);
    CHECK(w.primary() == TransportKind::Bluetooth);
    CHECK(w.rec.ups == 1);
}

TEST_CASE("a flapping Bluetooth link needs longer to be trusted again") {
    World w;
    w.cm->setSwitchBackDelay(10'000);
    w.s.advance(1000);
    for (int i = 0; i < 2; ++i) {
        w.srv.kill(TransportKind::Bluetooth);
        w.s.advance(100);
        CHECK(w.primary() == TransportKind::Lan);
        w.cm->onPeerBluetoothPresence(w.srv.info.id, false);
        w.cm->onPeerBluetoothPresence(w.srv.info.id, true);
        w.s.advance(1000); // BT reconnected
    }
    // Two drops -> 20 s stability required.
    w.s.advance(12'000);
    CHECK(w.primary() == TransportKind::Lan);
    w.s.advance(10'000);
    CHECK(w.primary() == TransportKind::Bluetooth);
}

TEST_CASE("commands in flight are re-sent after failover and executed exactly once") {
    World w;
    w.s.advance(1000);
    REQUIRE(w.primary() == TransportKind::Bluetooth);
    int results = 0;
    bool ok = false;
    media::MediaCommand cmd;
    cmd.action = media::CommandAction::Pause;
    // Kill BT right after sending, before the server processes it: the bytes are lost.
    uint64_t id = w.cm->sendCommand(cmd, [&](const link::CommandOutcome& o) {
        ++results;
        ok = o.ok;
    });
    REQUIRE(id != 0);
    w.fac.btReachable = false;
    w.srv.kill(TransportKind::Bluetooth);
    w.s.advance(500);
    CHECK(w.primary() == TransportKind::Lan);
    CHECK(results == 1);
    CHECK(ok);
    CHECK(w.srv.executions[id] == 1);
}

TEST_CASE("the same peer message arriving on two transports is delivered once") {
    World w;
    w.s.advance(1000);
    REQUIRE(w.srv.liveConns(TransportKind::Bluetooth) == 1);
    REQUIRE(w.srv.liveConns(TransportKind::Lan) == 1);
    // Server re-sends one message (same id) on both links, as after an ambiguous failure.
    std::string sid = w.srv.primaryConn()->sid;
    auto& ss = w.srv.sessions[sid];
    uint64_t id = ss.nextId++;
    for (auto& c : w.srv.conns) {
        if (c->st != FakeServer::Conn::St::Up) continue;
        proto::Envelope e;
        e.type = "media.command";
        e.id = id;
        e.src = w.srv.info.id;
        e.sid = sid;
        e.body = {{"action", "pause"}, {"origin", "user"}};
        auto str = e.serialize();
        c->t->deliverToClient(c->chan->sealFrame(proto::Channel::Control, view(str)));
    }
    w.s.advance(50);
    int cmds = 0;
    for (auto& e : w.rec.envs)
        if (e.type == "media.command") ++cmds;
    CHECK(cmds == 1);
}

TEST_CASE("session is resumed after reconnect; a server restart creates a new session") {
    World w(link::ConnMode::BluetoothOnly);
    w.s.advance(1000);
    CHECK(w.rec.ups == 1);
    w.srv.kill(TransportKind::Bluetooth);
    w.s.advance(100);
    CHECK(w.rec.downs == 1);
    w.s.advance(5'000);
    CHECK(w.rec.ups == 2);
    CHECK(w.rec.lastResumed);

    w.srv.forgetSessions = true;
    w.srv.kill(TransportKind::Bluetooth);
    w.s.advance(5'000);
    CHECK(w.rec.ups == 3);
    CHECK_FALSE(w.rec.lastResumed);
}

TEST_CASE("LAN-only mode never touches Bluetooth; Bluetooth-only never touches LAN") {
    World lan(link::ConnMode::LanOnly);
    lan.s.advance(1000);
    CHECK(lan.fac.btAttempts == 0);
    CHECK(lan.primary() == TransportKind::Lan);

    World bt(link::ConnMode::BluetoothOnly);
    bt.s.advance(1000);
    CHECK(bt.fac.lanAttempts == 0);
    CHECK(bt.primary() == TransportKind::Bluetooth);
}

TEST_CASE("reconnect back-off grows exponentially and is reset by presence events") {
    World w(link::ConnMode::BluetoothOnly, true, false);
    w.fac.btReachable = false;
    w.start();
    w.s.advance(120'000);
    // 2,4,8,16,32,60,60 s ... -> far fewer attempts than one per second.
    CHECK(w.fac.btAttempts >= 5);
    CHECK(w.fac.btAttempts <= 10);
    int before = w.fac.btAttempts;
    w.fac.btReachable = true;
    w.cm->onPeerBluetoothPresence(w.srv.info.id, true);
    w.s.advance(500);
    CHECK(w.fac.btAttempts == before + 1);
    CHECK(w.primary() == TransportKind::Bluetooth);
}

TEST_CASE("no Bluetooth adapter: LAN keeps working") {
    World w;
    w.cm->onBluetoothRadio(false);
    w.s.advance(1000);
    CHECK(w.fac.btAttempts <= 1);
    CHECK(w.primary() == TransportKind::Lan);
}

TEST_CASE("revoked pairing stops reconnecting") {
    World w(link::ConnMode::BluetoothOnly, true, false);
    w.srv.keys.clear();
    w.start();
    w.s.advance(1000);
    CHECK(w.rec.revoked == 1);
    int attempts = w.fac.btAttempts;
    w.s.advance(300'000);
    CHECK(w.fac.btAttempts == attempts);
    CHECK(w.rec.ups == 0);
}

TEST_CASE("user disconnect stays disconnected until reconnect is requested") {
    World w;
    w.s.advance(1000);
    REQUIRE(w.cm->isUp());
    w.cm->disconnect(w.srv.info.id);
    w.s.advance(60'000);
    CHECK_FALSE(w.cm->isUp());
    w.cm->connectNow(w.srv.info.id);
    w.s.advance(1000);
    CHECK(w.cm->isUp());
}

TEST_CASE("heartbeat detects a silent dead link") {
    World w(link::ConnMode::LanOnly);
    w.s.advance(1000);
    REQUIRE(w.cm->isUp());
    // Make the server stop answering without closing the stream.
    for (auto& c : w.srv.conns) c->t->setServer(nullptr);
    w.s.advance(60'000 * 4 + 10'000);
    CHECK(w.rec.downs >= 1);
}

TEST_CASE("unsolicited peer messages are delivered once") {
    World w;
    w.s.advance(1000);
    REQUIRE(w.srv.push("media.snapshot", {{"gen", 1}, {"sessions", proto::json::array()}}));
    w.s.advance(10);
    int snaps = 0;
    for (auto& e : w.rec.envs)
        if (e.type == "media.snapshot") ++snaps;
    CHECK(snaps == 1);
}

namespace {
// The phone of the bug report: DNS-SD announces its Wi-Fi address; transport.info
// also reports an address that accepts TCP but never speaks our protocol.
constexpr const char* kGood = "192.168.1.20";
constexpr const char* kBad = "10.64.0.9";

void announce(World& w, const char* host) { w.cm->onLanEndpoint(w.srv.info.id, {host, 47321, "mdns"}, true); }
void reportBad(World& w) {
    REQUIRE(w.srv.push("transport.info", {{"lan", proto::json::array({{{"host", kGood}, {"port", 47321}},
                                                                      {{"host", kBad}, {"port", 47321}}})}}));
    w.s.advance(100);
}
} // namespace

TEST_CASE("LAN: an address that accepts but never handshakes does not block the good one") {
    World w(link::ConnMode::LanOnly, false, false);
    w.fac.lanHostMode[kBad] = "silent";
    // Worst case ordering: the silent address is a manual endpoint, ranked before DNS-SD...
    w.peer.manualEndpoints.push_back({kBad, 47321, "manual"});
    w.start();
    w.s.advance(100);
    CHECK(w.fac.attemptsTo(kBad) == 1);
    announce(w, kGood); // ...and the good one only appears later
    w.s.advance(1500);
    CHECK(w.cm->isUp());
    CHECK(w.fac.attemptsTo(kGood) >= 1);
}

TEST_CASE("LAN: silent address raced against a good one (happy eyeballs)") {
    World w(link::ConnMode::LanOnly, false, false);
    w.fac.lanHostMode[kBad] = "silent";
    w.peer.manualEndpoints = {{kBad, 47321, "manual"}, {kGood, 47321, "manual"}};
    w.start();
    w.s.advance(1000); // bad gets a 300 ms head start, then good races it and wins
    CHECK(w.cm->isUp());
    CHECK(w.fac.attemptsTo(kBad) == 1);
    CHECK(w.fac.attemptsTo(kGood) == 1);
}

TEST_CASE("LAN: phone app crash and restart reconnects without restarting the host") {
    World w(link::ConnMode::LanOnly, false, false);
    w.fac.lanHostMode[kBad] = "silent";
    w.start();
    announce(w, kGood);
    w.s.advance(1000);
    REQUIRE(w.cm->isUp());
    reportBad(w);
    w.s.advance(60'000); // a stable link

    // The app crashes: connections are refused for 5 s while it restarts.
    w.fac.lanHostMode[kGood] = "refuse";
    w.srv.kill(TransportKind::Lan);
    w.s.advance(5'000);
    CHECK_FALSE(w.cm->isUp());
    w.fac.lanHostMode[kGood] = "ok";
    w.s.advance(6'000);
    CHECK(w.cm->isUp()); // within a few seconds of the app being back
    // The silent address was tried at most once and is demoted after its handshake times out.
    CHECK(w.fac.attemptsTo(kBad) <= 1);

    // Next drop: the silent address is not even tried while the good one works.
    int bad = w.fac.attemptsTo(kBad);
    w.srv.kill(TransportKind::Lan);
    w.s.advance(3'000);
    CHECK(w.cm->isUp());
    CHECK(w.fac.attemptsTo(kBad) == bad);
}

TEST_CASE("LAN: re-announcement over DNS-SD resets the back-off") {
    World w(link::ConnMode::LanOnly, false, false);
    w.start();
    w.fac.lanHostMode[kGood] = "refuse";
    announce(w, kGood);
    w.s.advance(70'000); // back-off has grown to its 30 s cap
    int before = w.fac.lanAttempts;
    w.fac.lanHostMode[kGood] = "ok";
    announce(w, kGood); // the restarted app advertises again
    w.s.advance(500);
    CHECK(w.fac.lanAttempts == before + 1);
    CHECK(w.cm->isUp());
}

TEST_CASE("LAN: unconfirmed peer-reported addresses expire") {
    World w(link::ConnMode::LanOnly, false, false);
    w.fac.lanHostMode[kBad] = "refuse";
    w.start();
    announce(w, kGood);
    w.s.advance(1000);
    REQUIRE(w.cm->isUp());
    reportBad(w);
    w.cm->onLanEndpoint(w.srv.info.id, {kGood, 47321, "mdns"}, false); // DNS-SD lost it
    w.fac.lanHostMode[kGood] = "refuse";
    w.srv.kill(TransportKind::Lan);
    w.s.advance(5 * 60'000);
    CHECK(w.fac.attemptsTo(kBad) >= 1); // still a candidate while fresh
    int bad = w.fac.attemptsTo(kBad);
    w.s.advance(10 * 60'000);
    int badLater = w.fac.attemptsTo(kBad);
    w.s.advance(5 * 60'000);
    CHECK(w.fac.attemptsTo(kBad) == badLater); // expired: no longer tried
    CHECK(badLater >= bad);
    CHECK(w.fac.attemptsTo(kGood) > 1);         // the address that worked is kept
}
