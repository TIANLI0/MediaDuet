#include "md/protocol/Frame.h"

#include <doctest/doctest.h>

using namespace md;
using namespace md::proto;

TEST_CASE("frame decoder handles byte-by-byte input and multiple frames") {
    Bytes a = encodeFrame(FrameType::Handshake, toBytes("{\"a\":1}"));
    Bytes b = encodeFrame(FrameType::Secure, Bytes(300, 0xAB));
    Bytes all = a;
    append(all, b);
    FrameDecoder d;
    std::vector<Frame> got;
    for (uint8_t byte : all) {
        d.feed(ByteView(&byte, 1));
        while (auto f = d.next()) got.push_back(std::move(*f));
    }
    REQUIRE(got.size() == 2);
    CHECK(got[0].type == FrameType::Handshake);
    CHECK(toString(got[0].body) == "{\"a\":1}");
    CHECK(got[1].type == FrameType::Secure);
    CHECK(got[1].body.size() == 300);
    CHECK(d.buffered() == 0);
    CHECK_FALSE(d.failed());
}

TEST_CASE("frame decoder rejects bad lengths and types") {
    SUBCASE("zero length") {
        FrameDecoder d;
        Bytes z = {0, 0, 0, 0};
        d.feed(z);
        CHECK_FALSE(d.next());
        CHECK(d.failed());
    }
    SUBCASE("too large") {
        FrameDecoder d;
        Bytes big(4);
        putU32BE(big.data(), kMaxFrameLength + 1);
        d.feed(big);
        CHECK_FALSE(d.next());
        CHECK(d.failed());
    }
    SUBCASE("unknown type") {
        FrameDecoder d;
        Bytes f = {0, 0, 0, 1, 0x09};
        d.feed(f);
        CHECK_FALSE(d.next());
        CHECK(d.failed());
    }
    SUBCASE("oversized handshake") {
        FrameDecoder d;
        Bytes f = encodeFrame(FrameType::Secure, Bytes(kMaxHandshakeBody + 10, 1));
        f[4] = 0x01;
        d.feed(f);
        CHECK_FALSE(d.next());
        CHECK(d.failed());
    }
    SUBCASE("encode refuses oversize") { CHECK_THROWS(encodeFrame(FrameType::Secure, Bytes(kMaxFrameLength, 0))); }
}

TEST_CASE("blob chunk validation") {
    BlobChunk c{Bytes(32, 1), 100, 40, Bytes(60, 2)};
    auto enc = encodeBlobChunk(c);
    auto dec = decodeBlobChunk(enc);
    REQUIRE(dec);
    CHECK(dec->total == 100);
    CHECK(dec->offset == 40);
    CHECK(dec->data.size() == 60);
    // data overruns total
    BlobChunk bad{Bytes(32, 1), 50, 40, Bytes(60, 2)};
    CHECK_FALSE(decodeBlobChunk(encodeBlobChunk(bad)));
    // total too large
    BlobChunk huge{Bytes(32, 1), static_cast<uint32_t>(kMaxArtworkBytes + 1), 0, Bytes(10, 2)};
    CHECK_FALSE(decodeBlobChunk(encodeBlobChunk(huge)));
    CHECK_FALSE(decodeBlobChunk(Bytes(10)));
}
