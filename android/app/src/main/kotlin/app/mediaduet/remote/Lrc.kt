package app.mediaduet.remote

data class LyricWord(val startMs: Long, val text: String)

data class LyricLine(
    val timeMs: Long,
    val text: String,
    val words: List<LyricWord> = emptyList(),
    val translation: String? = null,
)

data class Lyrics(val lines: List<LyricLine>) {
    val hasTranslations: Boolean get() = lines.any { !it.translation.isNullOrEmpty() }

    /** Index of the line active at [positionMs], or -1 before the first line. */
    fun indexAt(positionMs: Long): Int {
        var lo = 0
        var hi = lines.size - 1
        var found = -1
        while (lo <= hi) {
            val mid = (lo + hi) ushr 1
            if (lines[mid].timeMs <= positionMs) {
                found = mid
                lo = mid + 1
            } else {
                hi = mid - 1
            }
        }
        return found
    }

    val isEmpty: Boolean get() = lines.isEmpty()
}

/**
 * LRC parser: multiple time tags per line, `[offset:±ms]` (positive = lyrics earlier),
 * enhanced word timing `<mm:ss.xx>`, metadata tags ignored. Lines sharing a timestamp are
 * merged as original + translation (common in Chinese lyric sources).
 */
/**
 * Attaches a separately delivered translation LRC (NetEase tlyric / QQ trans) to the original
 * lines: each non-empty translated line that is not "//" goes to the original line with the
 * nearest timestamp within ±[TOLERANCE_MS], unless it is identical to the original text.
 *
 * [timedLike]: the lyrics whose timestamps the translation follows when [original] is timed
 * differently (NetEase: tlyric follows the plain LRC, not the word-timed yrc, whose line starts
 * often differ by more than the tolerance). A translation then goes to the original line with
 * the same text as its LRC line (the nearest one), else to the line nearest in time. Mirrors the
 * host's `lyrics::mergeTranslation`.
 */
object TranslationMerge {
    const val TOLERANCE_MS = 600L
    /** A repeated line far away is another occurrence, not this one. */
    private const val SAME_LINE_MS = 15_000L
    private const val VIA_FALLBACK_MS = 2_000L

    private fun key(s: String) = buildString { for (c in s) if (!c.isWhitespace()) append(if (c.code < 0x80) c.lowercaseChar() else c) }

    private fun nearest(lines: List<LyricLine>, t: Long, tol: Long): Int {
        var best = -1
        var bestDistance = tol + 1
        for ((i, line) in lines.withIndex()) {
            val d = kotlin.math.abs(line.timeMs - t)
            if (d < bestDistance) {
                bestDistance = d
                best = i
            }
        }
        return best
    }

    fun merge(original: Lyrics, translationLrc: String, timedLike: Lyrics? = null): Lyrics {
        if (original.isEmpty) return original
        val translated = LrcParser.parse(translationLrc).lines
        val out = original.lines.toMutableList()
        for (t in translated) {
            val text = t.text.trim()
            if (text.isEmpty() || text == "//") continue
            var best = -1
            if (timedLike != null && !timedLike.isEmpty) {
                val via = timedLike.lines.getOrNull(nearest(timedLike.lines, t.timeMs, TOLERANCE_MS))
                val k = via?.let { key(it.text) }.orEmpty()
                if (via != null && k.isNotEmpty()) {
                    var d = SAME_LINE_MS + 1
                    for ((i, line) in out.withIndex()) {
                        val dist = kotlin.math.abs(line.timeMs - via.timeMs)
                        if (dist < d && key(line.text) == k) {
                            d = dist
                            best = i
                        }
                    }
                    if (best < 0) best = nearest(out, via.timeMs, maxOf(TOLERANCE_MS, VIA_FALLBACK_MS))
                }
            }
            if (best < 0) best = nearest(out, t.timeMs, TOLERANCE_MS)
            if (best < 0) continue
            val line = out[best]
            if (line.text.isEmpty() || text == line.text.trim()) continue
            out[best] = line.copy(translation = text)
        }
        return Lyrics(out)
    }
}

object LrcParser {
    private val BOM = 0xFEFF.toChar().toString()
    private val TAG = Regex("""\[([^\[\]]*)]""")
    private val TIME = Regex("""^\s*(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\s*$""")
    private val OFFSET = Regex("""^\s*offset\s*:\s*([+-]?\d+)\s*$""", RegexOption.IGNORE_CASE)
    private val WORD = Regex("""<(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?>""")

    private fun toMs(min: String, sec: String, frac: String?): Long {
        val fracMs = when (frac?.length) {
            null, 0 -> 0L
            1 -> frac.toLong() * 100
            2 -> frac.toLong() * 10
            else -> frac.toLong()
        }
        return min.toLong() * 60_000 + sec.toLong() * 1_000 + fracMs
    }

    private class Raw(val timeMs: Long, val text: String, val words: List<LyricWord>, val order: Int)

    /** Unsynced lyrics: non-blank lines with any [tag] prefixes removed. */
    fun plainLines(text: String): List<String> =
        text.removePrefix(BOM).lines()
            .map { line -> line.trim().replace(Regex("""^(\[[^\[\]]*])+"""), "").trim() }
            .filter { it.isNotEmpty() }

    fun parse(text: String): Lyrics {
        var offset = 0L
        val raw = ArrayList<Raw>()
        var order = 0
        for (rawLine in text.removePrefix(BOM).lines()) {
            var rest = rawLine.trim()
            val times = ArrayList<Long>(2)
            while (rest.startsWith("[")) {
                val m = TAG.find(rest) ?: break
                if (m.range.first != 0) break
                val content = m.groupValues[1]
                val t = TIME.matchEntire(content)
                if (t != null) {
                    times += toMs(t.groupValues[1], t.groupValues[2], t.groupValues[3].ifEmpty { null })
                } else {
                    OFFSET.matchEntire(content)?.let { offset = it.groupValues[1].toLongOrNull() ?: offset }
                }
                rest = rest.substring(m.range.last + 1)
            }
            if (times.isEmpty()) continue
            val words = ArrayList<LyricWord>()
            val matches = WORD.findAll(rest).toList()
            val plain: String
            if (matches.isNotEmpty()) {
                for ((i, m) in matches.withIndex()) {
                    val start = toMs(m.groupValues[1], m.groupValues[2], m.groupValues[3].ifEmpty { null })
                    val end = if (i + 1 < matches.size) matches[i + 1].range.first else rest.length
                    val w = rest.substring(m.range.last + 1, end)
                    if (w.isNotEmpty()) words += LyricWord(start, w)
                }
                val prefix = rest.substring(0, matches.first().range.first)
                if (prefix.isNotBlank() && words.isNotEmpty()) words.add(0, LyricWord(times.first(), prefix))
                plain = WORD.replace(rest, "").trim()
            } else {
                plain = rest.trim()
            }
            val base = times.first()
            for (t in times) {
                val shifted = words.map { it.copy(startMs = it.startMs + (t - base)) }
                raw += Raw(t, plain, shifted, order++)
            }
        }
        val sorted = raw.sortedWith(compareBy<Raw> { it.timeMs }.thenBy { it.order })
        val merged = ArrayList<LyricLine>(sorted.size)
        for (r in sorted) {
            val adjusted = (r.timeMs - offset).coerceAtLeast(0)
            val last = merged.lastOrNull()
            if (last != null && last.timeMs == adjusted && last.translation == null && r.text.isNotEmpty() && last.text.isNotEmpty() && r.text != last.text) {
                merged[merged.size - 1] = last.copy(translation = r.text)
            } else {
                merged += LyricLine(
                    adjusted,
                    r.text,
                    r.words.map { it.copy(startMs = (it.startMs - offset).coerceAtLeast(0)) },
                )
            }
        }
        return Lyrics(merged)
    }
}
