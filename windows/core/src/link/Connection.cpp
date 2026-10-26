#include "md/link/Connection.h"

#include "md/util/Log.h"

#include <atomic>

namespace md::link {

namespace {
constexpr int64_t kSessionHandshakeTimeoutMs = 10'000;
constexpr int64_t kPairingTimeoutMs = 120'000;
std::atomic<uint64_t> g_nextConnId{1};
} // namespace

std::shared_ptr<Connection> Connection::create(IScheduler& sched, TransportPtr transport, Events events) {
    return std::shared_ptr<Connection>(new Connection(sched, std::move(transport), std::move(events)));
}

Connection::Connection(IScheduler& sched, TransportPtr transport, Events events)
    : sched_(sched), transport_(std::move(transport)), ev_(std::move(events)), handshakeTimer_(&sched),
      id_(g_nextConnId++) {}

Connection::~Connection() = default;

// The transport keeps a raw listener pointer, so the connection keeps itself alive
// through selfRef_ until the transport reports closure.
void Connection::beginSession(const proto::DeviceInfo& self, const std::string& peerId, const Bytes& pairingKey,
                              std::vector<std::string> caps, const std::string& resumeSid,
                              std::function<crypto::EcdhKey()> keygen) {
    selfRef_ = shared_from_this();
    state_ = State::Handshaking;
    lastRx_ = sched_.nowMs();
    SessionClient::Callbacks cb;
    cb.send = [this](proto::Envelope e) { sendHandshake(e); };
    cb.onEstablished = [this](SessionClient::Established est) {
        const auto& k = est.keys;
        channel_ = std::make_unique<SecureChannel>(k.keyC2S, k.ivC2S, k.keyS2C, k.ivS2C);
        est_ = std::move(est);
        state_ = State::AwaitReady;
    };
    cb.onFailed = [this](const HandshakeFailure& f) {
        log::warn("conn", "#{} session handshake failed: {} {}", id_, f.code, f.message);
        finish("handshake failed: " + f.code, f.code);
    };
    session_ = std::make_unique<SessionClient>(self, peerId, pairingKey, std::move(caps),
                                               toString(transport_->kind()), resumeSid, std::move(cb),
                                               std::move(keygen));
    handshakeTimer_.start(kSessionHandshakeTimeoutMs, [this] { finish("handshake timeout", "timeout"); });
    transport_->start(this);
    session_->start(sched_.wallMs());
}

void Connection::beginPairing(const proto::DeviceInfo& self, Bytes qrToken) {
    selfRef_ = shared_from_this();
    state_ = State::Pairing;
    lastRx_ = sched_.nowMs();
    PairingClient::Callbacks cb;
    cb.send = [this](proto::Envelope e) { sendHandshake(e); };
    cb.onSas = [this](const std::string& sas, const proto::DeviceInfo& peer) {
        if (ev_.onSas) ev_.onSas(*this, sas, peer);
    };
    cb.onPaired = [this](const proto::DeviceInfo& peer, const Bytes& key) {
        handshakeTimer_.cancel();
        if (ev_.onPaired) ev_.onPaired(*this, peer, key);
        close("paired");
    };
    cb.onFailed = [this](const HandshakeFailure& f) {
        log::warn("conn", "#{} pairing failed: {} {}", id_, f.code, f.message);
        finish("pairing failed: " + f.code, f.code);
    };
    pairing_ = std::make_unique<PairingClient>(self, std::move(cb), nullptr, nullptr, std::move(qrToken));
    handshakeTimer_.start(kPairingTimeoutMs, [this] {
        if (pairing_ && !pairing_->done()) pairing_->abort("timeout", "pairing was not confirmed in time");
        finish("pairing timeout", "timeout");
    });
    transport_->start(this);
    pairing_->start(sched_.wallMs());
}

void Connection::pairingDecision(bool accept) {
    if (state_ == State::Pairing && pairing_) pairing_->userDecision(accept, sched_.wallMs());
}

void Connection::sendHandshake(const proto::Envelope& e) {
    std::string s = e.serialize();
    Bytes frame = proto::encodeFrame(proto::FrameType::Handshake, view(s));
    bytesOut_ += frame.size();
    transport_->send(std::move(frame));
}

bool Connection::sendEnvelope(const proto::Envelope& e) {
    if (state_ != State::Up || !channel_) return false;
    std::string s = e.serialize();
    if (s.size() > proto::kMaxControlJson) {
        log::error("conn", "#{} control message {} too large ({} bytes)", id_, e.type, s.size());
        return false;
    }
    try {
        Bytes frame = channel_->sealFrame(proto::Channel::Control, view(s));
        bytesOut_ += frame.size();
        transport_->send(std::move(frame));
    } catch (const std::exception& ex) {
        finish(std::string("seal failed: ") + ex.what(), "internal");
        return false;
    }
    return true;
}

bool Connection::queueBlob(Bytes channelData) {
    if (state_ != State::Up) return false;
    blobs_.push_back(std::move(channelData));
    pumpBlobs();
    return true;
}

void Connection::pumpBlobs() {
    // Seal lazily so control records sent in between keep their sequence order.
    while (state_ == State::Up && !blobs_.empty() && transport_->pendingWrites() == 0) {
        Bytes frame = channel_->sealFrame(proto::Channel::Blob, blobs_.front());
        blobs_.pop_front();
        bytesOut_ += frame.size();
        transport_->send(std::move(frame));
    }
}

void Connection::onTransportWritable() { pumpBlobs(); }

void Connection::close(const std::string& reason) {
    if (state_ == State::Closed || closing_) return;
    closing_ = true;
    if (closeReason_.empty()) closeReason_ = reason;
    transport_->close();
}

void Connection::finish(const std::string& reason, const std::string& code) {
    if (closeCode_.empty()) closeCode_ = code;
    close(reason);
}

void Connection::onTransportData(ByteView data) {
    if (state_ == State::Closed) return;
    bytesIn_ += data.size();
    lastRx_ = sched_.nowMs();
    decoder_.feed(data);
    while (state_ != State::Closed && !closing_) {
        auto f = decoder_.next();
        if (!f) break;
        handleFrame(std::move(*f));
    }
    if (decoder_.failed()) finish(std::string("framing error: ") + decoder_.error(), "bad_message");
}

void Connection::handleFrame(proto::Frame&& f) {
    using proto::FrameType;
    if (f.type == FrameType::Handshake) {
        auto env = proto::Envelope::parse(md::toString(f.body));
        if (!env) return finish("invalid handshake message", "bad_message");
        if (state_ == State::Handshaking && session_) return session_->onEnvelope(*env);
        if (state_ == State::Pairing && pairing_) return pairing_->onEnvelope(*env);
        // The server refuses `auth` in plaintext before it derives keys.
        if (state_ == State::AwaitReady && env->type == proto::types::Error) {
            std::string code = env->body.value("code", std::string("auth_failed"));
            return finish("rejected by peer: " + code, code);
        }
        return finish("unexpected handshake frame", "protocol");
    }
    if (f.type != FrameType::Secure || !channel_ || (state_ != State::AwaitReady && state_ != State::Up))
        return finish("unexpected frame type", "protocol");
    auto rec = channel_->openFrameBody(f.body);
    if (!rec) return finish("record authentication failed", "auth_failed");
    if (rec->channel == proto::Channel::Blob) {
        if (state_ != State::Up) return finish("blob before ready", "protocol");
        auto chunk = proto::decodeBlobChunk(rec->data);
        if (!chunk) return finish("invalid blob chunk", "bad_message");
        if (ev_.onBlob) ev_.onBlob(*this, std::move(*chunk));
        return;
    }
    auto env = proto::Envelope::parse(md::toString(rec->data));
    if (!env) {
        log::warn("conn", "#{} dropping invalid control message", id_);
        return;
    }
    if (est_ && env->src != est_->peer.id) return finish("source id mismatch", "auth_failed");
    if (state_ == State::AwaitReady) {
        if (env->type != proto::types::SessionReady) return finish("expected session.ready", "protocol");
        if (env->body.value("sid", std::string()) != est_->sid) return finish("sid mismatch", "protocol");
        handshakeTimer_.cancel();
        state_ = State::Up;
        upSince_ = sched_.nowMs();
        if (ev_.onUp) ev_.onUp(*this, *est_);
        return;
    }
    if (ev_.onEnvelope) ev_.onEnvelope(*this, std::move(*env));
}

void Connection::onTransportClosed(const std::string& reason) {
    if (state_ == State::Closed) return;
    state_ = State::Closed;
    handshakeTimer_.cancel();
    blobs_.clear();
    channel_.reset();
    std::string why = closeReason_.empty() ? reason : closeReason_;
    auto keep = std::move(selfRef_);
    if (ev_.onClosed) ev_.onClosed(*this, why, closeCode_);
}

} // namespace md::link
