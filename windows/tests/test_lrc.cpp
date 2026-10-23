#include "md/lyrics/Lrc.h"

#include <doctest/doctest.h>

using namespace md::lyrics;

TEST_CASE("basic LRC with tags, multiple stamps and offset") {
    auto l = parseLrc("\xEF\xBB\xBF[ti:Song]\r\n[ar:Someone]\r\n[offset:500]\r\n"
                      "[00:10.00][00:30.50]Chorus\r\n[00:20.123]Verse\r\n\r\n[01:02:50]Late\r\n");
    REQUIRE(l.synced);
    CHECK(l.tags["ti"] == "Song");
    REQUIRE(l.lines.size() == 4);
    CHECK(l.lines[0].t == 9500);
    CHECK(l.lines[0].text == "Chorus");
    CHECK(l.lines[1].t == 19623);
    CHECK(l.lines[1].text == "Verse");
    CHECK(l.lines[2].t == 30000);
    CHECK(l.lines[3].t == 62000); // mm:ss:cc form
    CHECK(l.lines[0].d == l.lines[1].t - l.lines[0].t);
    CHECK(lineAt(l, 0) == -1);
    CHECK(lineAt(l, 9500) == 0);
    CHECK(lineAt(l, 25000) == 1);
    CHECK(lineAt(l, 999999) == 3);
}

TEST_CASE("enhanced LRC word timing") {
    auto l = parseLrc("[00:01.00]<00:01.00>Hel<00:01.40>lo <00:02.00>world<00:03.00>\n[00:05.00]Next");
    REQUIRE(l.lines.size() == 2);
    auto& w = l.lines[0].words;
    REQUIRE(w.size() == 3);
    CHECK(l.lines[0].text == "Hello world");
    CHECK(w[0].t == 1000);
    CHECK(w[0].d == 400);
    CHECK(w[1].text == "lo ");
    CHECK(w[2].t == 2000);
    CHECK(w[2].d == 1000);
    CHECK(l.lines[1].words.empty());
}

TEST_CASE("unsynced text falls back to plain lyrics") {
    auto l = parseLrc("Line one\nLine two\n");
    CHECK_FALSE(l.synced);
    REQUIRE(l.lines.size() == 2);
    CHECK(lineAt(l, 1000) == -1);
    CHECK(parseLrc("").empty());
    CHECK(parseLrc("[ti:only tags]").empty());
}

TEST_CASE("garbage timestamps are ignored safely") {
    auto l = parseLrc("[99:99.99]bad\n[ab:cd]x\n[00:01.00]ok");
    REQUIRE(l.lines.size() == 1);
    CHECK(l.lines[0].text == "ok");
}

TEST_CASE("NetEase yrc word timing") {
    auto l = parseYrc("{\"t\":0,\"c\":[{\"tx\":\"\u4f5c\u8bcd: \"}]}\n"
                      "[16210,3460](16210,670,0)Hello (16880,410,0)world(17290,500,0)!\n"
                      "[20000,2000](20000,1000,0)Line (21000,1000,0)(two)\n");
    REQUIRE(l.synced);
    REQUIRE(l.lines.size() == 2);
    CHECK(l.lines[0].t == 16210);
    CHECK(l.lines[0].d == 3460);
    CHECK(l.lines[0].text == "Hello world!");
    REQUIRE(l.lines[0].words.size() == 3);
    CHECK(l.lines[0].words[1].t == 16880);
    CHECK(l.lines[0].words[1].d == 410);
    CHECK(l.lines[1].text == "Line (two)"); // parentheses in lyric text are kept
    CHECK(lineAt(l, 20500) == 1);
}

TEST_CASE("yrc converts to enhanced LRC that parses back to the same timing") {
    auto y = parseYrc("[16210,3460](16210,670,0)Hello (16880,410,0)world\n[20000,1500](20000,1500,0)Next\n");
    auto back = parseLrc(toLrc(y));
    REQUIRE(back.lines.size() == 2);
    CHECK(back.lines[0].t == 16210);
    REQUIRE(back.lines[0].words.size() == 2);
    CHECK(back.lines[0].words[1].t == 16880);
    CHECK(back.lines[0].words[1].d == 410);
    CHECK(back.lines[0].text == "Hello world");
}

TEST_CASE("translations merge by timestamp and round-trip as LRC") {
    auto base = parseLrc("[00:01.00]Hello\n[00:05.00]World\n[00:09.00]Untranslated\n");
    auto tr = parseLrc("[by:someone]\n[00:01.02]\xE4\xBD\xA0\xE5\xA5\xBD\n[00:05.30]\xE4\xB8\x96\xE7\x95\x8C\n[00:20.00]orphan\n");
    CHECK(mergeTranslation(base, tr) == 2);
    CHECK(base.lines[0].translation == "\xE4\xBD\xA0\xE5\xA5\xBD");
    CHECK(base.lines[1].translation == "\xE4\xB8\x96\xE7\x95\x8C");
    CHECK(base.lines[2].translation.empty());
    CHECK(toJson(base)["lines"][0]["tr"] == "\xE4\xBD\xA0\xE5\xA5\xBD");
    auto again = parseLrc("[00:01.00]Hello\n[00:05.00]World\n[00:09.00]Untranslated\n");
    CHECK(mergeTranslation(again, parseLrc(translationLrc(base))) == 2);
    CHECK(hasTranslation(again));
}

TEST_CASE("translations of word-timed lyrics are aligned through the plain LRC") {
    // NetEase: tlyric follows the plain LRC; the yrc lines start up to seconds earlier or later
    // ("Someone Like You": 40 of 68 translations were lost with the timestamp match alone).
    auto yrc = parseYrc("[10500,2000](10500,2000,0)I heard\n[14100,3000](14100,3000,0)that you're settled down\n"
                        "[20000,2000](20000,2000,0)I heard\n[30000,2000](30000,2000,0)Never mind\n");
    auto plain = parseLrc("[00:12.74]I heard\n[00:16.48]That you're settled down\n[00:21.20]I heard\n[00:30.10]Never mind\n");
    auto tr = parseLrc("[00:12.74]A\n[00:16.48]B\n[00:21.20]C\n[00:30.10]D\n");
    auto lost = yrc;
    CHECK(mergeTranslation(lost, tr) == 1); // the old match: only lines within 600 ms
    CHECK(mergeTranslation(yrc, tr, 600, &plain) == 4);
    CHECK(yrc.lines[0].translation == "A");
    CHECK(yrc.lines[1].translation == "B"); // same text up to case and spaces
    CHECK(yrc.lines[2].translation == "C"); // a repeated line: the nearest occurrence
    CHECK(yrc.lines[3].translation == "D");
    auto out = translationLrc(yrc);
    CHECK(out.rfind(kTranslationMark, 0) == 0);
    auto again = parseYrc("[10500,2000](10500,2000,0)I heard\n[14100,3000](14100,3000,0)that you're settled down\n"
                          "[20000,2000](20000,2000,0)I heard\n[30000,2000](30000,2000,0)Never mind\n");
    CHECK(mergeTranslation(again, parseLrc(out)) == 4); // re-read from the cache / the phone
    CHECK(translationLrc(parseLrc("[00:01.00]x\n")).empty());
}

TEST_CASE("entities and instrumental notices") {
    CHECK(decodeEntities("Don&apos;t &quot;stop&quot; &amp; &#26228;") == "Don't \"stop\" & \xE6\x99\xB4");
    CHECK(isInstrumentalNotice(parseLrc("[00:00.000]\xE7\xBA\xAF\xE9\x9F\xB3\xE4\xB9\x90\xEF\xBC\x8C\xE8\xAF\xB7\xE6\xAC\xA3\xE8\xB5\x8F")));
    CHECK_FALSE(isInstrumentalNotice(parseLrc("[00:01.00]real words")));
}

TEST_CASE("normalisation and matching") {
    CHECK(normalize("Shape of You (Live)") == "shapeofyou");
    CHECK(normalize("晴天【Live】") == normalize("晴天"));
    CHECK(normalize("Song feat. Other") == "song");
    Candidate want{"晴天", "周杰伦", "叶惠美", 269000};
    CHECK(matchScore(want, {"晴天", "周杰伦", "", 270000}) > 0.8);
    CHECK(matchScore(want, {"晴天 (Live)", "周杰伦", "", 268000}) > 0.8);
    CHECK(matchScore(want, {"晴天", "周杰伦", "", 300000}) == 0); // duration mismatch
    CHECK(matchScore(want, {"稻香", "周杰伦", "", 269000}) == 0);
    CHECK(matchScore(want, {"晴天", "Someone Else", "", 269000}) < matchScore(want, {"晴天", "周杰伦", "", 269000}));
}

TEST_CASE("sameSong tolerates trackId churn of one song") {
    Candidate a{"晴天", "周杰伦", "叶惠美", 269000};
    // Duration jitter / late duration / album filled in later: same song.
    CHECK(sameSong(a, {"晴天", "周杰伦", "叶惠美", 269600}));
    CHECK(sameSong(a, {"晴天", "周杰伦", "", 271900}));
    CHECK(sameSong(a, {"晴天", "周杰伦", "叶惠美", std::nullopt}));
    CHECK(sameSong({"晴天", "周杰伦", "", std::nullopt}, a));
    CHECK(sameSong(a, {"晴天 (Live)", "周杰伦", "", 269000})); // normalize() strips the suffix
    CHECK(sameSong({"Hello", "Adele", "", 295000}, {"hello", "ADELE", "25", 295400}));
    CHECK(sameSong({"!!!", "", "", 1000}, {"!!!", "", "", 1000})); // literal match when normalize is empty
    // Different songs.
    CHECK_FALSE(sameSong(a, {"晴天", "周杰伦", "叶惠美", 275000}));   // > 3 s apart
    CHECK_FALSE(sameSong(a, {"七里香", "周杰伦", "七里香", 269000}));
    CHECK_FALSE(sameSong(a, {"晴天", "其他歌手", "", 269000}));
    CHECK_FALSE(sameSong(a, {"", "周杰伦", "叶惠美", 269000}));        // a blank report is not the song
    CHECK_FALSE(sameSong({"", "", "", std::nullopt}, {"", "", "", std::nullopt}));
    CHECK_FALSE(sameSong({"!!!", "", "", 1000}, {"???", "", "", 1000}));
    CHECK(sameSong(a, {"晴天", "周杰伦", "叶惠美", 274000}, 6000)); // custom tolerance
}

TEST_CASE("two lines with one timestamp are original and translation") {
    auto l = parseLrc("[00:01.00]Hello\n[00:01.00]\xE4\xBD\xA0\xE5\xA5\xBD\n[00:03.00]World\n");
    REQUIRE(l.lines.size() == 2);
    CHECK(l.lines[0].text == "Hello");
    CHECK(l.lines[0].translation == "\xE4\xBD\xA0\xE5\xA5\xBD");
    CHECK(l.lines[0].d == 2000);
}

TEST_CASE("plain lyrics drop blank lines and tag prefixes") {
    auto l = fromPlainText("[ti:Song]\n\nFirst line\n  \n[ar:x]Second\n");
    REQUIRE(l.lines.size() == 2);
    CHECK(l.lines[0].text == "First line");
    CHECK(l.lines[1].text == "Second");
    CHECK_FALSE(l.synced);
}
