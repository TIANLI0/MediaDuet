#pragma once

#include "md/link/Connection.h"
#include "md/link/LinkSession.h"
#include "md/media/MediaTypes.h"
#include "md/util/Scheduler.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace md::link {

struct LanEndpoint {
    std::string host;
    uint16_t port = 0;
    std::string source; // "mdns", "manual", "peer"
    bool operator==(const LanEndpoint& o) const { return host == o.host && port == o.port; }
    std::string str() const;
};

struct PeerRecord {
    std::string id;
    std::string name;
    std::string platform;
    uint64_t btAddress = 0; // 0 = unknown
    std::vector<LanEndpoint> manualEndpoints;
    Bytes key;              // pairing key (in memory only)
};

enum class ConnMode { Auto, BluetoothOnly, LanOnly };
const char* toString(ConnMode m);
ConnMode connModeFromString(std::string_view s);

class ITransportFactory {
public:
    using Done = std::function<void(TransportPtr transport, std::string error)>;
    virtual ~ITransportFactory() = default;
    // Async; `done` must be invoked exactly once on the scheduler thread.
    virtual void connectBluetooth(const PeerRecord& peer, Done done) = 0;
    virtual void connectLan(const LanEndpoint& ep, Done done) = 0;
};

struct LinkStatus {
    enum class State { NoPeer, Disconnected, Connecting, Connected };
    State state = State::NoPeer;
    std::string peerId, peerName, peerPlatform;
    std::string peerApp; // the peer's app version (DeviceInfo.app) from this session; empty = not known yet
    std::optional<TransportKind> primary, secondary;
    int64_t sinceMs = 0;
    int64_t rttMs = -1;
    uint64_t bytesIn = 0, bytesOut = 0;
    uint64_t msgsIn = 0, msgsOut = 0;
    std::string lastError;
    std::vector<std::string> peerCaps;
};

// Client-side link policy (spec §1, §7, §11): Auto = LAN first (lower latency),
// Bluetooth as a hot standby / fallback, stable switch-back, exponential back-off with jitter, heartbeats, de-dup.
class ConnectionManager {
public:
    struct Listener {
        virtual ~Listener() = default;
        virtual void onLinkUp(const std::string& peerId, bool resumed) = 0;
        virtual void onLinkDown(const std::string& peerId) = 0;
        // De-duplicated incoming control message. connId identifies the transport for replies.
        virtual void onEnvelope(const std::string& peerId, const proto::Envelope& e, uint64_t connId) = 0;
        virtual void onBlob(const std::string& peerId, const proto::BlobChunk& c) = 0;
        virtual void onPeerRevoked(const std::string& peerId) = 0;
        virtual void onStatusChanged() = 0;
        virtual void onPeerEndpoints(const std::string& peerId, const std::vector<LanEndpoint>& eps) {}
    };

    struct Config {
        ConnMode mode = ConnMode::Auto;
        int64_t switchBackDelayMs = 10'000;
        int64_t btBackoffBaseMs = 2'000, btBackoffMaxMs = 60'000;
        int64_t lanBackoffBaseMs = 1'000, lanBackoffMaxMs = 30'000;
        int64_t pingActiveMs = 15'000, pingIdleMs = 60'000, pingSecondaryMs = 120'000;
        int64_t resendMaxAgeMs = 5'000;
        int64_t nextEndpointDelayMs = 250;        // next LAN candidate after a connect failure
        int64_t raceDelayMs = 300;                // head start of a LAN handshake before the next candidate races it
        int64_t endpointCooldownMs = 5 * 60'000;  // after a handshake failure on an endpoint
        int64_t peerEndpointTtlMs = 10 * 60'000;  // unconfirmed transport.info endpoints
    };

    ConnectionManager(IScheduler& sched, ITransportFactory& factory, Listener& listener, proto::DeviceInfo self,
                      std::vector<std::string> caps, Config cfg = {});
    ~ConnectionManager();

    void setPeers(std::vector<PeerRecord> peers);
    void setMode(ConnMode mode);
    void setSwitchBackDelay(int64_t ms);
    void setMediaActive(bool active);
    void setJitterSeed(uint32_t seed) { rng_.seed(seed); }

    // Platform events.
    void onBluetoothRadio(bool on);
    void onPeerBluetoothPresence(const std::string& peerId, bool connected);
    void onLanEndpoint(const std::string& peerId, const LanEndpoint& ep, bool available);
    void onNetworkChanged();

    // User actions.
    void connectNow(const std::string& peerId);
    void disconnect(const std::string& peerId);

    // Sending (active peer). Return the message id, or 0 if no link is up.
    uint64_t send(const std::string& type, proto::json body, std::optional<uint64_t> re = std::nullopt);
    uint64_t sendVia(uint64_t connId, const std::string& type, proto::json body, std::optional<uint64_t> re);
    uint64_t sendCommand(const media::MediaCommand& cmd, LinkSession::ResultFn fn);
    // Bulk data: prefers a LAN connection, else the given connection, else primary.
    bool sendBlob(Bytes channelData, uint64_t preferConnId = 0);
    // Replies to a duplicate command with the cached result (spec §7).
    void cacheResult(uint64_t requestId, const proto::Envelope& result);

    bool isUp() const;
    std::optional<std::string> activePeer() const;
    LinkStatus status() const;
    std::string remoteLanHost() const; // host of an up LAN connection, if any

    // Where to stream audio (spec §9): prefers an up LAN connection; otherwise the
    // primary plus a known LAN address. keyC2S is that connection's client->server key.
    struct AudioRoute {
        uint64_t connId = 0;
        Bytes keyC2S;
        std::string host;
    };
    std::optional<AudioRoute> audioRoute() const;
    const std::vector<std::string>* peerCaps() const;

private:
    struct Slot {
        TransportKind kind;
        ConnectionPtr conn;
        bool connecting = false;
        int failures = 0;
        ScopedTimer retry;
        ScopedTimer heartbeat;
        // LAN only. A retry cycle walks the ranked candidates (fixed when the cycle
        // starts); transports still in their handshake race each other ("happy
        // eyeballs") and never block the next candidate or the next cycle. `conn`
        // is only set once a LAN connection is Up.
        std::vector<LanEndpoint> cycle;
        size_t endpointCursor = 0;
        struct Pending {
            ConnectionPtr conn;
            LanEndpoint ep;
        };
        std::vector<Pending> pending; // in handshake
        int inFlight = 0;             // TCP connects not answered yet
        std::optional<LanEndpoint> currentEp; // endpoint of the Up connection
        int64_t upSince = 0;
        bool peerAskedBackoff = false;
    };
    struct Peer {
        PeerRecord rec;
        std::string app; // DeviceInfo.app reported in this run (hello.ack / device.info)
        Slot bt{TransportKind::Bluetooth};
        Slot lan{TransportKind::Lan};
        Connection* primary = nullptr;
        LinkSession session;
        bool userDisconnected = false;
        bool revoked = false;
        bool btPresent = false;
        int authFailures = 0;
        std::vector<LanEndpoint> mdns;     // currently announced via DNS-SD (newest first)
        std::vector<LanEndpoint> reported; // from the phone's transport.info
        std::optional<LanEndpoint> lastGood; // last endpoint that reached Up
        struct EpInfo {
            int64_t reportedAt = -1; // last transport.info mentioning it
            int64_t lastUp = -1;
            int64_t coolUntil = -1;  // demoted after a failed handshake
        };
        std::map<std::string, EpInfo> eps; // by LanEndpoint::str()
        int64_t announceResetAt = -1;      // DNS-SD (re-)announcement reset the back-off (rate limited)
        std::vector<int64_t> lanDrops;
        ScopedTimer promote;
        int64_t rttMs = -1;
        std::vector<std::string> caps;
        std::string lastError;
        uint64_t msgsIn = 0, msgsOut = 0;
        Peer(IScheduler& s, const std::string& self) : session(s, self) {}
    };

    Peer* find(const std::string& id);
    const Peer* activePeerPtr() const;
    Slot& slot(Peer& p, TransportKind k) { return k == TransportKind::Bluetooth ? p.bt : p.lan; }
    bool allowed(TransportKind k) const;
    // Ranked LAN candidates: last endpoint that reached Up, DNS-SD, manual, then
    // peer-reported (expired after peerEndpointTtlMs unless confirmed); endpoints
    // in handshake cool-down are skipped unless nothing else is left.
    std::vector<LanEndpoint> lanCandidates(const Peer& p) const;
    void attemptLan(Peer& p);
    // Continues the LAN cycle after `delay` (next candidate) or ends it (back-off).
    void lanAdvance(Peer& p, Slot& s, int64_t delay);
    bool lanBusy(const Slot& s) const { return s.inFlight > 0 || !s.pending.empty(); }
    void closePending(Slot& s, const std::string& why);
    void evaluateAll();
    void evaluate(Peer& p);
    void attempt(Peer& p, TransportKind k);
    void scheduleRetry(Peer& p, Slot& s);
    void onConnUp(Peer& p, Slot& s, Connection& c, const SessionClient::Established& est);
    void onConnClosed(const std::string& peerId, TransportKind k, uint64_t connId, const std::string& reason,
                      const std::string& code);
    void onConnEnvelope(Peer& p, Connection& c, proto::Envelope e);
    void choosePrimary(Peer& p);
    void setPrimary(Peer& p, Connection* c);
    void armHeartbeat(Peer& p, Slot& s);
    int64_t pingInterval(const Peer& p, const Slot& s) const;
    int64_t requiredLanStability(Peer& p);
    bool otherPeerActive(const Peer& p) const;
    Connection* connById(Peer& p, uint64_t id);

    IScheduler& sched_;
    ITransportFactory& factory_;
    Listener& listener_;
    proto::DeviceInfo self_;
    std::vector<std::string> caps_;
    Config cfg_;
    bool btRadio_ = true;
    bool mediaActive_ = false;
    std::map<std::string, std::unique_ptr<Peer>> peers_;
    std::mt19937 rng_{std::random_device{}()};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace md::link
