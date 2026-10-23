package app.mediaduet.remote

import kotlin.math.abs
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToLong

/**
 * Smoothed playback clock for lyrics (pure, unit-tested). It is sampled every frame with the
 * source's own extrapolated position ("truth", which jumps a little whenever a new position report
 * arrives) and the frame time:
 *
 * * play / pause, a rate change or a new [epoch] (track change) snap to the truth;
 * * an error beyond [snapThresholdMs] (a seek) snaps;
 * * smaller errors are slewed out by running the local clock at `rate × (1 + c)` with
 *   `c = clamp(error / 1000 ms, ±5 %)` (same rule as Windows, `docs/ipc.md`), so the displayed
 *   position never moves backwards while playing and lines never flicker.
 */
class SmoothedClock(
    private val snapThresholdMs: Double = 300.0,
    private val maxSlew: Double = 0.05,
    private val correctionMs: Double = 1_000.0,
) {
    private var display = 0.0
    private var lastNowMs = Double.NaN
    private var playing = false
    private var rate = 1.0
    private var epoch: Any? = null
    private var slew = 0.0

    /** Current slew (fraction of the rate; positive = running faster to catch up). */
    val currentSlew: Double get() = slew

    fun reset() {
        lastNowMs = Double.NaN
        slew = 0.0
    }

    /**
     * Returns the displayed position at [nowMs] given the source position [truthMs] at the same
     * instant. [nowMs] must be monotonic (frame time).
     */
    fun update(truthMs: Long, nowMs: Double, playing: Boolean, rate: Double, epoch: Any?): Long {
        val r = if (rate.isFinite() && rate > 0) rate else 1.0
        val discontinuity = lastNowMs.isNaN() || playing != this.playing || r != this.rate || epoch != this.epoch || nowMs < lastNowMs
        if (discontinuity || !playing) {
            display = truthMs.toDouble()
            slew = 0.0
        } else {
            val dt = nowMs - lastNowMs
            val predicted = display + dt * r * (1 + slew)
            val error = truthMs - predicted
            if (abs(error) > snapThresholdMs) {
                display = truthMs.toDouble()
                slew = 0.0
            } else {
                display = predicted
                slew = (error / correctionMs).coerceIn(-maxSlew, maxSlew)
            }
        }
        lastNowMs = nowMs
        this.playing = playing
        this.rate = r
        this.epoch = epoch
        return display.roundToLong()
    }
}

/**
 * Lyrics delay: `display position = smoothed position − delay`, where delay = the device's output
 * latency (when automatic compensation is on) + the user's per-app offset (positive = lyrics later).
 */
object LyricsDelay {
    const val MIN_OFFSET_MS = -5_000
    const val MAX_OFFSET_MS = 5_000
    const val STEP_MS = 100
    const val MAX_OUTPUT_LATENCY_MS = 2_000

    fun totalMs(autoEnabled: Boolean, outputLatencyMs: Int?, manualOffsetMs: Int): Int {
        val auto = if (autoEnabled) (outputLatencyMs ?: 0).coerceIn(0, MAX_OUTPUT_LATENCY_MS) else 0
        return auto + manualOffsetMs.coerceIn(MIN_OFFSET_MS, MAX_OFFSET_MS)
    }

    fun displayPosition(smoothedMs: Long, delayMs: Int): Long = (smoothedMs - delayMs).coerceAtLeast(0)

    /** One step of the manual offset control, clamped to the allowed range. */
    fun step(offsetMs: Int, steps: Int): Int = (offsetMs + steps * STEP_MS).coerceIn(MIN_OFFSET_MS, MAX_OFFSET_MS)

    /** "+0.3", "−1.2", "0.0" (seconds, one decimal, typographic minus). */
    fun signedSeconds(offsetMs: Int): String {
        val tenths = Math.round(offsetMs / 100.0).toInt()
        val sign = when {
            tenths > 0 -> "+"
            tenths < 0 -> "−"
            else -> ""
        }
        val a = abs(tenths)
        return "$sign${a / 10}.${a % 10}"
    }
}

/**
 * A word (or the whole line for line-timed lyrics) as a UTF-16 span `[start, end)` of the displayed
 * text, filled over `[startMs, endMs)`.
 */
data class WordSpan(val start: Int, val end: Int, val startMs: Long, val endMs: Long)

/**
 * A line prepared for display: the trimmed text and its timed spans. [durationMs] is the line's
 * duration (until the next line; 5 s for the last line) and [vocalEndMs] an estimate of when the
 * singing ends (used to detect instrumental gaps).
 */
data class TimedLine(
    val text: String,
    val spans: List<WordSpan>,
    val startMs: Long,
    val durationMs: Long,
    val vocalEndMs: Long,
    val wordTimed: Boolean,
)

/**
 * The fill edge, like the Windows `FillSpan`: it lies [fraction] of the way from the caret at UTF-16
 * offset [from] to the caret at [to] (both code-point boundaries); everything before [from] is sung.
 */
data class Caret(val from: Int, val to: Int, val fraction: Float)

/**
 * Karaoke timing shared with Windows (`docs/ipc.md` "Lyrics timing", reference
 * `windows/core/src/lyrics/Karaoke.cpp`):
 *
 * * word-timed lines: the words are concatenated, the outer whitespace of the line is trimmed
 *   (spans shift / clamp; a whitespace-only word becomes an empty span) and inner whitespace
 *   belongs to the word carrying it. A word lasts until the next word starts; the last word until
 *   the next line (or 1 s). Between words the edge holds at the next word start;
 * * line-timed lines sweep evenly over `min(0.9 × d, max(1200 ms, codePoints × 450 ms))`, d = the
 *   time to the next line (5 s for the last line, 4 s if unknown).
 */
object LyricTiming {
    const val LAST_LINE_MS = 5_000L
    const val UNKNOWN_LINE_MS = 4_000L
    const val LAST_WORD_MS = 1_000L

    /** Line-timed sweep duration (same rule as Windows; counts Unicode code points). */
    fun sweepMs(codePoints: Int, durationMs: Long): Long {
        val d = if (durationMs > 0) durationMs.toDouble() else UNKNOWN_LINE_MS.toDouble()
        return max(1L, min(0.9 * d, max(1_200.0, codePoints * 450.0)).toLong())
    }

    fun prepare(line: LyricLine, nextLineMs: Long?): TimedLine {
        if (line.words.isEmpty()) {
            val text = line.text.trim()
            val d = if (nextLineMs != null) max(0L, nextLineMs - line.timeMs) else LAST_LINE_MS
            val sweep = sweepMs(text.codePointCount(0, text.length), d)
            val spans = if (text.isEmpty()) emptyList() else listOf(WordSpan(0, text.length, line.timeMs, line.timeMs + sweep))
            return TimedLine(text, spans, line.timeMs, d, line.timeMs + sweep, wordTimed = false)
        }
        val full = line.words.joinToString("") { it.text }
        val lead = full.length - full.trimStart().length
        val text = full.trim()
        val spans = ArrayList<WordSpan>(line.words.size)
        var offset = 0
        for ((i, w) in line.words.withIndex()) {
            val s = offset
            offset += w.text.length
            val next = line.words.getOrNull(i + 1)?.startMs
            val d = when {
                next != null -> max(0L, next - w.startMs)
                else -> max(0L, (nextLineMs ?: (w.startMs + LAST_WORD_MS)) - w.startMs)
            }
            spans += WordSpan((s - lead).coerceIn(0, text.length), (offset - lead).coerceIn(0, text.length), w.startMs, w.startMs + max(1L, d))
        }
        val last = spans.last()
        val lineEnd = nextLineMs ?: last.endMs
        // Singing ends with the last word; its timed duration often covers a following pause.
        val vocalEnd = last.startMs + min(last.endMs - last.startMs, 1_500L)
        return TimedLine(text, spans, min(line.timeMs, spans.first().startMs), max(0L, lineEnd - line.timeMs), vocalEnd, wordTimed = true)
    }

    /** The fill edge at [positionMs]; null before the line (its first word) starts. */
    fun caret(t: TimedLine, positionMs: Long): Caret? {
        val total = t.text.length
        if (total == 0 || t.spans.isEmpty()) return null
        if (positionMs < t.spans.first().startMs) return null
        for (s in t.spans) {
            if (positionMs < s.startMs) return Caret(s.start, s.start, 0f) // gap between words: hold
            if (positionMs < s.endMs) {
                val f = if (s.end > s.start) (positionMs - s.startMs).toFloat() / (s.endMs - s.startMs) else 0f
                return Caret(s.start, s.end, f.coerceIn(0f, 1f))
            }
        }
        return Caret(total, total, 0f)
    }

    /**
     * The edge inside [c] as one code-point step: `(lo, hi, fraction)` with lo / hi adjacent
     * code-point boundaries (surrogate pairs are never split). Used when a span wraps across
     * visual lines, so the edge can move line by line.
     */
    fun codePointStep(text: String, c: Caret): Caret {
        val cps = text.codePointCount(c.from, c.to)
        if (cps == 0) return Caret(c.from, c.from, 0f)
        val target = c.fraction.toDouble() * cps
        val k = floor(target).toInt()
        if (k >= cps) return Caret(c.to, c.to, 0f)
        val lo = text.offsetByCodePoints(c.from, k)
        val hi = lo + Character.charCount(text.codePointAt(lo))
        return Caret(lo, hi, (target - k).toFloat())
    }
}

/** One row of the lyrics list: a line or an instrumental gap ("•••"). */
sealed interface LyricRow {
    val startMs: Long
}

data class LineRow(val index: Int, val line: TimedLine, val translation: String?, override val startMs: Long) : LyricRow

data class GapRow(override val startMs: Long, val endMs: Long) : LyricRow

object LyricRows {
    /** Instrumental gaps at least this long get a breathing "•••" row. */
    const val MIN_GAP_MS = 4_000L

    fun build(lyrics: Lyrics): List<LyricRow> {
        val lines = lyrics.lines
        val rows = ArrayList<LyricRow>(lines.size + 4)
        val firstSung = lines.indexOfFirst { it.text.isNotBlank() }
        if (firstSung < 0) return rows
        if (lines[firstSung].timeMs >= MIN_GAP_MS) rows += GapRow(0, lines[firstSung].timeMs)
        for (i in firstSung until lines.size) {
            val line = lines[i]
            val next = lines.getOrNull(i + 1)?.timeMs
            if (line.text.isBlank()) {
                // An empty timestamp line marks an interlude; short ones are dropped.
                if (next != null && next - line.timeMs >= MIN_GAP_MS) rows += GapRow(line.timeMs, next)
                continue
            }
            val timed = LyricTiming.prepare(line, next)
            // Row starts stay sorted (binary search) even for odd word timestamps.
            val start = max(timed.startMs, rows.lastOrNull()?.startMs ?: 0L)
            rows += LineRow(i, timed, line.translation?.takeIf { it.isNotBlank() }, start)
            if (next == null || lines[i + 1].text.isBlank()) continue
            val vocalEnd = vocalEnd(timed, next)
            if (next - vocalEnd >= MIN_GAP_MS && vocalEnd > start) rows += GapRow(vocalEnd, next)
        }
        return rows
    }

    /**
     * When the singing of a line is estimated to end: shortly after the last word for word-timed
     * lines; for line-timed lines a generous estimate (code points × 450 ms + 2 s, at least the
     * fill sweep), so long held notes are not mistaken for interludes.
     */
    private fun vocalEnd(t: TimedLine, nextMs: Long): Long {
        if (t.wordTimed) return t.vocalEndMs
        val cps = t.text.codePointCount(0, t.text.length)
        return min(nextMs, max(t.vocalEndMs, t.startMs + cps * 450L + 2_000L))
    }

    const val END_HOLD_MS = 3_000L

    /**
     * When the lyrics are over: [END_HOLD_MS] after the last line is sung. From then on no line is
     * current, so the last line does not look "now playing" during a long outro.
     */
    fun endMs(rows: List<LyricRow>): Long = when (val last = rows.lastOrNull()) {
        null -> Long.MAX_VALUE
        is LineRow -> max(last.startMs + min(last.line.durationMs, LyricTiming.LAST_LINE_MS), last.line.vocalEndMs) + END_HOLD_MS
        is GapRow -> last.endMs
    }

    /**
     * The line row being sung at [positionMs]: null before the lyrics, in a gap and from [endMs]
     * on (where only one line fits, e.g. the mini player and the lyrics notification).
     */
    fun lineAt(rows: List<LyricRow>, endMs: Long, positionMs: Long): LineRow? =
        if (positionMs >= endMs) null else rows.getOrNull(activeIndex(rows, positionMs)) as? LineRow

    /** The next position after [positionMs] at which [lineAt] / [activeIndex] can change, null if none. */
    fun nextChange(rows: List<LyricRow>, endMs: Long, positionMs: Long): Long? {
        if (rows.isEmpty() || positionMs >= endMs) return null
        val next = rows.getOrNull(activeIndex(rows, positionMs) + 1)?.startMs
        return if (next != null) min(next, endMs) else endMs.takeIf { it != Long.MAX_VALUE }
    }

    /** Index of the row active at [positionMs] (the last one started), -1 before the first. */
    fun activeIndex(rows: List<LyricRow>, positionMs: Long): Int {
        var lo = 0
        var hi = rows.size - 1
        var found = -1
        while (lo <= hi) {
            val mid = (lo + hi) ushr 1
            if (rows[mid].startMs <= positionMs) {
                found = mid
                lo = mid + 1
            } else {
                hi = mid - 1
            }
        }
        return found
    }
}
