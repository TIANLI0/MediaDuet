#include "md/link/Handshake.h"

#include "md/util/Log.h"

#include <cstdio>

namespace md::link {

using proto::Envelope;
using proto::json;

namespace ks {

static Bytes transcript(std::string_view label, ByteView nC, ByteView nS, ByteView epkC, ByteView epkS,
                        std::string_view idC, std::string_view idS) {
    return crypto::sha256(concat({view(label), nC, nS, epkC, epkS, view(idC), view(idS)}));
}

Bytes pairTranscript(ByteView nC, ByteView nS, ByteView epkC, ByteView epkS, std::string_view idC,
                     std::string_view idS) {
    return transcript("MediaDuet-pair-v1", nC, nS, epkC, epkS, idC, idS);
}

Bytes sessionTranscript(ByteView nC, ByteView nS, ByteView epkC, ByteView epkS, std::string_view idC,
                        std::string_view idS) {
    return transcript("MediaDuet-session-v1", nC, nS, epkC, epkS, idC, idS);
}

PairingSecrets pairing(ByteView z, ByteView th) {
    PairingSecrets p;
    p.prk = crypto::hkdfExtract(th, z);
    Bytes s = crypto::hkdfExpand(p.prk, view(std::string_view("sas")), 4);
    uint32_t v = getU32BE(s.data()) % 1000000u;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06u", v);
    p.sas = buf;
    p.pairingKey = crypto::hkdfExpand(p.prk, view(std::string_view("pairing key")), 32);
    return p;
}

Bytes confirmMac(ByteView key, char role, ByteView th) {
    std::string label = "confirm";
    label += role;
    return crypto::hmacSha256(key, concat({view(label), th}));
}

Bytes sessionMac(ByteView key, std::string_view label, ByteView th) {
    return crypto::hmacSha256(key, concat({view(label), th}));
}

SessionKeys sessionKeys(ByteView pairingKey, ByteView z, ByteView th) {
    Bytes prk = crypto::hkdfExtract(pairingKey, z);
    auto info = [&](std::string_view label) { return concat({view(label), th}); };
    SessionKeys k;
    k.keyC2S = crypto::hkdfExpand(prk, info("c2s key"), 32);
    k.keyS2C = crypto::hkdfExpand(prk, info("s2c key"), 32);
    k.ivC2S = crypto::hkdfExpand(prk, info("c2s iv"), 4);
    k.ivS2C = crypto::hkdfExpand(prk, info("s2c iv"), 4);
    secureZero(prk);
    return k;
}

} // namespace ks

static std::string errorCodeOf(const Envelope& e) {
    if (auto c = e.body.find("code"); c != e.body.end() && c->is_string()) return c->get<std::string>();
    return "error";
}

// ======================================================================= PairingClient

PairingClient::PairingClient(proto::DeviceInfo self, Callbacks cb, std::function<crypto::EcdhKey()> keygen,
                             std::function<Bytes(size_t)> rng)
    : self_(std::move(self)), cb_(std::move(cb)), rng_(rng ? std::move(rng) : crypto::randomBytes) {
    eph_ = keygen ? keygen() : crypto::EcdhKey::generate();
    nonce_ = rng_(32);
}

Envelope PairingClient::make(const char* type, json body) {
    Envelope e;
    e.type = type;
    e.id = nextId_++;
    e.ts = wall_;
    e.src = self_.id;
    e.body = std::move(body);
    return e;
}

void PairingClient::start(int64_t wallMs) {
    wall_ = wallMs;
    state_ = State::AwaitResponse;
    cb_.send(make(proto::types::PairRequest, {{"device", proto::toJson(self_)},
                                              {"epk", base64Encode(eph_->publicKey())},
                                              {"nonce", base64Encode(nonce_)}}));
}

void PairingClient::fail(std::string code, std::string msg, bool notifyPeer) {
    if (state_ == State::Failed || state_ == State::Done) return;
    state_ = State::Failed;
    secureZero(key_);
    if (notifyPeer) cb_.send(make(proto::types::PairResult, {{"ok", false}, {"reason", code}}));
    if (cb_.onFailed) cb_.onFailed({std::move(code), std::move(msg)});
}

void PairingClient::onEnvelope(const Envelope& e) {
    if (state_ == State::Done || state_ == State::Failed) return;
    if (e.type == proto::types::Error) return fail(errorCodeOf(e), "peer error", false);
    if (e.type == proto::types::PairResult) {
        bool ok = e.body.value("ok", false);
        if (!ok) return fail(e.body.value("reason", std::string("rejected")), "peer rejected pairing", false);
        if (state_ != State::AwaitResult) return fail("protocol", "unexpected pair.result", true);
        state_ = State::Done;
        if (cb_.onPaired) cb_.onPaired(peer_, key_);
        return;
    }
    if (e.type == proto::types::PairResponse) {
        if (state_ != State::AwaitResponse) return fail("protocol", "unexpected pair.response", true);
        auto peer = proto::deviceInfoFromJson(e.body.value("device", json::object()));
        auto epk = proto::getB64(e.body, "epk", 65);
        auto nonce = proto::getB64(e.body, "nonce", 32);
        if (!peer || !epk || !nonce || peer->id != e.src || peer->id == self_.id)
            return fail("bad_message", "invalid pair.response", true);
        peer_ = *peer;
        Bytes z;
        try {
            z = eph_->agree(*epk);
        } catch (const std::exception& ex) {
            return fail("bad_message", ex.what(), true);
        }
        transcript_ = ks::pairTranscript(nonce_, *nonce, eph_->publicKey(), *epk, self_.id, peer_.id);
        auto secrets = ks::pairing(z, transcript_);
        secureZero(z);
        key_ = secrets.pairingKey;
        state_ = State::AwaitUser;
        if (cb_.onSas) cb_.onSas(secrets.sas, peer_);
        return;
    }
    if (e.type == proto::types::PairConfirm) {
        if (state_ != State::AwaitUser && state_ != State::AwaitPeerConfirm)
            return fail("protocol", "unexpected pair.confirm", true);
        auto mac = proto::getB64(e.body, "mac", 32);
        if (!mac || !crypto::constantTimeEqual(*mac, ks::confirmMac(key_, 'S', transcript_)))
            return fail("mac_mismatch", "pairing confirmation does not match", true);
        peerConfirmed_ = true;
        maybeFinish();
        return;
    }
    fail("protocol", "unexpected message " + e.type, true);
}

void PairingClient::userDecision(bool accept, int64_t wallMs) {
    wall_ = wallMs;
    if (state_ != State::AwaitUser) return;
    if (!accept) return fail("rejected", "rejected by local user", true);
    userAccepted_ = true;
    cb_.send(make(proto::types::PairConfirm, {{"mac", base64Encode(ks::confirmMac(key_, 'C', transcript_))}}));
    state_ = State::AwaitPeerConfirm;
    maybeFinish();
}

void PairingClient::abort(const std::string& code, const std::string& message) { fail(code, message, true); }

void PairingClient::maybeFinish() {
    if (userAccepted_ && peerConfirmed_ && state_ == State::AwaitPeerConfirm) state_ = State::AwaitResult;
}

// ======================================================================= SessionClient

SessionClient::SessionClient(proto::DeviceInfo self, std::string expectedPeerId, Bytes pairingKey,
                             std::vector<std::string> caps, std::string transport, std::string resumeSid,
                             Callbacks cb, std::function<crypto::EcdhKey()> keygen,
                             std::function<Bytes(size_t)> rng)
    : self_(std::move(self)), peerId_(std::move(expectedPeerId)), key_(std::move(pairingKey)),
      caps_(std::move(caps)), transport_(std::move(transport)), resume_(std::move(resumeSid)), cb_(std::move(cb)) {
    eph_ = keygen ? keygen() : crypto::EcdhKey::generate();
    nonce_ = rng ? rng(32) : crypto::randomBytes(32);
}

void SessionClient::start(int64_t wallMs) {
    wall_ = wallMs;
    state_ = State::AwaitAck;
    Envelope e;
    e.type = proto::types::Hello;
    e.id = 1;
    e.ts = wall_;
    e.src = self_.id;
    e.body = {{"device", proto::toJson(self_)},
              {"protocol", {{"min", proto::kProtocolVersion}, {"max", proto::kProtocolVersion}}},
              {"caps", caps_},
              {"epk", base64Encode(eph_->publicKey())},
              {"nonce", base64Encode(nonce_)},
              {"transport", transport_}};
    if (!resume_.empty()) e.body["resume"] = resume_;
    cb_.send(std::move(e));
}

void SessionClient::fail(std::string code, std::string msg) {
    if (state_ == State::Failed || state_ == State::Done) return;
    state_ = State::Failed;
    if (cb_.onFailed) cb_.onFailed({std::move(code), std::move(msg)});
}

void SessionClient::onEnvelope(const Envelope& e) {
    if (state_ != State::AwaitAck) return;
    if (e.type == proto::types::Error) return fail(errorCodeOf(e), e.body.value("message", std::string()));
    if (e.type != proto::types::HelloAck) return fail("protocol", "unexpected " + e.type);
    auto peer = proto::deviceInfoFromJson(e.body.value("device", json::object()));
    auto epk = proto::getB64(e.body, "epk", 65);
    auto nonce = proto::getB64(e.body, "nonce", 32);
    auto mac = proto::getB64(e.body, "mac", 32);
    auto sidIt = e.body.find("sid");
    if (!peer || !epk || !nonce || !mac || sidIt == e.body.end() || !sidIt->is_string() ||
        !proto::isValidDeviceId(sidIt->get_ref<const std::string&>()))
        return fail("bad_message", "invalid hello.ack");
    if (peer->id != peerId_ || e.src != peerId_) return fail("auth_failed", "unexpected peer identity");
    int version = e.body.value("protocol", 0);
    if (version != proto::kProtocolVersion) return fail("version_unsupported", "peer chose unsupported version");

    Bytes th = ks::sessionTranscript(nonce_, *nonce, eph_->publicKey(), *epk, self_.id, peerId_);
    if (!crypto::constantTimeEqual(*mac, ks::sessionMac(key_, "server", th)))
        return fail("auth_failed", "server authentication failed");
    Bytes z;
    try {
        z = eph_->agree(*epk);
    } catch (const std::exception& ex) {
        return fail("bad_message", ex.what());
    }
    Established est;
    est.peer = *peer;
    est.sid = sidIt->get<std::string>();
    est.resumed = e.body.value("resumed", false);
    est.protocol = version;
    if (auto c = e.body.find("caps"); c != e.body.end() && c->is_array())
        for (auto& x : *c)
            if (x.is_string() && est.caps.size() < 64) est.caps.push_back(x.get<std::string>());
    est.keys = ks::sessionKeys(key_, z, th);
    secureZero(z);

    Envelope auth;
    auth.type = proto::types::Auth;
    auth.id = 2;
    auth.ts = wall_;
    auth.src = self_.id;
    auth.body = {{"mac", base64Encode(ks::sessionMac(key_, "client", th))}};
    cb_.send(std::move(auth));
    state_ = State::Done;
    secureZero(key_);
    cb_.onEstablished(std::move(est));
}

} // namespace md::link
