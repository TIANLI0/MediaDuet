package app.mediaduet.remote

import app.mediaduet.protocol.MediaSessionInfo

/**
 * Receiver-side position extrapolation (§8.3): progress is never streamed; the receiver adds the
 * time elapsed since the snapshot arrived (monotonic clock) plus rtt/2.
 */
object RemotePosition {
    fun at(session: MediaSessionInfo, receivedAtMs: Long, rttMs: Long?, nowMs: Long): Long? {
        val base = session.positionMs ?: return null
        var pos = base
        if (session.isPlaying) {
            val elapsed = (nowMs - receivedAtMs).coerceAtLeast(0) + (rttMs ?: 0) / 2
            pos += (elapsed * session.rate).toLong()
        }
        session.durationMs?.let { if (pos > it) pos = it }
        return pos.coerceAtLeast(0)
    }
}
