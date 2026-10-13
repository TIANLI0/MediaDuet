package app.mediaduet.link

import app.mediaduet.protocol.DeviceInfo
import app.mediaduet.util.MonoClock
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/**
 * Pairing mode (§3.3): pairing is only accepted while the user has opened the time-limited
 * window. At most one pairing attempt runs at a time; the SAS is shown to the user who accepts
 * or rejects it.
 */
class PairingController(private val scope: CoroutineScope, private val clock: MonoClock) {

    data class Prompt(
        val attemptId: Int,
        val peerId: String,
        val peerName: String,
        val peerPlatform: String,
        val sas: String,
        val transport: TransportKind,
        val localAccepted: Boolean,
    )

    data class Outcome(val attemptId: Int, val peerName: String, val success: Boolean, val reason: String?)

    data class State(
        /** Monotonic deadline of the pairing window, or null when pairing mode is off. */
        val windowUntilMs: Long? = null,
        val prompt: Prompt? = null,
        val lastOutcome: Outcome? = null,
    )

    private val _state = MutableStateFlow(State())
    val state: StateFlow<State> = _state

    private var windowJob: Job? = null
    private var attemptCounter = 0
    private var current: Attempt? = null

    fun isOpen(): Boolean {
        val until = _state.value.windowUntilMs ?: return false
        return clock.nowMs() < until
    }

    @Synchronized
    fun open(durationMs: Long = WINDOW_MS) {
        val until = clock.nowMs() + durationMs
        _state.update { it.copy(windowUntilMs = until, lastOutcome = null) }
        windowJob?.cancel()
        windowJob = scope.launch {
            delay(durationMs)
            closeWindow()
        }
    }

    @Synchronized
    fun closeWindow() {
        windowJob?.cancel()
        windowJob = null
        _state.update { it.copy(windowUntilMs = null) }
    }

    /** UI: the user accepted the SAS shown for [attemptId]. */
    @Synchronized
    fun accept(attemptId: Int) {
        val a = current ?: return
        if (a.id != attemptId) return
        _state.update { s -> s.copy(prompt = s.prompt?.copy(localAccepted = true)) }
        a.decision.complete(true)
    }

    /** UI: the user rejected the SAS (or dismissed the dialog). */
    @Synchronized
    fun reject(attemptId: Int) {
        val a = current ?: return
        if (a.id != attemptId) return
        a.decision.complete(false)
    }

    fun clearOutcome() = _state.update { it.copy(lastOutcome = null) }

    inner class Attempt internal constructor(val id: Int, val peer: DeviceInfo) {
        internal val decision = CompletableDeferred<Boolean>()

        suspend fun awaitDecision(): Boolean = decision.await()

        fun finish(success: Boolean, reason: String?) = this@PairingController.finish(this, success, reason)
    }

    /** Starts an attempt; returns null when pairing mode is closed or another attempt runs. */
    @Synchronized
    internal fun begin(peer: DeviceInfo, sas: String, transport: TransportKind): Attempt? {
        if (!isOpen() || current != null) return null
        val attempt = Attempt(++attemptCounter, peer)
        current = attempt
        _state.update {
            it.copy(
                prompt = Prompt(attempt.id, peer.id, peer.name, peer.platform, sas, transport, localAccepted = false),
                lastOutcome = null,
            )
        }
        return attempt
    }

    /** True while an attempt is in progress (used to answer `busy`). */
    @Synchronized
    internal fun isBusy(): Boolean = current != null

    @Synchronized
    private fun finish(attempt: Attempt, success: Boolean, reason: String?) {
        if (current !== attempt) return
        current = null
        attempt.decision.complete(false)
        _state.update {
            it.copy(prompt = null, lastOutcome = Outcome(attempt.id, attempt.peer.name, success, reason))
        }
        if (success) closeWindow()
    }

    companion object {
        const val WINDOW_MS = 5 * 60 * 1000L
        const val ATTEMPT_TIMEOUT_MS = 120 * 1000L
    }
}
