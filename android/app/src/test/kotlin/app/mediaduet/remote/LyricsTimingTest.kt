package app.mediaduet.remote

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class LyricsTimingTest {
    // ---- smoothed clock ---------------------------------------------------------------------

    @Test
    fun clockSnapsOnStartAndFollowsTruthWhilePlaying() {
        val c = SmoothedClock()
        assertEquals(10_000L, c.update(10_000, 0.0, true, 1.0, "t1"))
        // Perfect extrapolation: display == truth, no slew.
        assertEquals(10_016L, c.update(10_016, 16.0, true, 1.0, "t1"))
        assertEquals(0.0, c.currentSlew, 1e-9)
    }

    @Test
    fun clockSlewsSmallErrorsAndNeverGoesBackwards() {
        val c = SmoothedClock()
        var now = 0.0
        var truth = 10_000.0
        c.update(truth.toLong(), now, true, 1.0, "t")
        // A late report: the source is 80 ms behind what we show. The display must slow down,
        // not jump back.
        truth -= 80
        var last = 10_000L
        repeat(180) {
            now += 16.0
            truth += 16.0
            val shown = c.update(truth.toLong(), now, true, 1.0, "t")
            assertTrue("moved backwards at frame $it: $shown < $last", shown >= last)
            last = shown
        }
        // Slew is clamped to 5 % and the error is absorbed within ~3 s.
        assertTrue(kotlin.math.abs(last - truth) < 10)
    }

    @Test
    fun clockSlewIsClampedToFivePercent() {
        val c = SmoothedClock()
        c.update(0, 0.0, true, 1.0, "t")
        c.update(16 + 250, 16.0, true, 1.0, "t") // 250 ms ahead: within the snap threshold
        assertEquals(0.05, c.currentSlew, 1e-9)
    }

    @Test
    fun clockSnapsOnSeekPauseRateAndTrackChange() {
        val c = SmoothedClock()
        c.update(10_000, 0.0, true, 1.0, "t")
        assertEquals(60_000L, c.update(60_000, 16.0, true, 1.0, "t")) // seek: |error| > 300 ms
        assertEquals(59_000L, c.update(59_000, 32.0, true, 1.0, "t")) // seek back
        assertEquals(59_100L, c.update(59_100, 48.0, false, 1.0, "t")) // pause
        assertEquals(59_100L, c.update(59_100, 64.0, true, 1.0, "t")) // resume
        assertEquals(59_200L, c.update(59_200, 80.0, true, 2.0, "t")) // rate change
        assertEquals(5L, c.update(5, 96.0, true, 2.0, "t2")) // new track
    }

    // ---- delay --------------------------------------------------------------------------------

    @Test
    fun delayComposition() {
        assertEquals(480, LyricsDelay.totalMs(autoEnabled = true, outputLatencyMs = 180, manualOffsetMs = 300))
        assertEquals(300, LyricsDelay.totalMs(autoEnabled = false, outputLatencyMs = 180, manualOffsetMs = 300))
        assertEquals(-200, LyricsDelay.totalMs(true, null, -200))
        assertEquals(2_000 + 5_000, LyricsDelay.totalMs(true, 9_999, 9_999)) // both clamped
        assertEquals(9_520L, LyricsDelay.displayPosition(10_000, 480))
        assertEquals(10_300L, LyricsDelay.displayPosition(10_000, -300)) // negative offset: lyrics earlier
        assertEquals(0L, LyricsDelay.displayPosition(100, 480))
        assertEquals(5_000, LyricsDelay.step(4_950, 1))
        assertEquals(-100, LyricsDelay.step(0, -1))
    }

    @Test
    fun signedSecondsLabel() {
        assertEquals("+0.3", LyricsDelay.signedSeconds(300))
        assertEquals("−1.2", LyricsDelay.signedSeconds(-1_200))
        assertEquals("0.0", LyricsDelay.signedSeconds(0))
        assertEquals("+5.0", LyricsDelay.signedSeconds(5_000))
    }

    // ---- char mapping / fill ------------------------------------------------------------------

    private fun words(vararg w: Pair<Long, String>) = LyricLine(w.first().first, w.joinToString("") { it.second }, w.map { LyricWord(it.first, it.second) })

    @Test
    fun wordSpansTrimLineButKeepInnerWhitespace() {
        val line = words(1_000L to "  Hello ", 1_500L to " ", 2_000L to "wide  ", 2_600L to "world  ")
        val t = LyricTiming.prepare(line, 4_000)
        assertEquals("Hello  wide  world", t.text)
        // "  Hello " -> [0, 6) after trimming 2 leading spaces; " " -> [6, 7); "wide  " -> [7, 13); "world  " -> [13, 18) clamped.
        assertEquals(listOf(0 to 6, 6 to 7, 7 to 13, 13 to 18), t.spans.map { it.start to it.end })
        assertEquals(listOf(1_500L, 2_000L, 2_600L, 4_000L), t.spans.map { it.endMs })
    }

    @Test
    fun wordFillMovesWithinWordsAndHoldsBetween() {
        val line = LyricLine(1_000, "", listOf(LyricWord(1_000, "Hi "), LyricWord(1_400, "there")))
        val t = LyricTiming.prepare(line, 3_000)
        assertNull(LyricTiming.caret(t, 999))
        assertEquals(Caret(0, 3, 0f), LyricTiming.caret(t, 1_000))
        assertEquals(Caret(0, 3, 0.5f), LyricTiming.caret(t, 1_200))
        assertEquals(Caret(3, 8, 0f), LyricTiming.caret(t, 1_400))
        assertEquals(Caret(8, 8, 0f), LyricTiming.caret(t, 3_000)) // complete after the last word
        // A gap between words holds at the next word's start.
        val gap = LyricTiming.prepare(LyricLine(0, "", listOf(LyricWord(0, "a"), LyricWord(1_000, "b"))), 2_000)
        assertEquals(Caret(0, 1, 0.5f), LyricTiming.caret(gap, 500))
    }

    @Test
    fun lastWordRunsToNextLineOrOneSecond() {
        val t = LyricTiming.prepare(LyricLine(0, "", listOf(LyricWord(0, "x"))), null)
        assertEquals(1_000L, t.spans.single().endMs)
    }

    @Test
    fun surrogatePairsAreNeverSplit() {
        val text = "a🎵b" // a 🎵 b: 3 code points, 4 UTF-16 units
        val t = LyricTiming.prepare(LyricLine(0, text), 10_000)
        // Line-timed: sweep = min(9000, max(1200, 3 × 450)) = 1350 ms over 3 code points.
        assertEquals(1_350L, t.spans.single().endMs)
        val mid = LyricTiming.codePointStep(t.text, LyricTiming.caret(t, 675)!!) // half-way: inside the emoji
        assertEquals(1, mid.from)
        assertEquals(3, mid.to) // the whole surrogate pair
        assertEquals(0.5f, mid.fraction, 1e-4f)
        val end = LyricTiming.codePointStep(t.text, LyricTiming.caret(t, 2_000)!!)
        assertEquals(Caret(4, 4, 0f), end)
    }

    @Test
    fun lineSweepRuleMatchesWindows() {
        assertEquals(1_200L, LyricTiming.sweepMs(2, 10_000)) // floor 1200 ms
        assertEquals(4_500L, LyricTiming.sweepMs(10, 10_000)) // 10 × 450
        assertEquals(2_700L, LyricTiming.sweepMs(10, 3_000)) // capped at 0.9 × d
        assertEquals(3_600L, LyricTiming.sweepMs(40, 0)) // unknown d = 4 s
        val last = LyricTiming.prepare(LyricLine(0, "four"), null)
        assertEquals(5_000L, last.durationMs)
        assertEquals(1_800L, last.spans.single().endMs) // 4 × 450
    }

    // ---- rows / gaps --------------------------------------------------------------------------

    @Test
    fun rowsInsertIntroAndInterludeGaps() {
        val l = LrcParser.parse(
            listOf(
                "[00:05.00]First",
                "[00:07.00]Second",
                "[00:09.00]",
                "[00:20.00]<00:20.00>Third <00:20.50>words",
                "[00:30.00]Fourth",
                "[00:31.00]",
                "[00:33.00]Fifth",
            ).joinToString("\n"),
        )
        val rows = LyricRows.build(l)
        val kinds = rows.map { if (it is GapRow) "gap@${it.startMs}" else "line@${it.startMs}" }
        assertEquals(
            listOf("gap@0", "line@5000", "line@7000", "gap@9000", "line@20000", "gap@22000", "line@30000", "line@33000"),
            kinds,
        )
        assertEquals(-1, LyricRows.activeIndex(emptyList(), 0))
        assertEquals(0, LyricRows.activeIndex(rows, 100))
        assertEquals(3, LyricRows.activeIndex(rows, 15_000))
        assertEquals(4, LyricRows.activeIndex(rows, 20_100))
        assertEquals(7, LyricRows.activeIndex(rows, 999_999))
        // Last line (line-timed "Fifth", 5 s) + 3 s hold.
        assertEquals(33_000L + 5_000 + 3_000, LyricRows.endMs(rows))
    }

    @Test
    fun longLineTimedLinesAreNotMistakenForGaps() {
        // 30 code points × 450 + 2 s = 15.5 s of assumed singing: a 16 s line has no 4 s gap.
        val l = LrcParser.parse("[00:00.00]" + "x".repeat(30) + "\n[00:16.00]next")
        assertEquals(2, LyricRows.build(l).size)
        val short = LrcParser.parse("[00:00.00]hey\n[00:16.00]next")
        assertTrue(LyricRows.build(short)[1] is GapRow)
    }
}
