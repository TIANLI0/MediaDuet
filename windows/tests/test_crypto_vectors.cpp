#include "TestUtil.h"

#include "md/crypto/Crypto.h"
#include "md/link/Handshake.h"
#include "md/link/SecureChannel.h"
#include "md/media/MediaTypes.h"
#include "md/protocol/Frame.h"

#include <doctest/doctest.h>

using namespace md;
using md::test::hex;

TEST_CASE("base64 and hex round trips") {
    CHECK(base64Encode(toBytes("")) == "");
    CHECK(base64Encode(toBytes("f")) == "Zg==");
    CHECK(base64Encode(toBytes("fo")) == "Zm8=");
    CHECK(base64Encode(toBytes("foo")) == "Zm9v");
    CHECK(base64Encode(toBytes("foobar")) == "Zm9vYmFy");
    CHECK(*base64Decode("Zm9vYmE=") == toBytes("fooba"));
    CHECK_FALSE(base64Decode("Zm9=YmE="));
    CHECK_FALSE(base64Decode("Zm9"));
    CHECK_FALSE(base64Decode("Zm9v!A=="));
    CHECK(hexEncode(*hexDecode("00ff10Ab")) == "00ff10ab");
    CHECK_FALSE(hexDecode("abc"));
}

TEST_CASE("HKDF matches RFC 5869 test case 1") {
    Bytes ikm(22, 0x0b);
    Bytes salt = hex("000102030405060708090a0b0c");
    Bytes info = hex("f0f1f2f3f4f5f6f7f8f9");
    Bytes prk = crypto::hkdfExtract(salt, ikm);
    CHECK(hexEncode(prk) == "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
    Bytes okm = crypto::hkdfExpand(prk, info, 42);
    CHECK(hexEncode(okm) ==
          "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
}

TEST_CASE("protocol v1 key schedule matches the reference vectors") {
    auto v = test::loadVectors();
    auto& eph = v["ephemeral"];
    auto kc = crypto::EcdhKey::fromPrivate(hex(eph["client_private"]), hex(eph["client_public"]));
    auto ksrv = crypto::EcdhKey::fromPrivate(hex(eph["server_private"]), hex(eph["server_public"]));
    Bytes z = kc.agree(hex(eph["server_public"]));
    CHECK(hexEncode(z) == eph["shared_secret"].get<std::string>());
    CHECK(ksrv.agree(hex(eph["client_public"])) == z);

    std::string idC = v["ids"]["client"], idS = v["ids"]["server"];
    Bytes nC = hex(eph["client_nonce"]), nS = hex(eph["server_nonce"]);
    Bytes epkC = hex(eph["client_public"]), epkS = hex(eph["server_public"]);

    SUBCASE("pairing") {
        auto& p = v["pairing"];
        Bytes th = link::ks::pairTranscript(nC, nS, epkC, epkS, idC, idS);
        CHECK(hexEncode(th) == p["transcript_hash"].get<std::string>());
        auto sec = link::ks::pairing(z, th);
        CHECK(hexEncode(sec.prk) == p["prk"].get<std::string>());
        CHECK(sec.sas == p["sas"].get<std::string>());
        CHECK(hexEncode(sec.pairingKey) == p["pairing_key"].get<std::string>());
        CHECK(hexEncode(link::ks::confirmMac(sec.pairingKey, 'C', th)) == p["confirm_mac_client"].get<std::string>());
        CHECK(hexEncode(link::ks::confirmMac(sec.pairingKey, 'S', th)) == p["confirm_mac_server"].get<std::string>());
        Bytes qr = hex(p["qr_token"]);
        CHECK(base64UrlEncode(qr) == p["qr_token_b64url"].get<std::string>());
        CHECK(hexEncode(link::ks::qrConfirmMac(qr, 'C', th)) == p["qr_confirm_mac_client"].get<std::string>());
        CHECK(hexEncode(link::ks::qrConfirmMac(qr, 'S', th)) == p["qr_confirm_mac_server"].get<std::string>());
    }
    SUBCASE("session") {
        auto& s = v["session"];
        Bytes key = hex(s["pairing_key"]);
        Bytes th = link::ks::sessionTranscript(nC, nS, epkC, epkS, idC, idS);
        CHECK(hexEncode(th) == s["transcript_hash"].get<std::string>());
        CHECK(hexEncode(link::ks::sessionMac(key, "server", th)) == s["mac_server"].get<std::string>());
        CHECK(hexEncode(link::ks::sessionMac(key, "client", th)) == s["mac_client"].get<std::string>());
        auto keys = link::ks::sessionKeys(key, z, th);
        CHECK(hexEncode(keys.keyC2S) == s["key_c2s"].get<std::string>());
        CHECK(hexEncode(keys.keyS2C) == s["key_s2c"].get<std::string>());
        CHECK(hexEncode(keys.ivC2S) == s["iv_c2s"].get<std::string>());
        CHECK(hexEncode(keys.ivS2C) == s["iv_s2c"].get<std::string>());

        // Records
        link::RecordCipher c2s(keys.keyC2S, keys.ivC2S), s2c(keys.keyS2C, keys.ivS2C);
        link::RecordCipher c2sRx(keys.keyC2S, keys.ivC2S);
        for (auto& r : v["records"]) {
            Bytes pt = hex(r["plaintext"]);
            auto& cipher = r["dir"] == "c2s" ? c2s : s2c;
            CHECK(cipher.sequence() == r["seq"].get<uint64_t>());
            Bytes ct = cipher.seal(pt);
            CHECK(hexEncode(ct) == r["ciphertext"].get<std::string>());
            CHECK(hexEncode(proto::encodeFrame(proto::FrameType::Secure, ct)) == r["frame"].get<std::string>());
            if (r["dir"] == "c2s") {
                auto back = c2sRx.open(ct);
                REQUIRE(back);
                CHECK(*back == pt);
            }
        }
        // Replaying the first record against a fresh receiver after it advanced must fail.
        link::RecordCipher rx(keys.keyC2S, keys.ivC2S);
        Bytes rec0 = hex(v["records"][0]["ciphertext"]);
        CHECK(rx.open(rec0));
        CHECK_FALSE(rx.open(rec0));

        // Audio key derivation
        auto& a = v["audio"];
        Bytes sid = hex(a["stream_id"]);
        Bytes ak = crypto::hmacSha256(keys.keyC2S, concat({view(std::string_view("audio key")), sid}));
        Bytes aiv = crypto::hmacSha256(keys.keyC2S, concat({view(std::string_view("audio iv")), sid}));
        aiv.resize(4);
        CHECK(hexEncode(ak) == a["audio_key"].get<std::string>());
        CHECK(hexEncode(aiv) == a["audio_iv"].get<std::string>());
        link::RecordCipher audio(ak, aiv, proto::FrameType::Audio);
        CHECK(hexEncode(audio.seal(hex(a["plaintext"]))) == a["ciphertext"].get<std::string>());
    }
}

TEST_CASE("blob chunk and handshake frame vectors") {
    auto v = test::loadVectors();
    Bytes blob;
    for (int r = 0; r < 4; ++r)
        for (int i = 0; i < 256; ++i) blob.push_back(static_cast<uint8_t>(i));
    CHECK(hexEncode(crypto::sha256(blob)) == v["blob"]["content_sha256"].get<std::string>());
    proto::BlobChunk c{crypto::sha256(blob), static_cast<uint32_t>(blob.size()), 0, Bytes(blob.begin(), blob.begin() + 512)};
    Bytes rec = {0x02};
    append(rec, proto::encodeBlobChunk(c));
    CHECK(hexEncode(rec) == v["blob"]["first_chunk_plaintext"].get<std::string>());

    std::string js = v["handshake_frame"]["json"];
    CHECK(hexEncode(proto::encodeFrame(proto::FrameType::Handshake, view(js))) ==
          v["handshake_frame"]["frame"].get<std::string>());
}

TEST_CASE("trackId vectors") {
    auto v = test::loadVectors();
    for (auto& t : v["track_ids"]) {
        std::optional<int64_t> d;
        if (!t["durationMs"].is_null()) d = t["durationMs"].get<int64_t>();
        CHECK(media::computeTrackId(t["title"], t["artist"], t["album"], d) == t["trackId"].get<std::string>());
    }
}

TEST_CASE("ECDH rejects invalid peer points") {
    auto k = crypto::EcdhKey::generate();
    CHECK(k.publicKey().size() == 65);
    Bytes bad(65, 0x01);
    bad[0] = 0x04;
    CHECK_THROWS(k.agree(bad));
    CHECK_THROWS(k.agree(Bytes(33, 0x02)));
    auto k2 = crypto::EcdhKey::generate();
    CHECK(k.agree(k2.publicKey()) == k2.agree(k.publicKey()));
}

TEST_CASE("AES-GCM detects tampering") {
    Bytes key = crypto::randomBytes(32);
    crypto::AesGcm g(key);
    Bytes nonce(12, 7);
    Bytes aad = toBytes("aad");
    Bytes ct = g.seal(nonce, aad, toBytes("hello"));
    CHECK(g.open(nonce, aad, ct) == toBytes("hello"));
    ct[0] ^= 1;
    CHECK_FALSE(g.open(nonce, aad, ct));
    ct[0] ^= 1;
    CHECK_FALSE(g.open(nonce, toBytes("AAD"), ct));
}
