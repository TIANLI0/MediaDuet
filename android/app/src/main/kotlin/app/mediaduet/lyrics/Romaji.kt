package app.mediaduet.lyrics

import app.mediaduet.remote.Lyrics

/**
 * Local kana → romaji (Hepburn), a port of the host's `md::lyrics` Romaji.cpp. Only lines written
 * in kana can be romanised here; kanji need a dictionary, so those lines get their romaji from the
 * provider (NetEase romalrc) or not at all.
 */
object Romaji {
    // Hiragana U+3041 .. U+3093.
    private val TABLE = arrayOf(
        "a", "a", "i", "i", "u", "u", "e", "e", "o", "o", // ぁ..ぉ
        "ka", "ga", "ki", "gi", "ku", "gu", "ke", "ge", "ko", "go", // か..ご
        "sa", "za", "shi", "ji", "su", "zu", "se", "ze", "so", "zo", // さ..ぞ
        "ta", "da", "chi", "ji", "", "tsu", "zu", "te", "de", "to", "do", // た..ど (っ is handled apart)
        "na", "ni", "nu", "ne", "no", // な..の
        "ha", "ba", "pa", "hi", "bi", "pi", "fu", "bu", "pu", "he", "be", "pe", "ho", "bo", "po", // は..ぽ
        "ma", "mi", "mu", "me", "mo", // ま..も
        "ya", "ya", "yu", "yu", "yo", "yo", // ゃ..よ
        "ra", "ri", "ru", "re", "ro", // ら..ろ
        "wa", "wa", "i", "e", "o", "n", // ゎ..ん
    )
    private const val FIRST = 0x3041
    private const val LAST = 0x3093

    init {
        check(TABLE.size == LAST - FIRST + 1) { "kana table out of step" }
    }

    private fun isVowel(c: Char) = c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'

    /** Katakana → hiragana; everything else unchanged. */
    private fun fold(c: Int) = if (c in 0x30A1..0x30F3) c - 0x60 else c

    private fun isHan(c: Int) = c in 0x4E00..0x9FFF || c in 0x3400..0x4DBF || c == 0x3005

    private fun punctuation(c: Int): String? = when (c) {
        0x3001 -> ", "
        0x3002 -> ". "
        0x30FB, 0x3000 -> " "
        0xFF01 -> "!"
        0xFF1F -> "?"
        0x300C, 0x300D, 0x300E, 0x300F -> "\""
        0x301C -> "~"
        else -> null
    }

    private fun startsVowelKana(c: Int) = when (c) {
        0x3042, 0x3044, 0x3046, 0x3048, 0x304A, // あ い う え お
        0x3084, 0x3086, 0x3088, // や ゆ よ
        -> true
        else -> false
    }

    /**
     * Hepburn romaji of an all-kana line; "" when the line has kanji (needs a dictionary) or no
     * kana at all (English or Chinese lines). Other characters pass through.
     */
    fun kanaToRomaji(text: String): String {
        val out = StringBuilder()
        var hasKana = false
        var sokuon = false
        fun emit(s0: String) {
            var s = s0
            if (sokuon && s.isNotEmpty() && !isVowel(s[0]) && s[0] != 'n') s = (if (s.startsWith("ch")) "t" else s[0].toString()) + s
            sokuon = false
            out.append(s)
        }
        var i = 0
        while (i < text.length) {
            val raw = text[i].code
            val c = fold(raw)
            if (isHan(raw)) return "" // needs a dictionary: leave it to the provider
            if (c == 0x3063) { // っ
                sokuon = true
                hasKana = true
                i++
                continue
            }
            if (raw == 0x30FC) { // ー lengthens the vowel before it
                if (out.isNotEmpty() && isVowel(out.last())) out.append(out.last())
                i++
                continue
            }
            val r = when {
                raw == 0x30F4 -> "vu" // ヴ
                raw == 0x30F5 -> "ka" // ヵ
                raw == 0x30F6 -> "ke" // ヶ
                c in FIRST..LAST -> TABLE[c - FIRST]
                else -> ""
            }
            if (r.isEmpty()) {
                sokuon = false
                out.append(punctuation(raw) ?: text[i].toString())
                i++
                continue
            }
            hasKana = true
            if (c == 0x3093) { // ん: n' before a vowel or y so "kan'i" differs from "kani"
                val apostrophe = i + 1 < text.length && startsVowelKana(fold(text[i + 1].code))
                emit(if (apostrophe) "n'" else "n")
                i++
                continue
            }
            // A small ゃゅょ or ぁぃぅぇぉ right after combines with this kana.
            val small = c == 0x3083 || c == 0x3085 || c == 0x3087 || (c <= 0x3049 && (c and 1) == 1)
            if (i + 1 < text.length && !small) {
                val n = fold(text[i + 1].code)
                val smallY = n == 0x3083 || n == 0x3085 || n == 0x3087
                val smallV = n == 0x3041 || n == 0x3043 || n == 0x3045 || n == 0x3047 || n == 0x3049
                if (smallY || smallV) {
                    val v = TABLE[n - FIRST].last() // the vowel: "ya" -> a
                    val stem = when {
                        smallY -> {
                            val soft = r == "shi" || r == "chi" || r == "ji"
                            r.dropLast(1) + if (soft) "" else "y" // drops the i (or the u of "fu")
                        }
                        r == "u" -> "w"
                        r == "vu" -> "v"
                        else -> r.dropLast(1)
                    }
                    emit(stem + v)
                    i += 2
                    continue
                }
            }
            emit(r)
            i++
        }
        return if (hasKana) out.toString() else ""
    }

    private fun hasJapanese(text: String): Boolean = text.any { ch ->
        val c = ch.code
        val f = fold(c)
        isHan(c) || f in FIRST..LAST || c == 0x30FC || c == 0x30F4
    }

    /**
     * Fills the romaji of every line that has none with [kanaToRomaji] (the host's
     * `lyrics::fillLocalRomaji`). Without the provider's romaji, a song gets local romaji only when
     * every Japanese line can have it: kanji lines can't, and romaji on just the odd kana line looks
     * broken.
     */
    fun fillLocal(l: Lyrics): Lyrics {
        if (l.isEmpty) return l
        if (!l.hasRomaji) {
            var any = false
            for (line in l.lines) {
                if (!hasJapanese(line.text)) continue
                if (kanaToRomaji(line.text).isEmpty()) return l
                any = true
            }
            if (!any) return l
        }
        var changed = false
        val out = l.lines.map { line ->
            if (!line.romaji.isNullOrEmpty() || line.text.isEmpty()) return@map line
            val r = kanaToRomaji(line.text)
            if (r.isEmpty()) line else line.copy(romaji = r).also { changed = true }
        }
        return if (changed) Lyrics(out) else l
    }
}
