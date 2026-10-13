#include "md/lyrics/Karaoke.h"

#include <doctest/doctest.h>

using namespace md::lyrics;

static Line wordLine(std::vector<std::pair<int64_t, std::string>> ws, int64_t wordDur = 500) {
    Line l;
    for (auto& [t, s] : ws) l.words.push_back({t, wordDur, s});
    l.t = l.words.front().t;
    l.d = 4000;
    for (auto& w : l.words) l.text += w.text;
    return l;
}

TEST_CASE("karaoke: word spans follow the concatenated, trimmed words") {
    auto k = buildKaraoke(wordLine({{1000, "  Hello "}, {1500, "big  "}, {2000, "world  "}}));
    CHECK(k.text == L"Hello big  world");
    REQUIRE(k.words.size() == 3);
    CHECK(k.words[0].start == 0);
    CHECK(k.words[0].len == 6); // "Hello " (leading blanks trimmed away)
    CHECK(k.words[1].start == 6);
    CHECK(k.words[1].len == 5); // "big  " keeps both inner spaces
    CHECK(k.words[2].start == 11);
    CHECK(k.words[2].len == 5); // trailing blanks trimmed
    CHECK(k.codePoints == 16);
}

TEST_CASE("karaoke: whitespace-only words are clamped to empty spans") {
    auto k = buildKaraoke(wordLine({{0, " "}, {300, "La"}, {600, " "}, {900, "la"}, {1200, "  "}}));
    CHECK(k.text == L"La la");
    REQUIRE(k.words.size() == 5);
    CHECK(k.words[0].len == 0);
    CHECK(k.words[1].start == 0);
    CHECK(k.words[1].len == 2);
    CHECK(k.words[2].start == 2);
    CHECK(k.words[3].start == 3);
    CHECK(k.words[4].start == 5);
    CHECK(k.words[4].len == 0);
    // Before the first visible word starts, the fill is at 0.
    CHECK(fillAt(k, 100) == FillSpan{0, 0, 0});
    // Moving inside "La".
    auto f = fillAt(k, 550);
    CHECK(f.from == 0);
    CHECK(f.to == 2);
    CHECK(f.frac == doctest::Approx(0.5));
}

TEST_CASE("karaoke: surrogate pairs and CJK stay on code-point boundaries") {
    // "🎵" is a surrogate pair in UTF-16.
    auto k = buildKaraoke(wordLine({{0, "\xF0\x9F\x8E\xB5"}, {500, "\xE6\x99\xB4"}, {1000, "\xE5\xA4\xA9"}}));
    CHECK(k.text.size() == 4);
    CHECK(k.codePoints == 3);
    CHECK(k.words[0].start == 0);
    CHECK(k.words[0].len == 2);
    CHECK(k.words[1].start == 2);
    CHECK(k.words[2].start == 3);
    auto f = fillAt(k, 250);
    CHECK(f.from == 0);
    CHECK(f.to == 2);
}

TEST_CASE("karaoke: gaps hold at the next word, the end completes") {
    auto k = buildKaraoke(wordLine({{1000, "one "}, {2000, "two"}}, 400));
    CHECK(fillAt(k, 500) == FillSpan{0, 0, 0});
    CHECK(fillAt(k, 1200).frac == doctest::Approx(0.5));
    CHECK(fillAt(k, 1700) == FillSpan{4, 4, 0}); // gap: hold at "two"
    CHECK(fillAt(k, 5000) == FillSpan{7, 7, 0});
    double next = 0;
    CHECK_FALSE(fillMoving(k, 1700, &next));
    CHECK(next == 2000);
    CHECK(fillMoving(k, 2100));
    CHECK_FALSE(fillMoving(k, 2500, &next));
    CHECK(next == -1);
}

TEST_CASE("karaoke: line text that lost characters to trimming is rebuilt from words") {
    Line l = wordLine({{0, " (Ooh) "}, {500, "yeah"}});
    l.text = "(Ooh) yeah"; // what parseLrc stores
    auto k = buildKaraoke(l);
    CHECK(k.text == L"(Ooh) yeah");
    CHECK(k.words[1].start == 6);
}

TEST_CASE("karaoke: line-timed sweep pace") {
    Line l;
    l.t = 10000;
    l.d = 8000;
    l.text = "  abcd  ";
    auto k = buildKaraoke(l);
    CHECK(k.text == L"abcd");
    CHECK(k.words.empty());
    // min(0.9 x 8000, max(1200, 4 x 450)) = 1800
    CHECK(lineSweepMs(k) == doctest::Approx(1800));
    CHECK(fillAt(k, 10900).frac == doctest::Approx(0.5));
    CHECK(fillAt(k, 10900).to == 4);
    CHECK(fillAt(k, 20000).frac == doctest::Approx(1.0));
    l.d = 0;
    CHECK(lineSweepMs(buildKaraoke(l)) == doctest::Approx(1800));
    l.text = "a";
    l.d = 1000;
    CHECK(lineSweepMs(buildKaraoke(l)) == doctest::Approx(900));
}

TEST_CASE("karaoke: horizontal scroll keeps the edge visible") {
    CHECK(scrollOffset(500, 600, 400) == 0.f);              // fits
    CHECK(scrollOffset(1500, 600, 100) == 0.f);             // start aligned
    CHECK(scrollOffset(1500, 600, 760) == doctest::Approx(400)); // edge held at 60 %
    CHECK(scrollOffset(1500, 600, 1500) == doctest::Approx(900)); // end aligned
    CHECK(fitScale(500, 600) == 1.f);
    CHECK(fitScale(660, 600) == doctest::Approx(600.f / 660.f));
    CHECK(fitScale(2000, 600) == doctest::Approx(0.85f));
}

TEST_CASE("desktop rows: alternate layout only replaces one row per line") {
    Lyrics l;
    l.synced = true;
    for (int i = 0; i < 5; ++i) l.lines.push_back({i * 1000, 1000, "line" + std::to_string(i), {}, ""});
    auto r = desktopRows(l, -1, RowLayout::Alternate, true, true);
    CHECK(r.row[0] == RowContent{RowKind::Line, 0, false});
    CHECK(r.row[1] == RowContent{RowKind::Line, 1, false});
    CHECK(r.alignRight[1]);
    auto a = desktopRows(l, 0, RowLayout::Alternate, true, true);
    CHECK(a.row[0] == RowContent{RowKind::Line, 0, true});
    CHECK(a.row[1] == RowContent{RowKind::Line, 1, false});
    auto b = desktopRows(l, 1, RowLayout::Alternate, true, true);
    CHECK(b.row[1] == RowContent{RowKind::Line, 1, true}); // same line, now active
    CHECK(b.row[0] == RowContent{RowKind::Line, 2, false}); // only row 0 changed
    auto last = desktopRows(l, 4, RowLayout::Alternate, true, true);
    CHECK(last.row[0] == RowContent{RowKind::Line, 4, true});
    CHECK(last.row[1].kind == RowKind::None);
    auto c = desktopRows(l, 2, RowLayout::Center, true, true);
    CHECK(c.centered);
    CHECK(c.row[0] == RowContent{RowKind::Line, 2, true});
    CHECK(c.row[1] == RowContent{RowKind::Line, 3, false});
    auto one = desktopRows(l, 2, RowLayout::Alternate, false, true);
    CHECK(one.row[1].kind == RowKind::None);
}

TEST_CASE("desktop rows: translations centre the whole song") {
    Lyrics l;
    l.synced = true;
    l.lines.push_back({0, 1000, "a", {}, "A"});
    l.lines.push_back({1000, 1000, "b", {}, ""});
    auto r = desktopRows(l, 0, RowLayout::Alternate, true, true);
    CHECK(r.centered);
    CHECK(r.row[1] == RowContent{RowKind::Translation, 0, false});
    auto r2 = desktopRows(l, 1, RowLayout::Alternate, true, true);
    CHECK(r2.centered);
    CHECK(r2.row[1].kind == RowKind::None);
    auto off = desktopRows(l, 0, RowLayout::Alternate, true, false);
    CHECK_FALSE(off.centered);
}
