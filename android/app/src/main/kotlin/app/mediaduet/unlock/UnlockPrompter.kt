package app.mediaduet.unlock

import android.app.ActivityManager
import android.content.Context
import android.graphics.PixelFormat
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import app.mediaduet.service.CompanionHelper
import app.mediaduet.service.Notifications
import app.mediaduet.util.MdLog
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import java.util.concurrent.atomic.AtomicLong
import javax.crypto.Cipher

/**
 * The one open "confirm on this phone" prompt of `unlock.request` / `unlock.enroll`
 * (docs/unlock.md). [prompt] posts the notification (and opens [UnlockActivity] at once when
 * Android allows it), then waits for the activity's system prompt, a cancel from the PC, a newer
 * request, or the timeout.
 */
class UnlockPrompter(private val context: Context) {

    enum class Kind { UNLOCK, ENROLL }

    class Prompt internal constructor(
        val token: Long,
        val kind: Kind,
        val peerId: String,
        /** Id of the PC's message; `unlock.cancel` names it in `re`. */
        val msgId: Long,
        val pcName: String,
        /** Authorized by the system prompt (crypto object), then used once. */
        val cipher: Cipher,
        internal val outcome: CompletableDeferred<Outcome> = CompletableDeferred(),
    )

    sealed interface Outcome {
        /** The user confirmed: [Prompt.cipher] may be used now. */
        data object Authenticated : Outcome
        data class Failed(val code: String) : Outcome
        /** The PC withdrew the request: no answer is sent. */
        data object Cancelled : Outcome
    }

    private val tokens = AtomicLong()
    private val _current = MutableStateFlow<Prompt?>(null)
    /** The prompt [UnlockActivity] shows; null when there is none (the activity then closes). */
    val current: StateFlow<Prompt?> = _current

    suspend fun prompt(kind: Kind, peerId: String, msgId: Long, pcName: String, cipher: Cipher, timeoutMs: Long): Outcome {
        val p = Prompt(tokens.incrementAndGet(), kind, peerId, msgId, pcName, cipher)
        synchronized(this) {
            // A new request replaces the old one; the old one is answered busy.
            _current.value?.outcome?.complete(Outcome.Failed(UnlockErrors.BUSY))
            _current.value = p
        }
        MdLog.i(TAG, "${kind.name.lowercase()} prompt for $pcName")
        if (mayOpenActivities()) withContext(Dispatchers.Main) { open() }
        // Also when the activity opened: Android may block a background launch silently, and the
        // notification brings the prompt back if it is dismissed by accident.
        Notifications.showUnlock(context, pcName, kind == Kind.ENROLL, timeoutMs)
        return try {
            withTimeoutOrNull(timeoutMs) { p.outcome.await() } ?: Outcome.Failed(UnlockErrors.TIMEOUT)
        } finally {
            synchronized(this) {
                p.outcome.complete(Outcome.Failed(UnlockErrors.TIMEOUT))
                if (_current.value === p) {
                    _current.value = null
                    Notifications.cancel(context, Notifications.ID_UNLOCK)
                }
            }
        }
    }

    /** The PC sent `unlock.cancel` for its message [re]. */
    fun cancel(peerId: String, re: Long) {
        val p = _current.value ?: return
        if (p.peerId == peerId && p.msgId == re) {
            MdLog.i(TAG, "${p.pcName} withdrew its request")
            p.outcome.complete(Outcome.Cancelled)
        }
    }

    /** From [UnlockActivity]: the system prompt's result for prompt [token]. */
    fun finish(token: Long, outcome: Outcome) {
        val p = _current.value ?: return
        if (p.token == token) p.outcome.complete(outcome)
    }

    /** On the main thread. */
    private fun open() {
        val overlay = if (Settings.canDrawOverlays(context)) addLaunchOverlay() else null
        try {
            context.startActivity(UnlockActivity.intent(context))
        } catch (e: Exception) {
            MdLog.w(TAG, "cannot open the unlock prompt directly: ${e.message}")
        }
        overlay?.let { v -> main.postDelayed({ removeOverlay(v) }, OVERLAY_HOLD_MS) }
    }

    /**
     * Android lets a background app open activities when it may draw over other apps (from
     * Android 15 only while one of its overlay windows is visible: [addLaunchOverlay]) or has a
     * companion-device association. Otherwise only the notification comes up.
     */
    private fun mayOpenActivities(): Boolean {
        val info = ActivityManager.RunningAppProcessInfo()
        ActivityManager.getMyMemoryState(info)
        if (info.importance <= ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND) return true
        if (Settings.canDrawOverlays(context)) return true
        return runCatching { CompanionHelper(context).associations().isNotEmpty() }.getOrDefault(false)
    }

    /** A 1 px transparent overlay window that takes no touches, held while the activity starts. */
    private fun addLaunchOverlay(): View? = try {
        val view = View(context)
        val params = WindowManager.LayoutParams(
            1, 1,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
            PixelFormat.TRANSLUCENT,
        ).apply { gravity = Gravity.TOP or Gravity.START }
        context.getSystemService(WindowManager::class.java).addView(view, params)
        view
    } catch (e: Exception) {
        MdLog.w(TAG, "cannot add the launch overlay: ${e.message}")
        null
    }

    private fun removeOverlay(view: View) {
        runCatching { context.getSystemService(WindowManager::class.java).removeView(view) }
    }

    private val main = Handler(Looper.getMainLooper())

    companion object {
        private const val TAG = "MD/Unlock"
        private const val OVERLAY_HOLD_MS = 3_000L
    }
}
