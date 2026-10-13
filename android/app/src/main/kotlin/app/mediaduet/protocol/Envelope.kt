package app.mediaduet.protocol

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction

/** Message envelope (§6). */
data class Envelope(
    val v: Int,
    val type: String,
    val id: Long,
    val ts: Long,
    val src: String,
    val sid: String,
    val re: Long?,
    val body: JsonObject,
) {
    fun toJson(): JsonObject = buildJsonObject {
        put("v", v)
        put("type", type)
        put("id", id)
        put("ts", ts)
        put("src", src)
        put("sid", sid)
        if (re != null) put("re", re)
        put("body", body)
    }

    fun encode(): ByteArray = Json.encodeToString(JsonObject.serializer(), toJson()).toByteArray(Charsets.UTF_8)
}

/**
 * A received message violated §6 rules. [re] is the offending message id when it could be parsed,
 * so that an `error {code:"bad_message", re}` can be returned.
 */
class BadMessageException(message: String, val re: Long? = null, val code: String = ErrorCode.BAD_MESSAGE) :
    Exception(message)

object ErrorCode {
    const val NOT_PAIRED = "not_paired"
    const val AUTH_FAILED = "auth_failed"
    const val VERSION_UNSUPPORTED = "version_unsupported"
    const val BAD_MESSAGE = "bad_message"
    const val NOT_FOUND = "not_found"
    const val UNSUPPORTED = "unsupported"
    const val BUSY = "busy"
    const val TIMEOUT = "timeout"
    const val INTERNAL = "internal"
    const val NO_SESSION = "no_session"
    const val ACTION_UNSUPPORTED = "action_unsupported"
    const val LINK_LOST = "link_lost"
    const val TOO_LARGE = "too_large"
    /** The OS ignored the request because the app is in the background (e.g. Android 17 volume hardening). */
    const val BACKGROUND_RESTRICTED = "background_restricted"
}

object EnvelopeCodec {
    const val MAX_ID = (1L shl 53) - 1
    private val TYPE_REGEX = Regex("^[a-z0-9.\\-]{1,48}$")

    /** Strict UTF-8 decoding: malformed sequences are rejected rather than replaced. */
    fun decodeUtf8(bytes: ByteArray): String {
        val decoder = Charsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
        return try {
            decoder.decode(ByteBuffer.wrap(bytes)).toString()
        } catch (e: CharacterCodingException) {
            throw BadMessageException("invalid UTF-8")
        }
    }

    /**
     * Parses and validates an envelope. [maxBytes] is the applicable size limit (16 KiB for
     * handshake frames, 64 KiB for CONTROL records).
     */
    fun decode(bytes: ByteArray, maxBytes: Int): Envelope {
        if (bytes.size > maxBytes) throw BadMessageException("message too large", code = ErrorCode.TOO_LARGE)
        val text = decodeUtf8(bytes)
        val root = try {
            Json.parseToJsonElement(text)
        } catch (e: Exception) {
            throw BadMessageException("malformed JSON")
        }
        if (root !is JsonObject) throw BadMessageException("envelope is not an object")
        val id = root.requireLong("id")
        if (id < 1 || id > MAX_ID) throw BadMessageException("id out of range")
        try {
            val v = root.requireLong("v")
            if (v < 0 || v > Int.MAX_VALUE) throw BadMessageException("bad v")
            val type = root.requireString("type")
            if (!TYPE_REGEX.matches(type)) throw BadMessageException("bad type")
            val ts = root.requireLong("ts")
            val src = root.requireString("src")
            if (!Hex.isLowerHex(src, 32)) throw BadMessageException("bad src")
            val sid = root.requireString("sid")
            if (sid.isNotEmpty() && !Hex.isLowerHex(sid, 32)) throw BadMessageException("bad sid")
            val re = root.optLong("re")
            if (re != null && (re < 1 || re > MAX_ID)) throw BadMessageException("re out of range")
            val body = root["body"] as? JsonObject ?: throw BadMessageException("body must be an object")
            return Envelope(v.toInt(), type, id, ts, src, sid, re, body)
        } catch (e: BadMessageException) {
            throw BadMessageException(e.message ?: "bad message", id, e.code)
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Strict field accessors used by every body parser. Unknown fields are ignored (forward compat);
// known fields with the wrong JSON type are rejected.
// ---------------------------------------------------------------------------------------------

private fun JsonElement.asIntegerOrNull(): Long? {
    if (this !is JsonPrimitive || isString || this is JsonNull) return null
    val c = content
    if (c.isEmpty() || c.any { it == '.' || it == 'e' || it == 'E' }) return null
    return c.toLongOrNull()
}

fun JsonObject.requireLong(name: String): Long {
    val e = this[name] ?: throw BadMessageException("missing $name")
    return e.asIntegerOrNull() ?: throw BadMessageException("$name must be an integer")
}

fun JsonObject.optLong(name: String): Long? {
    val e = this[name] ?: return null
    if (e is JsonNull) return null
    return e.asIntegerOrNull() ?: throw BadMessageException("$name must be an integer")
}

fun JsonObject.requireString(name: String, maxCodePoints: Int = Int.MAX_VALUE): String =
    optString(name, maxCodePoints) ?: throw BadMessageException("missing $name")

fun JsonObject.optString(name: String, maxCodePoints: Int = Int.MAX_VALUE): String? {
    val e = this[name] ?: return null
    if (e is JsonNull) return null
    if (e !is JsonPrimitive || !e.isString) throw BadMessageException("$name must be a string")
    val s = e.content
    if (s.codePointLength() > maxCodePoints) throw BadMessageException("$name too long")
    return s
}

fun JsonObject.requireBool(name: String): Boolean =
    optBool(name) ?: throw BadMessageException("missing $name")

fun JsonObject.optBool(name: String): Boolean? {
    val e = this[name] ?: return null
    if (e is JsonNull) return null
    if (e !is JsonPrimitive || e.isString) throw BadMessageException("$name must be a boolean")
    return when (e.content) {
        "true" -> true
        "false" -> false
        else -> throw BadMessageException("$name must be a boolean")
    }
}

fun JsonObject.optDouble(name: String): Double? {
    val e = this[name] ?: return null
    if (e is JsonNull) return null
    if (e !is JsonPrimitive || e.isString) throw BadMessageException("$name must be a number")
    val d = e.content.toDoubleOrNull() ?: throw BadMessageException("$name must be a number")
    if (d.isNaN() || d.isInfinite()) throw BadMessageException("$name must be finite")
    return d
}

fun JsonObject.requireObject(name: String): JsonObject =
    optObject(name) ?: throw BadMessageException("missing $name")

fun JsonObject.optObject(name: String): JsonObject? {
    val e = this[name] ?: return null
    if (e is JsonNull) return null
    return e as? JsonObject ?: throw BadMessageException("$name must be an object")
}

fun JsonObject.optArray(name: String): JsonArray? {
    val e = this[name] ?: return null
    if (e is JsonNull) return null
    return e as? JsonArray ?: throw BadMessageException("$name must be an array")
}

fun JsonObject.optStringList(name: String, maxItems: Int, maxCodePoints: Int): List<String>? {
    val array = optArray(name) ?: return null
    if (array.size > maxItems) throw BadMessageException("$name has too many items")
    return array.map { e ->
        if (e !is JsonPrimitive || !e.isString) throw BadMessageException("$name items must be strings")
        if (e.content.codePointLength() > maxCodePoints) throw BadMessageException("$name item too long")
        e.content
    }
}

/** Base64 field with an exact decoded length. */
fun JsonObject.requireBase64(name: String, exactLength: Int? = null): ByteArray {
    val s = requireString(name, 4096)
    val bytes = try {
        B64.decode(s)
    } catch (e: IllegalArgumentException) {
        throw BadMessageException("$name is not base64")
    }
    if (exactLength != null && bytes.size != exactLength) throw BadMessageException("$name has wrong length")
    return bytes
}
