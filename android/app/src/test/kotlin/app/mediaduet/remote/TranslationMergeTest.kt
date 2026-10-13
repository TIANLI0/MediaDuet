package app.mediaduet.remote

import app.mediaduet.protocol.BadMessageException
import app.mediaduet.protocol.LyricsPush
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class TranslationMergeTest {
    private val original = LrcParser.parse(
        listOf("[00:01.00]Hello", "[00:05.00]World", "[00:09.00]Same", "[00:20.00]Far").joinToString("\n"),
    )

    @Test
    fun attachesToNearestLineWithinTolerance() {
        val tr = listOf(
            "[00:01.30]你好", // +300 ms -> Hello
            "[00:04.50]世界", // -500 ms -> World
            "[00:09.00]Same", // identical to the original -> skipped
            "[00:13.00]孤立", // nearest is 9 s / 20 s, beyond ±600 ms -> skipped
            "[00:20.00]//", // NetEase placeholder -> skipped
        ).joinToString("\n")
        val merged = TranslationMerge.merge(original, tr)
        assertEquals("你好", merged.lines[0].translation)
        assertEquals("世界", merged.lines[1].translation)
        assertNull(merged.lines[2].translation)
        assertNull(merged.lines[3].translation)
        assertTrue(merged.hasTranslations)
        assertFalse(original.hasTranslations)
        // Original text and timing are unchanged.
        assertEquals(original.lines.map { it.timeMs to it.text }, merged.lines.map { it.timeMs to it.text })
    }

    @Test
    fun toleranceBoundaryAndEmptyLines() {
        val atBoundary = TranslationMerge.merge(original, "[00:05.60]边界\n[00:01.00]")
        assertEquals("边界", atBoundary.lines[1].translation) // exactly 600 ms is accepted
        assertNull(atBoundary.lines[0].translation) // empty translated line ignored
        val beyond = TranslationMerge.merge(original, "[00:05.61]太远")
        assertFalse(beyond.hasTranslations)
        assertEquals(Lyrics(emptyList()), TranslationMerge.merge(Lyrics(emptyList()), "[00:01.00]x"))
    }

    @Test
    fun lyricsPushCarriesOptionalTranslation() {
        fun push(extra: String) = Json.parseToJsonElement(
            """{"trackId":"abc","format":"lrc","source":"netease","text":"[00:01.00]Hi"$extra}""",
        ).jsonObject
        assertNull(LyricsPush.parse(push("")).translation)
        assertNull(LyricsPush.parse(push(""","translation":"  """")).translation)
        assertEquals("[00:01.00]嗨", LyricsPush.parse(push(""","translation":"[00:01.00]嗨"""")).translation)
        assertThrows(BadMessageException::class.java) { LyricsPush.parse(push(""","translation":5""")) }
        val big = "x".repeat(256 * 1024 + 1)
        assertThrows(BadMessageException::class.java) { LyricsPush.parse(push(""","translation":"$big"""")) }
    }
}
