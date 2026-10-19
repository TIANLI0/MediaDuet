package app.mediaduet.lyrics

import app.mediaduet.remote.LrcParser
import app.mediaduet.remote.LyricLine
import app.mediaduet.remote.LyricWord
import app.mediaduet.remote.Lyrics

/** Lyric source formats besides plain LRC (ports of the host's `md::lyrics` helpers). */
object LyricFormats {
    private val YRC_LINE = Regex("""^\[(\d+),(\d+)]""")
    private val YRC_WORD = Regex("""\((\d+),(\d+),(-?\d+)\)""")

    /**
     * NetEase "yrc" word-timed lyrics: `[lineStartMs,lineDurMs](wordStartMs,wordDurMs,0)word…`.
     * JSON metadata lines (`{"t":…}`) are ignored. A "(" in the lyric text only starts a timing
     * group when it parses as three numbers.
     */
    fun parseYrc(text: String): Lyrics {
        val out = ArrayList<LyricLine>()
        for (raw in text.removePrefix("\uFEFF").lines()) {
            val line = raw.trim()
            val head = YRC_LINE.find(line) ?: continue
            val start = head.groupValues[1].toLongOrNull() ?: continue
            val body = line.substring(head.range.last + 1)
            val groups = YRC_WORD.findAll(body).toList()
            val words = ArrayList<LyricWord>()
            for ((i, m) in groups.withIndex()) {
                val end = if (i + 1 < groups.size) groups[i + 1].range.first else body.length
                val w = body.substring(m.range.last + 1, end)
                val t = m.groupValues[1].toLongOrNull() ?: continue
                if (w.isNotEmpty()) words += LyricWord(t, w)
            }
            val plain = if (groups.isEmpty()) body.trim() else words.joinToString("") { it.text }.trim()
            out += LyricLine(start, plain, if (words.size > 1) words else emptyList())
        }
        out.sortBy { it.timeMs }
        return Lyrics(out)
    }

    /** Synced lyrics as LRC (enhanced `<mm:ss.xx>` word stamps when word-timed). */
    fun toLrc(l: Lyrics): String = buildString {
        for (line in l.lines) {
            append('[').append(stamp(line.timeMs)).append(']')
            if (line.words.isEmpty()) {
                append(line.text)
            } else {
                for (w in line.words) append('<').append(stamp(w.startMs)).append('>').append(w.text)
            }
            append('\n')
        }
    }

    /**
     * First line of translation LRCs written since translations are aligned through the plain
     * LRC; cached translations of word-timed lyrics without it may be incomplete (re-fetched).
     * Same as the host's `lyrics::kTranslationMark`.
     */
    const val TRANSLATION_MARK = "[md:tr2]"

    /** The translations as an LRC on the base lines' timestamps (starting with [TRANSLATION_MARK]). */
    fun translationLrc(l: Lyrics): String {
        val body = buildString {
            for (line in l.lines) {
                val t = line.translation ?: continue
                append('[').append(stamp(line.timeMs)).append(']').append(t).append('\n')
            }
        }
        return if (body.isEmpty()) body else "$TRANSLATION_MARK\n$body"
    }

    private fun stamp(ms: Long): String {
        val m = ms / 60_000
        val s = (ms / 1000) % 60
        val cs = (ms % 1000) / 10
        return "%02d:%02d.%02d".format(java.util.Locale.ROOT, m, s, cs)
    }

    /** Decodes the HTML entities QQ Music uses in lyrics. */
    fun decodeEntities(s: String): String {
        if ('&' !in s) return s
        val out = StringBuilder(s.length)
        var i = 0
        while (i < s.length) {
            val c = s[i]
            val semi = if (c == '&') s.indexOf(';', i) else -1
            if (semi < 0 || semi - i > 10) {
                out.append(c)
                i++
                continue
            }
            val ent = s.substring(i + 1, semi)
            val rep = when {
                ent == "apos" -> "'"
                ent == "quot" -> "\""
                ent == "amp" -> "&"
                ent == "lt" -> "<"
                ent == "gt" -> ">"
                ent == "nbsp" -> " "
                ent.startsWith("#x") || ent.startsWith("#X") -> ent.substring(2).toIntOrNull(16)?.let { String(Character.toChars(it)) }
                ent.startsWith("#") -> ent.substring(1).toIntOrNull()?.let { String(Character.toChars(it)) }
                else -> null
            }
            if (rep == null) {
                out.append(c)
                i++
            } else {
                out.append(rep)
                i = semi + 1
            }
        }
        return out.toString()
    }

    /** True if the lyrics only say "instrumental" (e.g. 纯音乐，请欣赏) rather than words. */
    fun isInstrumentalNotice(l: Lyrics): Boolean =
        l.lines.isNotEmpty() && l.lines.size <= 3 &&
            l.lines.any { "纯音乐" in it.text || "instrumental" in it.text.lowercase() }

    /** Parses LRC; null when it has no timed lines. */
    fun parseSynced(lrc: String): Lyrics? = LrcParser.parse(lrc).takeIf { !it.isEmpty }
}
