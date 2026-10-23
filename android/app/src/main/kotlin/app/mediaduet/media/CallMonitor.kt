package app.mediaduet.media

import android.content.Context
import android.media.AudioManager
import android.os.Build
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * True while this device is in a phone or VoIP call (audio mode in-call / in-communication); no
 * permission needed. Android 12+ reports mode changes; older versions are polled every 2 s.
 */
class CallMonitor(context: Context, private val scope: CoroutineScope) {
    private val audio = context.getSystemService(AudioManager::class.java)
    private val executor = ContextCompat.getMainExecutor(context)
    private val _active = MutableStateFlow(false)
    val active: StateFlow<Boolean> = _active
    private var poll: Job? = null

    fun start() {
        refresh()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            runCatching { audio.addOnModeChangedListener(executor) { refresh() } }
        } else {
            poll = scope.launch {
                while (isActive) {
                    delay(2_000)
                    refresh()
                }
            }
        }
    }

    private fun refresh() {
        val mode = runCatching { audio.mode }.getOrDefault(AudioManager.MODE_NORMAL)
        _active.value = mode == AudioManager.MODE_IN_CALL || mode == AudioManager.MODE_IN_COMMUNICATION ||
            (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && mode == AudioManager.MODE_COMMUNICATION_REDIRECT)
    }
}
