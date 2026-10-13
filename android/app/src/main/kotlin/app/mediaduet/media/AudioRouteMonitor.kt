package app.mediaduet.media

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import app.mediaduet.util.MdLog
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/** The phone's current media output: route kind plus the device's product name (if any). */
data class OutputState(val route: OutputRoute, val name: String?)

/**
 * Tracks where the phone's media audio plays (speaker / wired / Bluetooth). API 33+ asks the
 * audio policy for the media route (`getAudioDevicesForAttributes(USAGE_MEDIA)`); older versions
 * infer it from the connected outputs. Re-evaluated on [AudioDeviceCallback] changes and whenever
 * [refresh] is called (each snapshot), and reported through [onChange].
 */
class AudioRouteMonitor(context: Context, private val onChange: (OutputState) -> Unit) {
    private val audio = context.getSystemService(AudioManager::class.java)
    private val handler = Handler(Looper.getMainLooper())
    private val media = AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build()

    private val _state = MutableStateFlow(read())
    val state: StateFlow<OutputState> = _state

    private var started = false

    private val callback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?) = refresh()
        override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?) = refresh()
    }

    @Synchronized
    fun start() {
        if (started) return
        started = true
        audio.registerAudioDeviceCallback(callback, handler)
        refresh()
    }

    /** Re-reads the route; reports a change. Cheap enough to call per snapshot. */
    fun refresh() {
        val next = read()
        if (next != _state.value) {
            MdLog.i(TAG, "media output: ${next.route} (${next.name ?: "-"})")
            _state.value = next
            onChange(next)
        }
    }

    @Suppress("DEPRECATION") // isBluetoothA2dpOn: only the pre-33 fallback
    private fun read(): OutputState = try {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            val routed = audio.getAudioDevicesForAttributes(media)
            val route = OutputRoutes.fromRouted(routed.map { it.type })
            OutputState(route, routed.firstOrNull { OutputRoutes.of(it.type) == route }?.productName?.toString()?.takeIf { route != OutputRoute.SPEAKER })
        } else {
            val outputs = audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS).toList()
            val route = OutputRoutes.fromConnected(outputs.map { it.type }, audio.isBluetoothA2dpOn)
            OutputState(route, outputs.firstOrNull { OutputRoutes.of(it.type) == route }?.productName?.toString()?.takeIf { route != OutputRoute.SPEAKER })
        }
    } catch (e: RuntimeException) {
        MdLog.w(TAG, "cannot read the media route: $e")
        OutputState(OutputRoute.SPEAKER, null)
    }

    private companion object {
        const val TAG = "MD/Route"
    }
}
