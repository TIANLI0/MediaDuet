package app.mediaduet.lyrics

import app.mediaduet.protocol.BadMessageException
import app.mediaduet.protocol.LyricsPush
import app.mediaduet.remote.Caret
import app.mediaduet.remote.LineRow
import app.mediaduet.remote.LyricTiming
import app.mediaduet.remote.LrcParser
import app.mediaduet.remote.LyricLine
import app.mediaduet.remote.LyricRows
import app.mediaduet.remote.Lyrics
import app.mediaduet.remote.TranslationMerge
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/** Mirrors windows/tests/test_romaji.cpp. */
class RomajiTest {
    private fun ro(s: String) = Romaji.kanaToRomaji(s)

    @Test fun hiraganaAndKatakana() {
        assertEquals("sakura", ro("さくら"))
        assertEquals("suki", ro("スキ"))
        assertEquals("arigatou", ro("ありがとう"))
        assertEquals("shinjite", ro("しんじて"))
    }

    @Test fun digraphsSokuonLongVowelsN() {
        assertEquals("kyou", ro("きょう"))
        assertEquals("janai", ro("じゃない"))
        assertEquals("chanto", ro("ちゃんと"))
        assertEquals("massugu", ro("まっすぐ"))
        assertEquals("zutto", ro("ずっと"))
        assertEquals("matte", ro("まって"))
        assertEquals("matchi", ro("マッチ"))
        assertEquals("sukii", ro("スキー"))
        assertEquals("hon'ya", ro("ほんや"))
        assertEquals("faito", ro("ファイト"))
        assertEquals("tisshu", ro("ティッシュ"))
        assertEquals("windo", ro("ウィンド"))
    }

    @Test fun onlyLinesThatAreAllKanaQualify() {
        assertEquals("", ro("Hello world"))
        assertEquals("", ro("你好"))
        assertEquals("", ro("私はあなたが好き")) // kanji: from the provider
        assertEquals("aishite, ru!", ro("あいして、る！"))
    }

    private fun lyrics(vararg lines: Pair<String, String?>) =
        Lyrics(lines.mapIndexed { i, (text, romaji) -> LyricLine(i * 4_000L, text, romaji = romaji) })

    @Test fun fillsOnlyMissingLines() {
        val l = Romaji.fillLocal(lyrics("さくら" to null, "桜" to null, "うた" to "UTA"))
        assertEquals(listOf("sakura", null, "UTA"), l.lines.map { it.romaji })
    }

    @Test fun withoutTheProvidersAllJapaneseLinesOrNone() {
        val mixed = lyrics("さくら" to null, "桜が咲いた" to null, "Hello" to null)
        assertSame(mixed, Romaji.fillLocal(mixed)) // a kanji line: no romaji on the odd kana line alone
        assertFalse(Romaji.fillLocal(mixed).hasRomaji)
        val kana = Romaji.fillLocal(lyrics("さくら" to null, "Hello" to null, "ララララ" to null))
        assertEquals(listOf("sakura", null, "rararara"), kana.lines.map { it.romaji })
        val chinese = lyrics("风从山背转过来" to null)
        assertSame(chinese, Romaji.fillLocal(chinese))
    }

    @Test fun romanisedLrcMergesIntoRomaji() {
        val base = LrcParser.parse("[00:01.00]さくら\n[00:05.00]ことば\n")
        val merged = TranslationMerge.mergeRomaji(base, "[00:01.00]sakura\n[00:05.10]kotoba\n")
        assertTrue(merged.hasRomaji)
        assertFalse(merged.hasTranslations)
        assertEquals("kotoba", merged.lines[1].romaji)
        assertEquals(2, LrcParser.parse(LyricFormats.romajiLrc(merged)).lines.size)
        // Romaji equal to the line itself (an English line) is skipped.
        assertNull(TranslationMerge.mergeRomaji(LrcParser.parse("[00:01.00]Hello"), "[00:01.00]Hello").lines[0].romaji)
    }

    @Test fun romajiFollowsThePlainLrcForWordTimedLyrics() {
        // NetEase: romalrc, like tlyric, is timed by the plain LRC, not the yrc.
        val yrc = LyricFormats.parseYrc("[10500,2000](10500,2000,0)さくら\n[20000,2000](20000,2000,0)ことば\n")
        val plain = LrcParser.parse("[00:12.74]さくら\n[00:21.20]ことば\n")
        val merged = TranslationMerge.mergeRomaji(yrc, "[00:12.74]sakura\n[00:21.20]kotoba\n", plain)
        assertEquals(listOf("sakura", "kotoba"), merged.lines.map { it.romaji })
    }

    @Test fun rowsCarryTheRomaji() {
        val rows = LyricRows.build(lyrics("さくら" to "sakura", "Hello" to null))
        assertEquals(listOf("sakura", null), rows.filterIsInstance<LineRow>().map { it.romaji })
        val frame = LyricsFrame.at(rows, LyricRows.endMs(rows), 0, translations = true, romaji = true)
        assertEquals("sakura", frame.romaji)
        assertEquals("sakura", frame.secondary)
        assertNull(LyricsFrame.at(rows, LyricRows.endMs(rows), 0, translations = true, romaji = false).romaji)
        // One secondary line (notification): the romaji wins over the translation when shown.
        val both = LyricsFrame("さくら", "樱花", null, "sakura")
        assertEquals("sakura", both.secondary)
        assertEquals("sakura\n樱花", both.secondaryRows)
        assertEquals("樱花", both.copy(romaji = null).secondary)
    }

    @Test fun romajiFillFollowsTheSungShareOfTheLine() {
        assertEquals(0f, LyricTiming.sungFraction("さくら", null), 0f)
        assertEquals(1f / 3, LyricTiming.sungFraction("さくら", Caret(1, 1, 0f)), 1e-6f)
        assertEquals(0.5f, LyricTiming.sungFraction("さくら", Caret(1, 2, 0.5f)), 1e-6f)
        assertEquals(1f, LyricTiming.sungFraction("さくら", Caret(3, 3, 0f)), 0f)
        // Code points, not UTF-16 units: 𠀋 is a surrogate pair.
        assertEquals(0.5f, LyricTiming.sungFraction("𠀋あ", Caret(2, 2, 0f)), 1e-6f)
    }

    @Test fun lyricsPushCarriesOptionalRomaji() {
        fun push(extra: String) = Json.parseToJsonElement(
            """{"trackId":"abc","format":"lrc","source":"netease","text":"[00:01.00]さくら\n[00:05.00]ことば"$extra}""",
        ).jsonObject
        assertNull(LyricsPush.parse(push("")).romaji)
        assertNull(LyricsPush.parse(push(""","romaji":" """")).romaji)
        val p = LyricsPush.parse(push(""","romaji":"[00:01.00]sakura","translation":"[00:01.00]樱花""""))
        assertEquals("[00:01.00]sakura", p.romaji)
        assertEquals("[00:01.00]樱花", p.translation)
        assertThrows(BadMessageException::class.java) { LyricsPush.parse(push(""","romaji":5""")) }
        val big = "x".repeat(256 * 1024 + 1)
        assertThrows(BadMessageException::class.java) { LyricsPush.parse(push(""","romaji":"$big"""")) }
    }
}
