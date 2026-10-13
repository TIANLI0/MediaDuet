package app.mediaduet.protocol

import java.util.Base64

object Hex {
    private val DIGITS = "0123456789abcdef".toCharArray()

    fun encode(bytes: ByteArray): String {
        val out = CharArray(bytes.size * 2)
        for (i in bytes.indices) {
            val v = bytes[i].toInt() and 0xff
            out[i * 2] = DIGITS[v ushr 4]
            out[i * 2 + 1] = DIGITS[v and 0x0f]
        }
        return String(out)
    }

    fun decode(text: String): ByteArray {
        require(text.length % 2 == 0) { "odd hex length" }
        val out = ByteArray(text.length / 2)
        for (i in out.indices) {
            val hi = Character.digit(text[i * 2], 16)
            val lo = Character.digit(text[i * 2 + 1], 16)
            require(hi >= 0 && lo >= 0) { "invalid hex" }
            out[i] = ((hi shl 4) or lo).toByte()
        }
        return out
    }

    /** True for lowercase hex strings of exactly [length] characters. */
    fun isLowerHex(text: String, length: Int): Boolean =
        text.length == length && text.all { it in '0'..'9' || it in 'a'..'f' }
}

object B64 {
    fun encode(bytes: ByteArray): String = Base64.getEncoder().encodeToString(bytes)

    /** Strict RFC 4648 (padded) decoding; throws IllegalArgumentException on malformed input. */
    fun decode(text: String): ByteArray = Base64.getDecoder().decode(text)
}

/** Counts Unicode code points (the unit used by the protocol's string limits, §6.1). */
fun String.codePointLength(): Int = codePointCount(0, length)

/** Truncates to at most [max] code points without splitting surrogate pairs. */
fun String.truncateCodePoints(max: Int): String {
    if (codePointLength() <= max) return this
    val end = offsetByCodePoints(0, max)
    return substring(0, end)
}
