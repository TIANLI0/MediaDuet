package app.mediaduet.media

import kotlin.math.abs

/**
 * Optimistic playback state for the player (pure, immutable, unit-tested).
 *
 * * Play/pause flips immediately ([onToggle]) and is reconciled once the device reports the
 *   expected status, or dropped after [CONFIRM_MS] (the real state wins).
 * * A seek shows the target position immediately and keeps extrapolating it until the device
 *   reports a position near it, or for at most [SEEK_HOLD_MS] - no rubber-banding.
 * * A failed command rolls the corresponding optimistic value back ([onFailed]).
 */
data class OptimisticPlayback(
    val pendingPlaying: Boolean? = null,
    val playingSince: Long = 0,
    val seekPositionMs: Long? = null,
    val seekAt: Long = 0,
    val seekRate: Double = 1.0,
    val seekPlaying: Boolean = false,
) {
    enum class Kind { PLAY_PAUSE, SEEK }

    fun onToggle(currentlyPlaying: Boolean, nowMs: Long) = copy(pendingPlaying = !currentlyPlaying, playingSince = nowMs)

    fun onSeek(positionMs: Long, nowMs: Long, playing: Boolean, rate: Double) =
        copy(seekPositionMs = positionMs, seekAt = nowMs, seekPlaying = playing, seekRate = rate)

    /** Rollback after a failed / timed-out command. */
    fun onFailed(kind: Kind) = when (kind) {
        Kind.PLAY_PAUSE -> copy(pendingPlaying = null)
        Kind.SEEK -> copy(seekPositionMs = null)
    }

    /** Reconciles with the device's actual state; returns this instance when nothing changed. */
    fun reconcile(actualPlaying: Boolean, actualPositionMs: Long?, nowMs: Long): OptimisticPlayback {
        var next = this
        val p = pendingPlaying
        if (p != null && (p == actualPlaying || nowMs - playingSince > CONFIRM_MS)) next = next.copy(pendingPlaying = null)
        val seek = seekPositionMs
        if (seek != null) {
            val expected = expectedSeekPosition(nowMs)!!
            val confirmed = actualPositionMs != null && nowMs - seekAt > 150 && abs(actualPositionMs - expected) < SEEK_TOLERANCE_MS
            if (confirmed || nowMs - seekAt > SEEK_HOLD_MS) next = next.copy(seekPositionMs = null)
        }
        return if (next == this) this else next
    }

    fun displayPlaying(actualPlaying: Boolean): Boolean = pendingPlaying ?: actualPlaying

    fun displayPosition(actualPositionMs: Long, nowMs: Long): Long = expectedSeekPosition(nowMs) ?: actualPositionMs

    private fun expectedSeekPosition(nowMs: Long): Long? {
        val s = seekPositionMs ?: return null
        return if (seekPlaying) s + ((nowMs - seekAt).coerceAtLeast(0) * seekRate).toLong() else s
    }

    val isIdle: Boolean get() = pendingPlaying == null && seekPositionMs == null

    companion object {
        const val CONFIRM_MS = 3_000L
        const val SEEK_HOLD_MS = 2_000L
        const val SEEK_TOLERANCE_MS = 1_500L
    }
}

/**
 * Drag/remote synchronisation for a slider bound to a remote value (volume): while dragging and
 * for [HOLD_MS] after release, incoming values are ignored; sends are throttled to at most one per
 * [THROTTLE_MS] (≤ 10/s) and the final value is always sent on release (unless already sent).
 */
data class SliderSync(
    val dragging: Boolean = false,
    val value: Float = 0f,
    val holdUntil: Long = 0,
    val lastSentAt: Long = Long.MIN_VALUE / 2,
    val lastSent: Float? = null,
) {
    /** Returns the new state and the value to send now (null = do not send). */
    fun onDrag(v: Float, nowMs: Long): Pair<SliderSync, Float?> {
        val send = nowMs - lastSentAt >= THROTTLE_MS && v != lastSent
        val next = copy(dragging = true, value = v, lastSentAt = if (send) nowMs else lastSentAt, lastSent = if (send) v else lastSent)
        return next to (if (send) v else null)
    }

    fun onRelease(v: Float, nowMs: Long): Pair<SliderSync, Float?> {
        val send = v != lastSent
        return copy(dragging = false, value = v, holdUntil = nowMs + HOLD_MS, lastSentAt = if (send) nowMs else lastSentAt, lastSent = if (send) v else lastSent) to
            (if (send) v else null)
    }

    /** The value to display given the device-reported [remote] value. */
    fun displayed(remote: Float, nowMs: Long): Float = if (dragging || nowMs < holdUntil) value else remote

    companion object {
        const val THROTTLE_MS = 100L
        const val HOLD_MS = 600L
    }
}
