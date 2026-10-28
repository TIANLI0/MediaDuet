#include "md/unlock/Unlock.h"

#include "md/crypto/Crypto.h"

#include <doctest/doctest.h>

using namespace md;
using namespace md::unlock;

namespace {
const std::string kEnrollment = "0123456789abcdef0123456789abcdef";
const std::string kSid = "S-1-5-21-1-2-3-1001";
} // namespace

TEST_CASE("unlock: a sealed password opens only with the same key, enrollment and user") {
    Bytes key = crypto::randomBytes(kKeyBytes);
    Bytes sealed = seal(key, kEnrollment, kSid, "pa$$wörd");
    CHECK(sealed.size() == kNonceBytes + 9 + 16);

    auto pw = open(key, kEnrollment, kSid, sealed);
    REQUIRE(pw);
    CHECK(*pw == "pa$$wörd");

    CHECK_FALSE(open(crypto::randomBytes(kKeyBytes), kEnrollment, kSid, sealed));
    CHECK_FALSE(open(key, "fedcba9876543210fedcba9876543210", kSid, sealed));
    CHECK_FALSE(open(key, kEnrollment, "S-1-5-21-1-2-3-1002", sealed));
    Bytes tampered = sealed;
    tampered.back() ^= 1;
    CHECK_FALSE(open(key, kEnrollment, kSid, tampered));
    CHECK_FALSE(open(key, kEnrollment, kSid, ByteView(sealed).first(20)));
    CHECK_FALSE(open(ByteView(key).first(16), kEnrollment, kSid, sealed));
}

TEST_CASE("unlock: two seals of the same password differ (fresh nonce)") {
    Bytes key = crypto::randomBytes(kKeyBytes);
    CHECK(seal(key, kEnrollment, kSid, "x") != seal(key, kEnrollment, kSid, "x"));
}

TEST_CASE("unlock: sealing requires an AES-256 key and a valid enrollment") {
    CHECK_THROWS_AS(seal(Bytes(16), kEnrollment, kSid, "test"), std::invalid_argument);
    CHECK_THROWS_AS(seal(Bytes(32), "bad-id", kSid, "test"), std::invalid_argument);
}

TEST_CASE("unlock: message bodies") {
    Bytes key(kKeyBytes, 7);
    json e = enrollBody(kEnrollment, key, "DESKTOP-1");
    CHECK(e["enrollment"] == kEnrollment);
    CHECK(base64Decode(e["key"].get<std::string>()) == key);
    CHECK(e["pc"] == "DESKTOP-1");
    CHECK(requestBody(kEnrollment, "PC")["enrollment"] == kEnrollment);
    CHECK(parseRevoke(revokeBody(kEnrollment)) == kEnrollment);
    CHECK_FALSE(parseRevoke(json{{"enrollment", "XYZ"}}));
    CHECK_FALSE(parseRevoke(json::array()));
    CHECK(isEnrollmentId(kEnrollment));
    CHECK_FALSE(isEnrollmentId("0123456789ABCDEF0123456789ABCDEF"));
}

TEST_CASE("unlock: unlock.result parsing") {
    Bytes key(kKeyBytes, 9);
    auto ok = parseResult(json{{"ok", true}, {"key", base64Encode(key)}});
    REQUIRE(ok);
    CHECK(ok->ok);
    CHECK(ok->key == key);

    auto enrolled = parseResult(json{{"ok", true}});
    REQUIRE(enrolled);
    CHECK(enrolled->key.empty());

    auto denied = parseResult(json{{"ok", false}, {"error", {{"code", "denied"}}}});
    REQUIRE(denied);
    CHECK_FALSE(denied->ok);
    CHECK(denied->error == "denied");
    CHECK(parseResult(json{{"ok", false}})->error == "internal");

    CHECK_FALSE(parseResult(json{{"ok", true}, {"key", base64Encode(Bytes(16, 1))}}));
    CHECK_FALSE(parseResult(json{{"key", base64Encode(key)}}));
    CHECK_FALSE(parseResult(json{{"ok", "yes"}}));
}

TEST_CASE("unlock: phone errors map to the UI's enable errors") {
    CHECK(enableError("denied") == "phone_denied");
    CHECK(enableError("timeout") == "phone_timeout");
    CHECK(enableError("no_auth") == "phone_no_auth");
    CHECK(enableError("busy") == "busy");
    CHECK(enableError("key_invalidated") == "phone_failed");
}
