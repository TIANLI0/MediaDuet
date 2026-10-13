package app.mediaduet.protocol

import java.security.MessageDigest

/** §8.3: first 16 hex chars of SHA-256(title 0x1F artist 0x1F album 0x1F round(duration s)). */
object TrackId {
    fun of(title: String, artist: String, album: String, durationMs: Long?): String {
        val duration = if (durationMs == null) "" else Math.floorDiv(durationMs + 500, 1000L).toString()
        val data = listOf(title, artist, album, duration).joinToString("\u001f").toByteArray(Charsets.UTF_8)
        val digest = MessageDigest.getInstance("SHA-256").digest(data)
        return Hex.encode(digest).substring(0, 16)
    }
}
