#include "md/protocol/Envelope.h"
#include "md/protocol/Messages.h"

#include <doctest/doctest.h>

using namespace md::proto;

static const std::string kSrc = "0123456789abcdef0123456789abcdef";

TEST_CASE("envelope round trip") {
    Envelope e;
    e.type = "media.command";
    e.id = 42;
    e.ts = 1760000000000;
    e.src = kSrc;
    e.sid = "fedcba9876543210fedcba9876543210";
    e.re = 41;
    e.body = {{"action", "pause"}, {"title", "晴天"}};
    auto p = Envelope::parse(e.serialize());
    REQUIRE(p);
    CHECK(p->type == e.type);
    CHECK(p->id == 42);
    CHECK(p->re == 41u);
    CHECK(p->sid == e.sid);
    CHECK(p->body["title"] == "晴天");
}

TEST_CASE("envelope validation rejects malformed input") {
    auto bad = [](const std::string& s) { return !Envelope::parse(s).has_value(); };
    std::string ok = R"({"v":1,"type":"ping","id":1,"ts":0,"src":")" + kSrc + R"(","sid":"","body":{}})";
    CHECK_FALSE(bad(ok));
    CHECK(bad("not json"));
    CHECK(bad("[]"));
    CHECK(bad(R"({"v":1,"type":"Ping","id":1,"ts":0,"src":")" + kSrc + R"(","sid":"","body":{}})"));
    CHECK(bad(R"({"v":1,"type":"ping","id":0,"ts":0,"src":")" + kSrc + R"(","sid":"","body":{}})"));
    CHECK(bad(R"({"v":1,"type":"ping","id":-5,"ts":0,"src":")" + kSrc + R"(","sid":"","body":{}})"));
    CHECK(bad(R"({"v":1,"type":"ping","id":9007199254740992,"ts":0,"src":")" + kSrc + R"(","sid":"","body":{}})"));
    CHECK(bad(R"({"v":1,"type":"ping","id":1,"ts":0,"src":"XYZ","sid":"","body":{}})"));
    CHECK(bad(R"({"v":1,"type":"ping","id":1,"ts":0,"src":")" + kSrc + R"(","sid":"short","body":{}})"));
    CHECK(bad(R"({"v":1,"type":"ping","id":1,"ts":0,"src":")" + kSrc + R"(","sid":"","body":[]})"));
    CHECK(bad(R"({"type":"ping","id":1,"ts":0,"src":")" + kSrc + R"(","sid":"","body":{}})"));
    CHECK(bad(std::string("{\"v\":1,\"type\":\"ping\",\"id\":1,\"ts\":0,\"src\":\"") + kSrc +
              "\",\"sid\":\"\",\"body\":{\"x\":\"\xff\"}}"));
    std::string longType(49, 'a');
    CHECK(bad(R"({"v":1,"type":")" + longType + R"(","id":1,"ts":0,"src":")" + kSrc + R"(","sid":"","body":{}})"));
}

TEST_CASE("device info validation") {
    json d = {{"id", kSrc}, {"name", "PC"}, {"platform", "windows"}};
    CHECK(deviceInfoFromJson(d).has_value());
    d["name"] = std::string(65, 'x');
    CHECK_FALSE(deviceInfoFromJson(d).has_value());
    d["name"] = "PC";
    d["id"] = "nothex";
    CHECK_FALSE(deviceInfoFromJson(d).has_value());
}

TEST_CASE("base64 fields with exact length") {
    json b = {{"k", "AAAA"}};
    CHECK(getB64(b, "k", 3).has_value());
    CHECK_FALSE(getB64(b, "k", 4).has_value());
    CHECK_FALSE(getB64(b, "missing", 3).has_value());
}
