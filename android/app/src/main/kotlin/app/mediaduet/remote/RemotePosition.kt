package app.mediaduet.remote

import app.mediaduet.protocol.MediaSessionInfo

/**
 * Receiver-side position extrapolation (§8.3): progress is never streamed; the receiver adds the
 * time elapsed since [positionAtMs], our monotonic clock at which the snapshot's positions hold
 * (the sender's `at` converted with the clock estimate, or arrival − rtt/2; §11.1).
 */
object RemotePosition {
    fun at(session: MediaSessionInfo, positionAtMs: Long, nowMs: Long): Long? {
        val base = session.positionMs ?: return null
        var pos = base
        if (session.isPlaying) {
            val elapsed = (nowMs - positionAtMs).coerceAtLeast(0)
            pos += (elapsed * session.rate).toLong()
        }
        session.durationMs?.let { if (pos > it) pos = it }
        return pos.coerceAtLeast(0)
    }
}
