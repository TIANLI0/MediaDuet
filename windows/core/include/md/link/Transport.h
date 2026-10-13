#pragma once

#include "md/util/Bytes.h"

#include <memory>
#include <string>

namespace md::link {

enum class TransportKind { Bluetooth, Lan };

inline const char* toString(TransportKind k) { return k == TransportKind::Bluetooth ? "bt" : "lan"; }

// A connected, ordered, reliable byte stream (RFCOMM or TCP).
// All listener callbacks are delivered on the scheduler thread.
class ITransport {
public:
    struct Listener {
        virtual ~Listener() = default;
        virtual void onTransportData(ByteView data) = 0;
        // Called once all queued writes have been handed to the OS.
        virtual void onTransportWritable() = 0;
        // Called exactly once, after close() or on any I/O error / EOF.
        virtual void onTransportClosed(const std::string& reason) = 0;
    };
    virtual ~ITransport() = default;
    virtual TransportKind kind() const = 0;
    virtual std::string describe() const = 0;
    virtual std::string remoteHost() const { return {}; }
    virtual void start(Listener* listener) = 0;
    // Queues bytes for writing in order.
    virtual void send(Bytes data) = 0;
    // Number of queued writes that have not completed yet.
    virtual size_t pendingWrites() const = 0;
    virtual void close() = 0;
};

using TransportPtr = std::shared_ptr<ITransport>;

} // namespace md::link
