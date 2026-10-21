package app.mediaduet.remote

import app.mediaduet.protocol.MediaSessionInfo
import app.mediaduet.protocol.PlayStatus
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class LrcParserTest {
    @Test
    fun parsesBasicLinesAndMetadata() {
        val lrc = """
            [ti:晴天]
            [ar:周杰伦]
            [00:01.00]故事的小黄花
            [00:05.5]从出生那年就飘着
            [01:02.123]童年的荡秋千
            not a lyric line
        """.trimIndent()
        val l = LrcParser.parse(lrc)
        assertEquals(listOf(1_000L, 5_500L, 62_123L), l.lines.map { it.timeMs })
        assertEquals("故事的小黄花", l.lines[0].text)
        assertEquals(-1, l.indexAt(999))
        assertEquals(0, l.indexAt(1_000))
        assertEquals(1, l.indexAt(60_000))
        assertEquals(2, l.indexAt(999_999))
    }

    @Test
    fun multipleTimestampsPerLine() {
        val l = LrcParser.parse("[00:10.00][00:30.00]Chorus\n[00:20.00]Verse")
        assertEquals(listOf(10_000L, 20_000L, 30_000L), l.lines.map { it.timeMs })
        assertEquals(listOf("Chorus", "Verse", "Chorus"), l.lines.map { it.text })
    }

    @Test
    fun offsetShiftsLyricsEarlier() {
        val l = LrcParser.parse("[offset:+500]\n[00:02.00]a\n[00:00.20]b")
        assertEquals(listOf(0L, 1_500L), l.lines.map { it.timeMs })
        val neg = LrcParser.parse("[offset:-250]\n[00:01.00]a")
        assertEquals(1_250L, neg.lines[0].timeMs)
    }

    @Test
    fun enhancedWordTiming() {
        val l = LrcParser.parse("[00:12.00]<00:12.00>Hello <00:12.50>wide <00:13.20>world")
        val line = l.lines.single()
        assertEquals("Hello wide world", line.text)
        assertEquals(listOf(12_000L, 12_500L, 13_200L), line.words.map { it.startMs })
        assertEquals(listOf("Hello ", "wide ", "world"), line.words.map { it.text })
    }

    @Test
    fun translationsAreMergedAndEmptyLinesKept() {
        val l = LrcParser.parse("[00:01.00]Hello\n[00:01.00]你好\n[00:03.00]\n[00:04.00]Bye")
        assertEquals(3, l.lines.size)
        assertEquals("你好", l.lines[0].translation)
        assertEquals("", l.lines[1].text)
        assertNull(l.lines[2].translation)
    }

    @Test
    fun plainLyricsStripTagsAndBlankLines() {
        val text = listOf("[ti:Song]", "", "First verse", "  Second verse  ", "[ar:Someone]", "").joinToString("\n")
        assertEquals(listOf("First verse", "Second verse"), LrcParser.plainLines(text))
        assertEquals(0, LrcParser.parse(listOf("First verse", "Second verse").joinToString("\n")).lines.size)
    }

    @Test
    fun remotePositionInterpolation() {
        val s = MediaSessionInfo(
            "k", "a", "A", "t", "", "", "", "", 100_000, 10_000, 1.0, PlayStatus.PLAYING, emptyList(), null, "music",
        )
        assertEquals(12_050L, RemotePosition.at(s, positionAtMs = 950, nowMs = 3_000))
        assertEquals(100_000L, RemotePosition.at(s, 0, 1_000_000))
        assertEquals(10_000L, RemotePosition.at(s.copy(status = PlayStatus.PAUSED), 0, 50_000))
        assertNull(RemotePosition.at(s.copy(positionMs = null), 0, 10))
        assertEquals(14_000L, RemotePosition.at(s.copy(rate = 2.0), 1_000, 3_000))
    }
}
