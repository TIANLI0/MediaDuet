#include "md/link/Probe.h"

#include <doctest/doctest.h>

using namespace md;
using namespace md::link;

namespace {
constexpr const char* kPc = "0123456789abcdef0123456789abcdef";
constexpr const char* kPhone = "fedcba9876543210fedcba9876543210";

Bytes frameOf(const proto::Envelope& e) { return proto::encodeFrame(proto::FrameType::Handshake, md::view(e.serialize())); }

proto::Envelope result(bool pairing) {
    proto::Envelope e;
    e.type = proto::types::ProbeResult;
    e.id = 1;
    e.ts = 1;
    e.src = kPhone;
    e.re = 1;
    e.body = {{"device", {{"id", kPhone}, {"name", "Pixel 9"}, {"platform", "android"}}}, {"pairing", pairing}};
    return e;
}
} // namespace

TEST_CASE("probe request is one handshake frame with an empty session id") {
    Bytes req = encodeProbeRequest(kPc, 1760000000000);
    proto::FrameDecoder dec;
    dec.feed(req);
    auto f = dec.next();
    REQUIRE(f);
    CHECK(f->type == proto::FrameType::Handshake);
    CHECK(dec.buffered() == 0);
    auto e = proto::Envelope::parse(md::toString(f->body));
    REQUIRE(e);
    CHECK(e->type == "probe");
    CHECK(e->src == kPc);
    CHECK(e->sid.empty());
    CHECK(e->body.is_object());
}

TEST_CASE("probe.result is read even when it arrives byte by byte") {
    Bytes bytes = frameOf(result(true));
    ProbeReader r;
    for (size_t i = 0; i + 1 < bytes.size(); ++i) CHECK_FALSE(r.feed(ByteView(&bytes[i], 1)));
    CHECK(r.feed(ByteView(&bytes.back(), 1)));
    REQUIRE(r.info());
    CHECK(r.speaksProtocol());
    CHECK(r.info()->id == kPhone);
    CHECK(r.info()->name == "Pixel 9");
    CHECK(r.info()->platform == "android");
    CHECK(r.info()->pairing);
}

TEST_CASE("an older phone answers error: reachable, identity unknown") {
    proto::Envelope err;
    err.type = proto::types::Error;
    err.id = 1;
    err.ts = 1;
    err.src = kPhone;
    err.body = {{"code", "bad_message"}};
    ProbeReader r;
    CHECK(r.feed(frameOf(err)));
    CHECK(r.speaksProtocol());
    CHECK_FALSE(r.info());
}

TEST_CASE("something else on the port is not mistaken for a phone") {
    ProbeReader r;
    std::string http = "HTTP/1.1 400 Bad Request\r\n\r\n";
    CHECK(r.feed(md::view(http)));
    CHECK_FALSE(r.speaksProtocol());
    CHECK_FALSE(r.info());
}

TEST_CASE("probe.result whose device does not match its sender is rejected") {
    auto e = result(false);
    e.body["device"]["id"] = std::string(kPc);
    CHECK_FALSE(parseProbeResult(e));
    auto ok = result(false);
    REQUIRE(parseProbeResult(ok));
    CHECK_FALSE(parseProbeResult(ok)->pairing);
}

TEST_CASE("IPv4 parsing and private ranges") {
    CHECK(parseIpv4("192.168.1.20") == 0xC0A80114u);
    CHECK(formatIpv4(0xC0A80114u) == "192.168.1.20");
    CHECK_FALSE(parseIpv4("192.168.1"));
    CHECK_FALSE(parseIpv4("192.168.1.256"));
    CHECK_FALSE(parseIpv4("192.168.1.2x"));
    CHECK_FALSE(parseIpv4("fe80::1"));
    CHECK(isPrivateIpv4(*parseIpv4("10.1.2.3")));
    CHECK(isPrivateIpv4(*parseIpv4("172.20.0.5")));
    CHECK_FALSE(isPrivateIpv4(*parseIpv4("172.32.0.5")));
    CHECK(isPrivateIpv4(*parseIpv4("192.168.0.1")));
    CHECK_FALSE(isPrivateIpv4(*parseIpv4("198.18.0.1"))); // TUN adapters of proxy tools
    CHECK_FALSE(isPrivateIpv4(*parseIpv4("100.64.0.1"))); // CGNAT / Tailscale
}

TEST_CASE("subnet scan covers at most one /24 and skips ourselves") {
    auto self = *parseIpv4("192.168.1.20");
    auto hosts = scanHosts(self, 24);
    CHECK(hosts.size() == 253);
    CHECK(hosts.front() == *parseIpv4("192.168.1.1"));
    CHECK(hosts.back() == *parseIpv4("192.168.1.254"));
    CHECK(std::find(hosts.begin(), hosts.end(), self) == hosts.end());
    // A /16 office network: only our own /24.
    auto wide = scanHosts(*parseIpv4("10.0.37.5"), 16);
    CHECK(wide.size() == 253);
    CHECK(net24(wide.front()) == *parseIpv4("10.0.37.0"));
    // A small network: just that.
    CHECK(scanHosts(*parseIpv4("192.168.8.2"), 29).size() == 5);
    // Public, too wide or point-to-point: nothing.
    CHECK(scanHosts(*parseIpv4("8.8.8.8"), 24).empty());
    CHECK(scanHosts(*parseIpv4("10.0.0.5"), 8).empty());
    CHECK(scanHosts(*parseIpv4("10.0.0.5"), 31).empty());
}
