#pragma once

#include "md/crypto/Crypto.h"
#include "md/protocol/Envelope.h"
#include "md/protocol/Messages.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace md::link {

// ---- Key schedule (spec §3.3, §4). Pure functions, verified by test vectors.
namespace ks {

Bytes pairTranscript(ByteView nC, ByteView nS, ByteView epkC, ByteView epkS, std::string_view idC,
                     std::string_view idS);
Bytes sessionTranscript(ByteView nC, ByteView nS, ByteView epkC, ByteView epkS, std::string_view idC,
                        std::string_view idS);

struct PairingSecrets {
    Bytes prk;
    std::string sas; // 6 digits
    Bytes pairingKey;
};
PairingSecrets pairing(ByteView z, ByteView transcript);
Bytes confirmMac(ByteView pairingKey, char role, ByteView transcript);
// QR pairing (§3.5): proves knowledge of the QR code's one-time secret instead of a SAS comparison.
Bytes qrConfirmMac(ByteView qrToken, char role, ByteView transcript);

Bytes sessionMac(ByteView pairingKey, std::string_view label, ByteView transcript); // "server"/"client"

struct SessionKeys {
    Bytes keyC2S, keyS2C, ivC2S, ivS2C;
};
SessionKeys sessionKeys(ByteView pairingKey, ByteView z, ByteView transcript);

} // namespace ks

struct HandshakeFailure {
    std::string code;    // protocol error code or local reason
    std::string message;
};

// Client side of pairing (spec §3.3). With a QR token (§3.5) both sides prove they know it
// instead: no SAS, no user decision.
class PairingClient {
public:
    struct Callbacks {
        std::function<void(proto::Envelope)> send;
        std::function<void(const std::string& sas, const proto::DeviceInfo& peer)> onSas;
        std::function<void(const proto::DeviceInfo& peer, const Bytes& pairingKey)> onPaired;
        std::function<void(const HandshakeFailure&)> onFailed;
    };
    PairingClient(proto::DeviceInfo self, Callbacks cb, std::function<crypto::EcdhKey()> keygen = nullptr,
                  std::function<Bytes(size_t)> rng = nullptr, Bytes qrToken = {});

    void start(int64_t wallMs);
    void onEnvelope(const proto::Envelope& e);
    // Local user compared the SAS and accepted (true) or rejected (false).
    void userDecision(bool accept, int64_t wallMs);
    // Aborts with a reason (e.g. "timeout"), telling the peer via pair.result.
    void abort(const std::string& code, const std::string& message);
    bool done() const { return state_ == State::Done || state_ == State::Failed; }

private:
    enum class State { Idle, AwaitResponse, AwaitUser, AwaitPeerConfirm, AwaitResult, Done, Failed };
    void fail(std::string code, std::string msg, bool notifyPeer);
    void maybeFinish();
    proto::Envelope make(const char* type, proto::json body);

    proto::DeviceInfo self_;
    Callbacks cb_;
    std::optional<crypto::EcdhKey> eph_;
    Bytes nonce_;
    State state_ = State::Idle;
    uint64_t nextId_ = 1;
    int64_t wall_ = 0;
    proto::DeviceInfo peer_;
    Bytes transcript_;
    Bytes key_;
    bool userAccepted_ = false;
    bool peerConfirmed_ = false;
    std::function<Bytes(size_t)> rng_;
    Bytes qr_; // QR pairing secret (empty: SAS pairing)
};

// Client side of the session handshake (spec §4).
class SessionClient {
public:
    struct Established {
        proto::DeviceInfo peer;
        std::string sid;
        bool resumed = false;
        int protocol = 1;
        std::vector<std::string> caps;
        ks::SessionKeys keys;
    };
    struct Callbacks {
        std::function<void(proto::Envelope)> send;
        std::function<void(Established)> onEstablished; // after auth was sent
        std::function<void(const HandshakeFailure&)> onFailed;
    };
    SessionClient(proto::DeviceInfo self, std::string expectedPeerId, Bytes pairingKey, std::vector<std::string> caps,
                  std::string transport, std::string resumeSid, Callbacks cb,
                  std::function<crypto::EcdhKey()> keygen = nullptr, std::function<Bytes(size_t)> rng = nullptr);

    void start(int64_t wallMs);
    void onEnvelope(const proto::Envelope& e);
    bool finished() const { return state_ == State::Done || state_ == State::Failed; }

private:
    enum class State { Idle, AwaitAck, Done, Failed };
    void fail(std::string code, std::string msg);

    proto::DeviceInfo self_;
    std::string peerId_;
    Bytes key_;
    std::vector<std::string> caps_;
    std::string transport_;
    std::string resume_;
    Callbacks cb_;
    std::optional<crypto::EcdhKey> eph_;
    Bytes nonce_;
    State state_ = State::Idle;
    int64_t wall_ = 0;
};

} // namespace md::link
