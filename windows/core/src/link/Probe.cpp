#include "md/link/Probe.h"

#include "md/protocol/Messages.h"

#include <charconv>

namespace md::link {

Bytes encodeProbeRequest(const std::string& selfId, int64_t wallMs) {
    proto::Envelope e;
    e.type = proto::types::Probe;
    e.id = 1;
    e.ts = wallMs;
    e.src = selfId;
    std::string text = e.serialize();
    return proto::encodeFrame(proto::FrameType::Handshake, md::view(text));
}

std::optional<ProbeInfo> parseProbeResult(const proto::Envelope& e) {
    if (e.type != proto::types::ProbeResult || !e.sid.empty()) return std::nullopt;
    auto dev = e.body.find("device");
    if (dev == e.body.end()) return std::nullopt;
    auto d = proto::deviceInfoFromJson(*dev);
    if (!d || d->id != e.src) return std::nullopt;
    ProbeInfo info;
    info.id = d->id;
    info.name = d->name;
    info.platform = d->platform;
    if (auto p = e.body.find("pairing"); p != e.body.end() && p->is_boolean()) info.pairing = p->get<bool>();
    return info;
}

bool ProbeReader::feed(ByteView data) {
    if (done_) return true;
    dec_.feed(data);
    auto f = dec_.next();
    if (dec_.failed()) {
        done_ = true; // not our framing (or an oversized answer)
        return true;
    }
    if (!f) return false;
    done_ = true;
    if (f->type != proto::FrameType::Handshake) return true;
    auto e = proto::Envelope::parse(md::toString(f->body));
    if (!e) return true;
    protocol_ = true;
    info_ = parseProbeResult(*e);
    return true;
}

std::optional<uint32_t> parseIpv4(std::string_view s) {
    uint32_t out = 0;
    for (int i = 0; i < 4; ++i) {
        unsigned v = 0;
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        if (ec != std::errc() || v > 255 || p == s.data() || p - s.data() > 3) return std::nullopt;
        out = (out << 8) | v;
        s.remove_prefix(static_cast<size_t>(p - s.data()));
        if (i < 3) {
            if (s.empty() || s.front() != '.') return std::nullopt;
            s.remove_prefix(1);
        }
    }
    if (!s.empty()) return std::nullopt;
    return out;
}

std::string formatIpv4(uint32_t a) {
    return std::to_string(a >> 24) + "." + std::to_string((a >> 16) & 0xFF) + "." + std::to_string((a >> 8) & 0xFF) +
           "." + std::to_string(a & 0xFF);
}

bool isPrivateIpv4(uint32_t a) {
    return (a >> 24) == 10 || (a >> 20) == ((172u << 4) | 1u) || (a >> 16) == ((192u << 8) | 168u);
}

std::vector<uint32_t> scanHosts(uint32_t self, int prefix) {
    std::vector<uint32_t> out;
    if (!isPrivateIpv4(self) || prefix < 16 || prefix > 30) return out;
    int bits = prefix < 24 ? 24 : prefix;
    uint32_t mask = 0xFFFFFFFFu << (32 - bits);
    uint32_t net = self & mask, bcast = net | ~mask;
    // On a network wider than /24 the .0 / .255 of our /24 are ordinary hosts, but DHCP
    // servers practically never hand them out: they are skipped like real edges.
    for (uint32_t a = net + 1; a < bcast; ++a)
        if (a != self) out.push_back(a);
    return out;
}

} // namespace md::link
