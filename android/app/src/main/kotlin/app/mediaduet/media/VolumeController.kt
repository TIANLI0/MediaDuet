package app.mediaduet.media

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.database.ContentObserver
import android.media.AudioManager
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import androidx.core.content.ContextCompat
import app.mediaduet.protocol.VolumeSet
import app.mediaduet.protocol.VolumeState
import app.mediaduet.util.MdLog
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlin.math.roundToInt

/**
 * The phone's media (STREAM_MUSIC) volume. Changes are observed event-driven - a ContentObserver on
 * Settings.System plus the volume / mute change broadcasts - never polled. Changes are applied
 * without showing the system volume UI. Devices with fixed volume report [fixed] and ignore sets.
 */
class VolumeController(private val context: Context) {
    private val audio = context.getSystemService(AudioManager::class.java)
    private val handler = Handler(Looper.getMainLooper())

    private val _state = MutableStateFlow(read())
    val state: StateFlow<VolumeState> = _state

    /** True when the device does not allow volume changes (e.g. fixed-volume outputs). */
    val fixed: Boolean get() = audio.isVolumeFixed

    private var started = false

    private val observer = object : ContentObserver(handler) {
        override fun onChange(selfChange: Boolean) = refresh()
    }

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) = refresh()
    }

    @Synchronized
    fun start() {
        if (started) return
        started = true
        context.contentResolver.registerContentObserver(Settings.System.CONTENT_URI, true, observer)
        ContextCompat.registerReceiver(
            context,
            receiver,
            IntentFilter().apply {
                addAction(ACTION_VOLUME_CHANGED)
                addAction(ACTION_MUTE_CHANGED)
            },
            ContextCompat.RECEIVER_NOT_EXPORTED,
        )
        refresh()
    }

    @Synchronized
    fun stop() {
        if (!started) return
        started = false
        runCatching { context.contentResolver.unregisterContentObserver(observer) }
        runCatching { context.unregisterReceiver(receiver) }
    }

    private fun refresh() {
        val next = read()
        if (next != _state.value) _state.value = next
    }

    private fun read(): VolumeState {
        val max = audio.getStreamMaxVolume(STREAM).coerceAtLeast(1)
        val min = runCatching { audio.getStreamMinVolume(STREAM) }.getOrDefault(0)
        val current = audio.getStreamVolume(STREAM)
        val range = (max - min).coerceAtLeast(1)
        return VolumeState(
            level = ((current - min).toDouble() / range).coerceIn(0.0, 1.0),
            muted = audio.isStreamMute(STREAM),
            steps = range,
        )
    }

    /** Result of [apply]: the truthful state afterwards, and whether the system ignored the change. */
    data class ApplyResult(val state: VolumeState, val ignored: Boolean)

    private fun raw() = RawVolume(audio.getStreamVolume(STREAM), audio.isStreamMute(STREAM))

    /**
     * Applies [request] (level, mute and/or delta in steps). A change the system silently ignored
     * (background audio hardening) is retried through [sessionFallback] (the active media
     * session's `MediaController.setVolumeTo`, when given) and otherwise reported as ignored.
     */
    fun apply(request: VolumeSet, sessionFallback: ((index: Int) -> Unit)? = null): ApplyResult {
        if (fixed) return ApplyResult(read(), ignored = false)
        val min = runCatching { audio.getStreamMinVolume(STREAM) }.getOrDefault(0)
        val max = audio.getStreamMaxVolume(STREAM)
        val before = raw()
        applyDirect(request)
        var after = raw()
        var ignored = VolumeRestriction.ignored(request, before, after, min, max)
        if (ignored && sessionFallback != null) {
            VolumeRestriction.expectedIndex(request, before, min, max)?.let { index ->
                runCatching { sessionFallback(index) }.onFailure { MdLog.w(TAG, "session volume fallback failed: $it") }
                after = raw()
                ignored = VolumeRestriction.ignored(request, before, after, min, max)
                MdLog.i(TAG, "session volume fallback ${if (ignored) "was also ignored" else "worked"}")
            }
        }
        if (ignored) MdLog.w(TAG, "volume change ignored by the system (background restriction?)")
        return ApplyResult(read().also { _state.value = it }, ignored)
    }

    private fun applyDirect(request: VolumeSet) {
        try {
            request.level?.let { level ->
                val max = audio.getStreamMaxVolume(STREAM)
                val min = runCatching { audio.getStreamMinVolume(STREAM) }.getOrDefault(0)
                val index = (min + level * (max - min)).roundToInt().coerceIn(min, max)
                audio.setStreamVolume(STREAM, index, 0)
                // Raising the level un-mutes, like the system slider does.
                if (request.muted == null && index > min && audio.isStreamMute(STREAM)) {
                    audio.adjustStreamVolume(STREAM, AudioManager.ADJUST_UNMUTE, 0)
                }
            }
            request.delta?.takeIf { it != 0 }?.let { d ->
                val dir = if (d > 0) AudioManager.ADJUST_RAISE else AudioManager.ADJUST_LOWER
                repeat(kotlin.math.abs(d)) { audio.adjustStreamVolume(STREAM, dir, 0) }
            }
            request.muted?.let { mute ->
                audio.adjustStreamVolume(STREAM, if (mute) AudioManager.ADJUST_MUTE else AudioManager.ADJUST_UNMUTE, 0)
            }
        } catch (e: SecurityException) {
            // Do Not Disturb policies (or hardening in "throw" mode) can forbid changes.
            MdLog.w(TAG, "volume change refused: ${e.message}")
        }
    }

    companion object {
        private const val TAG = "MD/Volume"
        private const val STREAM = AudioManager.STREAM_MUSIC

        // Sticky system broadcasts sent by AudioService on every stream volume / mute change.
        private const val ACTION_VOLUME_CHANGED = "android.media.VOLUME_CHANGED_ACTION"
        private const val ACTION_MUTE_CHANGED = "android.media.STREAM_MUTE_CHANGED_ACTION"
    }
}
