package app.mediaduet.link

import app.mediaduet.protocol.Hex
import java.net.URLDecoder

/**
 * The QR code a PC shows for QR pairing (§3.5):
 * `mediaduet://pair?v=1&id=<deviceId>&t=<T>&n=<urlencoded name>&p=windows`.
 * Unknown parameters are ignored; anything else malformed is rejected.
 */
object QrPairing {
    const val TOKEN_BYTES = 16
    private const val PREFIX = "mediaduet://pair?"

    class Code(val peerId: String, val peerName: String, val platform: String?, val token: ByteArray)

    /** The pairing code in [text], or null when it is not a valid MediaDuet pairing code. */
    fun parse(text: String): Code? {
        val raw = text.trim()
        if (raw.length > 1024 || !raw.regionMatches(0, PREFIX, 0, PREFIX.length, ignoreCase = true)) return null
        val params = HashMap<String, String>()
        for (part in raw.substring(PREFIX.length).substringBefore('#').split('&')) {
            if (part.isEmpty()) continue
            val eq = part.indexOf('=')
            val key = if (eq < 0) part else part.substring(0, eq)
            val value = if (eq < 0) "" else part.substring(eq + 1)
            if (key in params) return null // ambiguous
            params[key] = runCatching { URLDecoder.decode(value, "UTF-8") }.getOrNull() ?: return null
        }
        if (params["v"] != "1") return null
        val id = params["id"]?.takeIf { Hex.isLowerHex(it, 32) } ?: return null
        val token = params["t"]?.let(::decodeToken) ?: return null
        val name = params["n"]?.trim()?.take(64).orEmpty()
        val platform = params["p"]?.takeIf { it.length <= 16 }
        return Code(id, name, platform, token)
    }

    /** T: exactly 16 bytes as base64url without padding (22 characters). */
    fun decodeToken(s: String): ByteArray? {
        if (s.length != 22 || s.any { !(it.isLetterOrDigit() && it.code < 128) && it != '-' && it != '_' }) return null
        val bytes = runCatching { java.util.Base64.getUrlDecoder().decode(s) }.getOrNull() ?: return null
        return bytes.takeIf { it.size == TOKEN_BYTES }
    }
}
