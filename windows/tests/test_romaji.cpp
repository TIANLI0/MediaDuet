#include "md/lyrics/Karaoke.h"
#include "md/lyrics/Romaji.h"

#include <doctest/doctest.h>

using namespace md::lyrics;

TEST_CASE("romaji: hiragana and katakana") {
    CHECK(kanaToRomaji("さくら") == "sakura");
    CHECK(kanaToRomaji("スキ") == "suki");
    CHECK(kanaToRomaji("ありがとう") == "arigatou");
    CHECK(kanaToRomaji("しんじて") == "shinjite");
}

TEST_CASE("romaji: digraphs, sokuon, long vowels, n") {
    CHECK(kanaToRomaji("きょう") == "kyou");
    CHECK(kanaToRomaji("じゃない") == "janai");
    CHECK(kanaToRomaji("ちゃんと") == "chanto");
    CHECK(kanaToRomaji("まっすぐ") == "massugu");
    CHECK(kanaToRomaji("ずっと") == "zutto");
    CHECK(kanaToRomaji("まって") == "matte");
    CHECK(kanaToRomaji("マッチ") == "matchi");
    CHECK(kanaToRomaji("スキー") == "sukii");
    CHECK(kanaToRomaji("ほんや") == "hon'ya");
    CHECK(kanaToRomaji("ファイト") == "faito");
    CHECK(kanaToRomaji("ティッシュ") == "tisshu");
    CHECK(kanaToRomaji("ウィンド") == "windo");
}

TEST_CASE("romaji: only lines that are all kana qualify") {
    CHECK(kanaToRomaji("Hello world").empty());
    CHECK(kanaToRomaji("你好").empty());
    CHECK(kanaToRomaji("私はあなたが好き").empty()); // kanji: from the provider
    CHECK(kanaToRomaji("あいして、る！") == "aishite, ru!");
}

TEST_CASE("romaji: fills only missing lines") {
    Lyrics l;
    l.lines.resize(3);
    l.lines[0].text = "さくら";
    l.lines[1].text = "桜";
    l.lines[2].text = "うた";
    l.lines[2].romaji = "UTA";
    CHECK(fillLocalRomaji(l) == 1);
    CHECK(l.lines[0].romaji == "sakura");
    CHECK(l.lines[1].romaji.empty());
    CHECK(l.lines[2].romaji == "UTA");
}

TEST_CASE("romaji: without the provider's, all Japanese lines or none") {
    Lyrics mixed;
    mixed.lines.resize(3);
    mixed.lines[0].text = "さくら";
    mixed.lines[1].text = "桜が咲いた";
    mixed.lines[2].text = "Hello";
    CHECK(fillLocalRomaji(mixed) == 0); // a kanji line: no romaji on the odd kana line alone
    CHECK_FALSE(hasRomaji(mixed));
    Lyrics kana;
    kana.lines.resize(3);
    kana.lines[0].text = "さくら";
    kana.lines[1].text = "Hello";
    kana.lines[2].text = "ララララ";
    CHECK(fillLocalRomaji(kana) == 2);
    CHECK(kana.lines[2].romaji == "rararara");
    Lyrics chinese;
    chinese.lines.resize(1);
    chinese.lines[0].text = "风从山背转过来";
    CHECK(fillLocalRomaji(chinese) == 0);
}

TEST_CASE("romaji: desktop rows put the romaji under the line, the translation under that") {
    Lyrics l;
    l.synced = true;
    for (int i = 0; i < 3; ++i) {
        Line x;
        x.t = i * 4000;
        x.text = "さくら";
        x.romaji = "sakura";
        l.lines.push_back(x);
    }
    auto r = desktopRows(l, 1, RowLayout::Alternate, true, true, true);
    CHECK(r.centered);
    CHECK(r.row[0] == RowContent{RowKind::Line, 1, true});
    CHECK(r.row[1] == RowContent{RowKind::Romaji, 1, true}); // fills with its line
    CHECK(r.row[2].kind == RowKind::None);
    l.lines[1].translation = "樱花";
    auto both = desktopRows(l, 1, RowLayout::Alternate, true, true, true);
    CHECK(both.row[1].kind == RowKind::Romaji);
    CHECK(both.row[2] == RowContent{RowKind::Translation, 1, false});
    l.lines[2].romaji.clear(); // an English line: its translation moves up, no gap
    l.lines[2].translation = "x";
    auto up = desktopRows(l, 2, RowLayout::Alternate, true, true, true);
    CHECK(up.row[1] == RowContent{RowKind::Translation, 2, false});
    CHECK(up.row[2].kind == RowKind::None);
    auto off = desktopRows(l, 1, RowLayout::Alternate, true, true, false);
    CHECK(off.row[1].kind == RowKind::Translation); // romaji off: as before
}

TEST_CASE("romaji: a romanised LRC merges into Line::romaji") {
    Lyrics base = parseLrc("[00:01.00]さくら\n[00:05.00]ことば\n");
    Lyrics ro = parseLrc("[00:01.00]sakura\n[00:05.10]kotoba\n");
    CHECK(mergeRomaji(base, ro) == 2);
    CHECK(hasRomaji(base));
    CHECK(base.lines[1].romaji == "kotoba");
    CHECK(parseLrc(romajiLrc(base)).lines.size() == 2);
}

TEST_CASE("romaji: fill fraction follows the line's edge") {
    Line x;
    x.t = 0;
    x.d = 4000;
    x.text = "abcd";
    x.words = {{0, 1000, "ab"}, {1000, 1000, "cd"}};
    auto k = buildKaraoke(x);
    CHECK(fillFraction(k, fillAt(k, -10)) == doctest::Approx(0.0));
    CHECK(fillFraction(k, fillAt(k, 500)) == doctest::Approx(0.25));
    CHECK(fillFraction(k, fillAt(k, 1000)) == doctest::Approx(0.5));
    CHECK(fillFraction(k, fillAt(k, 5000)) == doctest::Approx(1.0));
}
