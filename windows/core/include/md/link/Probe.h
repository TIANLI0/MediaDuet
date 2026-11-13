#pragma once

#include "md/protocol/Envelope.h"
#include "md/protocol/Frame.h"
#include "md/util/Bytes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace md::link {

// Discovery probe (spec §3.5): who listens on a LAN address and whether its pairing window
// is open. Unauthenticated; it reveals no more than the DNS-SD TXT record (§1.2).
struct ProbeInfo {
    std::string id; // deviceId
    std::string name;
    std::string platform;
    bool pairing = false;
};

// The whole request: one HANDSHAKE frame sent right after the TCP connect.
Bytes encodeProbeRequest(const std::string& selfId, int64_t wallMs);

// probe.result -> ProbeInfo; nullopt for any other or malformed message.
std::optional<ProbeInfo> parseProbeResult(const proto::Envelope& e);

// Reads the answer to a probe from the raw byte stream.
class ProbeReader {
public:
    // Returns true once the answer is complete: a probe.result, any other first message
    // (servers before 1.11 answer `error`), or bytes that are not our protocol.
    bool feed(ByteView data);
    bool done() const { return done_; }
    // Set when the peer answered probe.result.
    const std::optional<ProbeInfo>& info() const { return info_; }
    // The peer spoke our framing (probe.result or another handshake message).
    bool speaksProtocol() const { return protocol_; }

private:
    proto::FrameDecoder dec_{static_cast<uint32_t>(proto::kMaxHandshakeBody + 1)};
    bool done_ = false;
    bool protocol_ = false;
    std::optional<ProbeInfo> info_;
};

// ----------------------------------------------------------------------- subnet scan

// IPv4 addresses are host byte order here.
std::optional<uint32_t> parseIpv4(std::string_view s);
std::string formatIpv4(uint32_t a);
// RFC 1918 (10/8, 172.16/12, 192.168/16).
bool isPrivateIpv4(uint32_t a);
// Hosts worth probing around `self` on a network with `prefix` bits: the whole network when
// it is a /24 or smaller, else only the /24 that contains `self` (a scan stays small and
// local). Never `self` nor the first / last address of that range. Empty for public
// addresses and prefixes shorter than /16 or longer than /30.
std::vector<uint32_t> scanHosts(uint32_t self, int prefix);
// The /24 network of an address (to match an endpoint against a local network).
inline uint32_t net24(uint32_t a) { return a & 0xFFFFFF00u; }

} // namespace md::link
