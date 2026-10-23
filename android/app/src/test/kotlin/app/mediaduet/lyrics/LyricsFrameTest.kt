package app.mediaduet.lyrics

import app.mediaduet.remote.LyricLine
import app.mediaduet.remote.LyricRows
import app.mediaduet.remote.Lyrics
import org.junit.Assert.assertEquals
import org.junit.Test

class LyricsFrameTest {
    private val lyrics = Lyrics(
        listOf(
            LyricLine(1_000, "Hello", translation = "你好"),
            LyricLine(3_000, "  "),
            LyricLine(5_000, "World"),
            LyricLine(7_000, "Again"),
            LyricLine(20_000, "Last"),
        ),
    )
    private val rows = LyricRows.build(lyrics)
    private val endMs = LyricRows.endMs(rows)

    private fun at(p: Long, translations: Boolean = true) = LyricsFrame.at(rows, endMs, p, translations)

    @Test
    fun beforeFirstLineShowsNoTextAndWaitsForIt() {
        assertEquals(LyricsFrame(null, null, 1_000), at(500))
    }

    @Test
    fun lineWithTranslationOnlyWhenEnabled() {
        assertEquals(LyricsFrame("Hello", "你好", 5_000), at(1_000))
        assertEquals(LyricsFrame("Hello", null, 5_000), at(2_000, translations = false))
    }

    @Test
    fun shortBlankLineKeepsThePreviousLine() {
        assertEquals(LyricsFrame("Hello", "你好", 5_000), at(3_500))
    }

    @Test
    fun silenceAfterALineIsAGap() {
        // "Again" (5 code points): singing ends by 7 s + 5 × 450 ms + 2 s = 11.25 s; the next line is at 20 s.
        assertEquals(LyricsFrame(null, null, 20_000), at(12_000))
    }

    @Test
    fun nothingAfterTheEnd() {
        assertEquals(LyricsFrame("Last", null, endMs), at(20_500))
        assertEquals(LyricsFrame(null, null, null), at(endMs))
    }
}
