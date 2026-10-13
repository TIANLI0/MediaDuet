package app.mediaduet.media

import app.mediaduet.protocol.VolumeSet
import kotlin.math.roundToInt

/** Raw STREAM_MUSIC state (device index + mute) read straight from AudioManager. */
data class RawVolume(val index: Int, val muted: Boolean)

/**
 * Detects a silently ignored volume change (Android 17 background audio hardening: volume
 * APIs are no-ops for apps without a visible activity or a while-in-use foreground service, with
 * no exception). Pure and unit-tested.
 */
object VolumeRestriction {
    fun targetIndex(level: Double, min: Int, max: Int): Int = (min + level * (max - min)).roundToInt().coerceIn(min, max)

    /** The index [request] asks for (level and/or delta), or null when it only (un)mutes. */
    fun expectedIndex(request: VolumeSet, before: RawVolume, min: Int, max: Int): Int? {
        var index: Int? = request.level?.let { targetIndex(it, min, max) }
        request.delta?.takeIf { it != 0 }?.let { d -> index = ((index ?: before.index) + d).coerceIn(min, max) }
        return index
    }

    /** True when [request] should have changed the stream but [after] is exactly [before]. */
    fun ignored(request: VolumeSet, before: RawVolume, after: RawVolume, min: Int, max: Int): Boolean {
        if (after != before) return false
        val index = expectedIndex(request, before, min, max)
        val indexChange = index != null && index != before.index
        // Raising the level from a muted stream un-mutes (like the system slider).
        val unmuteByLevel = request.muted == null && index != null && index > min && before.muted
        val muteChange = request.muted != null && request.muted != before.muted
        return indexChange || unmuteByLevel || muteChange
    }
}
