#pragma once

#include "md/link/Handshake.h"
#include "md/link/SecureChannel.h"
#include "md/link/Transport.h"
#include "md/protocol/Frame.h"
#include "md/util/Scheduler.h"

#include <deque>
#include <functional>
#include <memory>
#include <optional>

namespace md::link {

// One authenticated (or pairing) connection over a single transport.
// Owns framing, the handshake and record protection. Scheduler thread only.
class Connection : public std::enable_shared_from_this<Connection>, private ITransport::Listener {
public:
    enum class State { Idle, Handshaking, AwaitReady, Up, Pairing, Closed };

    struct Events {
        std::function<void(Connection&, const SessionClient::Established&)> onUp;
        std::function<void(Connection&, proto::Envelope)> onEnvelope;
        std::function<void(Connection&, proto::BlobChunk)> onBlob;
        // errorCode is the protocol error code when the peer refused us (e.g. not_paired).
        std::function<void(Connection&, const std::string& reason, const std::string& errorCode)> onClosed;
        std::function<void(Connection&, const std::string& sas, const proto::DeviceInfo& peer)> onSas;
        std::function<void(Connection&, const proto::DeviceInfo& peer, const Bytes& key)> onPaired;
    };

    static std::shared_ptr<Connection> create(IScheduler& sched, TransportPtr transport, Events events);
    ~Connection() override;

    void beginSession(const proto::DeviceInfo& self, const std::string& peerId, const Bytes& pairingKey,
                      std::vector<std::string> caps, const std::string& resumeSid,
                      std::function<crypto::EcdhKey()> keygen = nullptr);
    // qrToken: QR pairing (§3.5) instead of a SAS comparison.
    void beginPairing(const proto::DeviceInfo& self, Bytes qrToken = {});
    void pairingDecision(bool accept);

    // Up state only. Control records are sealed and written immediately.
    bool sendEnvelope(const proto::Envelope& e);
    // Bulk data, written only when no other write is pending.
    bool queueBlob(Bytes channelData);
    void close(const std::string& reason);

    State state() const { return state_; }
    TransportKind kind() const { return transport_->kind(); }
    std::string describe() const { return transport_->describe(); }
    std::string remoteHost() const { return transport_->remoteHost(); }
    const SessionClient::Established* session() const { return est_ ? &*est_ : nullptr; }
    int64_t lastReceiveMs() const { return lastRx_; }
    int64_t upSinceMs() const { return upSince_; }
    uint64_t bytesIn() const { return bytesIn_; }
    uint64_t bytesOut() const { return bytesOut_; }
    uint64_t id() const { return id_; }

private:
    Connection(IScheduler& sched, TransportPtr transport, Events events);
    void onTransportData(ByteView data) override;
    void onTransportWritable() override;
    void onTransportClosed(const std::string& reason) override;
    void handleFrame(proto::Frame&& f);
    void sendHandshake(const proto::Envelope& e);
    void pumpBlobs();
    void finish(const std::string& reason, const std::string& code);

    IScheduler& sched_;
    TransportPtr transport_;
    Events ev_;
    State state_ = State::Idle;
    proto::FrameDecoder decoder_;
    std::unique_ptr<SessionClient> session_;
    std::unique_ptr<PairingClient> pairing_;
    std::optional<SessionClient::Established> est_;
    std::unique_ptr<SecureChannel> channel_;
    std::deque<Bytes> blobs_;
    ScopedTimer handshakeTimer_;
    int64_t lastRx_ = 0;
    int64_t upSince_ = 0;
    uint64_t bytesIn_ = 0;
    uint64_t bytesOut_ = 0;
    uint64_t id_;
    std::string closeCode_;
    std::string closeReason_;
    bool closing_ = false;
    // Keeps the connection alive until the transport reports closure.
    std::shared_ptr<Connection> selfRef_;
};

using ConnectionPtr = std::shared_ptr<Connection>;

} // namespace md::link
