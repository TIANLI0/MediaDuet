package app.mediaduet.unlock

import app.mediaduet.protocol.B64
import app.mediaduet.protocol.BadMessageException
import app.mediaduet.protocol.ErrorInfo
import app.mediaduet.protocol.Hex
import app.mediaduet.protocol.StrLimit
import app.mediaduet.protocol.requireBase64
import app.mediaduet.protocol.requireString
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

/**
 * Unlocking the PC with the phone (docs/unlock.md), the pure parts: the `unlock.*` bodies and
 * error codes. The PC keeps the Windows password sealed with a key K; the phone keeps K behind
 * a Keystore key that needs the user's fingerprint (or device PIN) for every use.
 */
object UnlockErrors {
    const val DENIED = "denied"
    const val NOT_ENROLLED = "not_enrolled"
    const val KEY_INVALIDATED = "key_invalidated"
    const val NO_AUTH = "no_auth"
    const val BUSY = "busy"
    const val TIMEOUT = "timeout"
    const val UNSUPPORTED = "unsupported"
    const val BAD_MESSAGE = "bad_message"
}

object UnlockLimits {
    /** K: AES-256 key. */
    const val KEY_BYTES = 32
    /** How long the phone waits for the user on `unlock.request` / `unlock.enroll`. */
    const val REQUEST_WAIT_MS = 60_000L
    const val ENROLL_WAIT_MS = 120_000L
}

private fun JsonObject.requireEnrollment(): String {
    val id = requireString("enrollment", 32)
    if (!Hex.isLowerHex(id, 32)) throw BadMessageException("enrollment must be 32 hex chars")
    return id
}

/** `unlock.enroll {enrollment, key, pc}`. [key] is K (32 bytes). */
class UnlockEnroll(val enrollment: String, val key: ByteArray, val pc: String) {
    companion object {
        fun parse(o: JsonObject) = UnlockEnroll(
            enrollment = o.requireEnrollment(),
            key = o.requireBase64("key", UnlockLimits.KEY_BYTES),
            pc = o.requireString("pc", StrLimit.DEVICE_NAME),
        )
    }
}

/** `unlock.request {enrollment, pc}`. */
data class UnlockRequest(val enrollment: String, val pc: String) {
    companion object {
        fun parse(o: JsonObject) = UnlockRequest(o.requireEnrollment(), o.requireString("pc", StrLimit.DEVICE_NAME))
    }
}

/** `unlock.revoke {enrollment}`. */
object UnlockRevoke {
    fun parse(o: JsonObject): String = o.requireEnrollment()
    fun toJson(enrollment: String) = buildJsonObject { put("enrollment", enrollment) }
}

/** `unlock.result {ok, key?, error?:{code}}`. */
object UnlockResult {
    fun ok(key: ByteArray? = null) = buildJsonObject {
        put("ok", true)
        if (key != null) put("key", B64.encode(key))
    }

    fun failed(code: String) = buildJsonObject {
        put("ok", false)
        put("error", ErrorInfo(code).toJson())
    }
}
