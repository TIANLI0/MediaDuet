package app.mediaduet.unlock

import android.content.Context
import app.mediaduet.util.MdLog
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.JsonObject

/**
 * The phone's side of unlocking a PC (docs/unlock.md): answers `unlock.enroll` /
 * `unlock.request` after the user confirmed in the system prompt, withdraws prompts on
 * `unlock.cancel`, forgets enrollments on `unlock.revoke`.
 */
class UnlockController(context: Context, private val wallClock: () -> Long = System::currentTimeMillis) {
    val store = UnlockStore(context)
    val prompter = UnlockPrompter(context)

    /** `unlock.enroll` from PC [peerId] (its message [msgId]); the `unlock.result` body. */
    suspend fun enroll(peerId: String, msgId: Long, req: UnlockEnroll): JsonObject {
        val prepared = withContext(Dispatchers.Default) { store.enrollCipher(peerId, req.enrollment) }
        val cipher = prepared.cipher ?: return failed(prepared.failure)
        return when (val o = prompter.prompt(UnlockPrompter.Kind.ENROLL, peerId, msgId, req.pc, cipher, UnlockLimits.ENROLL_WAIT_MS)) {
            UnlockPrompter.Outcome.Authenticated -> {
                val ok = withContext(Dispatchers.Default) { store.completeEnroll(cipher, req.enrollment, peerId, req.pc, req.key, wallClock()) }
                req.key.fill(0)
                MdLog.i(TAG, if (ok) "${req.pc} can now be unlocked from this phone" else "enrolling ${req.pc} failed")
                if (ok) UnlockResult.ok() else UnlockResult.failed(UnlockErrors.NO_AUTH)
            }
            is UnlockPrompter.Outcome.Failed -> {
                req.key.fill(0)
                store.discard(peerId, req.enrollment) // a fresh Keystore key without a wrapped K is useless
                UnlockResult.failed(o.code)
            }
            UnlockPrompter.Outcome.Cancelled -> {
                req.key.fill(0)
                store.discard(peerId, req.enrollment)
                CANCELLED
            }
        }
    }

    /** `unlock.request` from PC [peerId]; the `unlock.result` body, or [CANCELLED] (no answer). */
    suspend fun request(peerId: String, msgId: Long, req: UnlockRequest): JsonObject {
        val prepared = withContext(Dispatchers.Default) { store.unlockCipher(peerId, req.enrollment) }
        val cipher = prepared.cipher ?: return failed(prepared.failure)
        return when (val o = prompter.prompt(UnlockPrompter.Kind.UNLOCK, peerId, msgId, req.pc, cipher, UnlockLimits.REQUEST_WAIT_MS)) {
            UnlockPrompter.Outcome.Authenticated -> {
                val key = withContext(Dispatchers.Default) { store.unwrap(cipher, peerId, req.enrollment, wallClock()) }
                    ?: return UnlockResult.failed(UnlockErrors.KEY_INVALIDATED)
                MdLog.i(TAG, "released the unlock key to ${req.pc}")
                UnlockResult.ok(key).also { key.fill(0) }
            }
            is UnlockPrompter.Outcome.Failed -> UnlockResult.failed(o.code)
            UnlockPrompter.Outcome.Cancelled -> CANCELLED
        }
    }

    fun cancel(peerId: String, re: Long) = prompter.cancel(peerId, re)

    /** `unlock.revoke` from PC [peerId]. */
    fun revoke(peerId: String, enrollment: String) {
        if (store.remove(peerId, enrollment)) MdLog.i(TAG, "the PC removed its unlock enrollment")
    }

    private fun failed(f: UnlockStore.Failure?): JsonObject {
        MdLog.i(TAG, "cannot answer: ${f?.code}")
        return UnlockResult.failed(f?.code ?: UnlockErrors.NOT_ENROLLED)
    }

    companion object {
        private const val TAG = "MD/Unlock"
        /** Marker: the PC withdrew the request, send nothing. */
        val CANCELLED = JsonObject(emptyMap())
    }
}
