package app.mediaduet.lyrics

import kotlin.math.abs

/**
 * Title / artist matching, a port of the host's `md::lyrics::normalize` / `matchScore` so both
 * platforms accept the same online results. Also decides whether two track ids are the same song
 * (players re-report metadata with a slightly different duration or album, which changes the
 * protocol track id but must not reload or hide the lyrics).
 */
object SongMatch {
    data class Candidate(val title: String, val artist: String, val album: String = "", val durationMs: Long? = null)

    private val CUTS = listOf(" feat.", " feat ", " ft.", " - ")

    /** Lower case; bracketed parts ((), [], （）, 【】), "feat." / " - " suffixes and ASCII punctuation removed. */
    fun normalize(s: String): String {
        val lower = s.lowercase()
        val out = StringBuilder()
        var depth = 0
        for (c in lower) {
            when (c) {
                '(', '[', '（', '【' -> depth++
                ')', ']', '）', '】' -> if (depth > 0) depth--
                else -> if (depth == 0) out.append(c)
            }
        }
        var cut = out.toString()
        for (token in CUTS) {
            val p = cut.indexOf(token)
            if (p > 0) cut = cut.substring(0, p)
        }
        val res = StringBuilder()
        for (c in cut) {
            if (c.code < 0x80) {
                if (c.isLetterOrDigit()) res.append(c)
            } else if (!c.isWhitespace()) {
                res.append(c)
            }
        }
        return res.toString()
    }

    fun splitArtists(s: String): List<String> =
        s.split('/', ',', '&', '、', ';', '，').map { normalize(it) }.filter { it.isNotEmpty() }

    /** Score in [0, 1]; 0 means "do not use". Duration differences over 5 s are rejected. */
    fun score(want: Candidate, have: Candidate): Double {
        if (want.durationMs != null && have.durationMs != null && abs(want.durationMs - have.durationMs) > 5_000) return 0.0
        val wt = normalize(want.title)
        val ht = normalize(have.title)
        if (wt.isEmpty() || ht.isEmpty()) return 0.0
        var score = when {
            wt == ht -> 0.6
            wt.contains(ht) || ht.contains(wt) -> 0.35
            else -> return 0.0
        }
        val wa = splitArtists(want.artist)
        val ha = splitArtists(have.artist)
        score += if (wa.isEmpty() || ha.isEmpty()) {
            0.1
        } else if (wa.any { x -> ha.any { y -> x == y || x.contains(y) || y.contains(x) } }) {
            0.3
        } else {
            -0.2
        }
        if (want.durationMs != null && have.durationMs != null && abs(want.durationMs - have.durationMs) <= 2_000) score += 0.1
        return score.coerceIn(0.0, 1.0)
    }

    /**
     * True when two reports describe the same recording: same normalized title and artist, and
     * durations within 3 s (or one unknown). Album changes are ignored.
     */
    fun sameSong(a: Candidate, b: Candidate): Boolean {
        val ta = normalize(a.title)
        if (ta.isEmpty() || ta != normalize(b.title)) return false
        if (normalize(a.artist) != normalize(b.artist)) return false
        return a.durationMs == null || b.durationMs == null || abs(a.durationMs - b.durationMs) <= 3_000
    }

    /** Search keyword: the title without bracketed suffixes plus the first artist. */
    fun searchQuery(title: String, artist: String): String {
        var t = title
        for (open in listOf(" (", "（", " [", "【")) {
            val p = t.indexOf(open)
            if (p > 0) t = t.substring(0, p)
        }
        var a = artist
        for (sep in listOf("/", ",", "&", "、", ";")) {
            val p = a.indexOf(sep)
            if (p > 0) a = a.substring(0, p)
        }
        return "$t $a".trim()
    }
}
