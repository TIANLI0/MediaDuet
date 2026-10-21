#include "md/media/NeteasePlayer.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <limits>

using namespace md::media::netease;

TEST_CASE("byte patterns match with wildcards and report the first hit") {
    BytePattern p("66 0F 2E 0D ? ? ? ? 7A ? 75 ? 66 0F 2E 15");
    CHECK(p.size() == 16);
    // Code as in cloudmusic.dll 3.1.41: ucomisd xmm1, [rip+0x020E8F99]; jp; jne; ucomisd xmm2, ...
    const uint8_t code[] = {0xCC, 0x66, 0x0F, 0x2E, 0x0D, 0x99, 0x8F, 0x0E, 0x02, 0x7A, 0x05, 0x75, 0x03,
                            0x66, 0x0F, 0x2E, 0x15, 0x00, 0x66, 0x0F, 0x2E, 0x0D};
    auto at = p.find(code, sizeof code);
    REQUIRE(at.has_value());
    CHECK(*at == 1);
    int32_t disp = 0;
    std::memcpy(&disp, code + *at + kScheduleSignature.dispOffset, sizeof disp);
    // The operand is relative to the end of the instruction (disp32 + 4).
    CHECK(ripTarget(0x7FFC00001000 + *at + kScheduleSignature.dispOffset, disp) == 0x7FFC00001000 + 1 + 8 + 0x020E8F99);
    CHECK(ripTarget(0x1000, -0x10) == 0x1000 + 4 - 0x10);

    CHECK_FALSE(p.find(code, 10).has_value());              // too short
    CHECK_FALSE(BytePattern("66 0F").find(code + 2, 3).has_value());
    CHECK(BytePattern("0F 2E").find(code, sizeof code) == 2u);
    CHECK(BytePattern("zz").size() == 0);                     // malformed: matches nothing
    CHECK_FALSE(BytePattern("zz").find(code, sizeof code).has_value());
}

TEST_CASE("the NetEase signatures parse") {
    CHECK(BytePattern(kPlayerSignature.pattern).size() == 82);
    CHECK(BytePattern(kScheduleSignature.pattern).size() == 16);
}

TEST_CASE("song ids come from the play info string") {
    CHECK(parseSongId("3340112782_YKR00X") == 3340112782);
    CHECK(parseSongId("1330348068") == 1330348068);
    CHECK(parseSongId("") == 0);
    CHECK(parseSongId("_abc") == 0);
    CHECK(parseSongId("12a_b") == 0);
    CHECK(parseSongId("99999999999999999999999_x") == 0); // overflow
}

TEST_CASE("implausible readings are rejected") {
    PlayerReading ok{1, 61.26, 253.441, 3340112782};
    CHECK(plausible(ok));
    auto bad = ok;
    bad.status = 7;
    CHECK_FALSE(plausible(bad));
    bad = ok;
    bad.positionSec = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(plausible(bad));
    bad = ok;
    bad.durationSec = 1e300;
    CHECK_FALSE(plausible(bad));
    bad = ok;
    bad.positionSec = -1;
    CHECK_FALSE(plausible(bad));
    CHECK(plausible(PlayerReading{0, 0, 0, 0})); // idle, nothing loaded
}

TEST_CASE("NetEase's play queue file is read in display order") {
    const char* file = R"({"list":[
        {"id":"2","displayOrder":1,"text":"我喜欢的音乐","track":{"id":"2","name":"晴天","artists":[{"name":"周杰伦"}]}},
        {"id":"1","displayOrder":0,"text":"我喜欢的音乐","track":{"id":"1","name":"起风了","artists":[{"name":"买辣椒也用券"},{"name":"Ⅲ"}]}},
        {"id":"x","displayOrder":2,"text":"本地","track":{"id":"0","name":"local.mp3","artists":[]}},
        {"id":"3","displayOrder":3},
        "garbage"
    ]})";
    auto list = parsePlayingList(file);
    REQUIRE(list.has_value());
    REQUIRE(list->entries.size() == 3);
    CHECK(list->entries[0].id == 1);
    CHECK(list->entries[0].title == "起风了");
    CHECK(list->entries[0].subtitle == "买辣椒也用券 / Ⅲ");
    CHECK(list->entries[1].id == 2);
    CHECK(list->entries[2].id == 0); // a local file: not playable by id
    CHECK(list->rev.size() == 8);

    auto same = parsePlayingList(file);
    CHECK(same->rev == list->rev);
    auto other = parsePlayingList(R"({"list":[{"id":"1","track":{"id":"1","name":"起风了"}}]})");
    CHECK(other->rev != list->rev);

    CHECK_FALSE(parsePlayingList("not json").has_value());
    CHECK_FALSE(parsePlayingList(R"({"nolist":1})").has_value());

    auto q = queueWindow(*list, "cloudmusic.exe#compat", 2);
    CHECK(q.key == "cloudmusic.exe#compat");
    CHECK(q.rev == list->rev);
    CHECK(q.total == 3);
    CHECK(q.offset == 0);
    CHECK(q.items.size() == 3);
    CHECK(q.activeId == 2);
    CHECK(q.title == "我喜欢的音乐");
    CHECK_FALSE(queueWindow(*list, "k", 99).activeId.has_value()); // not in the queue
}

TEST_CASE("long NetEase queues are sent as a window around the current song") {
    PlayingList list;
    for (int i = 1; i <= 524; ++i) list.entries.push_back({i, "t" + std::to_string(i), "", "src"});
    auto q = queueWindow(list, "k", 400, 300);
    CHECK(q.total == 524);
    CHECK(q.items.size() == 300);
    CHECK(q.offset == 224); // clamped to the end: 524 - 300
    q = queueWindow(list, "k", 100, 300);
    CHECK(q.offset == 69);  // id 100 is entry 99; 30 entries before it
    CHECK(q.items[30].id == 100);
    q = queueWindow(list, "k", 5, 300);
    CHECK(q.offset == 0);
    q = queueWindow(list, "k", std::nullopt, 300);
    CHECK(q.offset == 0);
    CHECK(q.items.size() == 300);
}
