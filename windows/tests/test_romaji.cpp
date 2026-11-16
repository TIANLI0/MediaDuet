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
    CHECK(r.row[1] == RowContent{RowKind::Romaji, 1, false});
    CHECK(r.row[2].kind == RowKind::None);
    l.lines[1].translation = "樱花";
    auto both = desktopRows(l, 1, RowLayout::Alternate, true, true, true);
    CHECK(both.row[1].kind == RowKind::Romaji);
    CHECK(both.row[2] == RowContent{RowKind::Translation, 1, false});
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
