package app.mediaduet.protocol

import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

/** Message type names (§8). */
object MsgType {
    const val PAIR_REQUEST = "pair.request"
    const val PAIR_RESPONSE = "pair.response"
    const val PAIR_CONFIRM = "pair.confirm"
    const val PAIR_RESULT = "pair.result"
    const val HELLO = "hello"
    const val HELLO_ACK = "hello.ack"
    const val AUTH = "auth"
    const val ERROR = "error"
    const val SESSION_READY = "session.ready"
    const val LINK_PRIMARY = "link.primary"
    const val PING = "ping"
    const val PONG = "pong"
    const val DEVICE_INFO = "device.info"
    const val TRANSPORT_INFO = "transport.info"
    const val MEDIA_SNAPSHOT = "media.snapshot"
    const val MEDIA_COMMAND = "media.command"
    const val MEDIA_COMMAND_RESULT = "media.command.result"
    const val ARTWORK_GET = "artwork.get"
    const val LYRICS_PUSH = "lyrics.push"
    const val LYRICS_NONE = "lyrics.none"
    const val POLICY_STATUS = "policy.status"
    const val POLICY_CONFIG = "policy.config"
    const val AUDIO_OFFER = "audio.offer"
    const val AUDIO_ANSWER = "audio.answer"
    const val AUDIO_STOP = "audio.stop"
    const val BYE = "bye"
    const val VOLUME_STATE = "volume.state"
    const val VOLUME_SET = "volume.set"
}

object Caps {
    const val MEDIA_SOURCE = "media.source"
    const val MEDIA_CONTROL = "media.control"
    const val ARTWORK = "artwork"
    const val LYRICS = "lyrics"
    const val POLICY_HOST = "policy.host"
    const val POLICY_CLIENT = "policy.client"
    const val AUDIO_RECV_PCM = "audio.recv.pcm"
    const val AUDIO_SEND_PCM = "audio.send.pcm"
    const val VOLUME = "volume"
}

/** §6.1 string limits (code points). */
object StrLimit {
    const val DEVICE_NAME = 64
    const val TEXT = 512
    const val APP_NAME = 128
    const val APP_ID = 256
    const val SESSION_KEY = 300
    const val ERROR_MESSAGE = 512
    /** Fields the spec does not bound explicitly (model, os, reasons, ...). */
    const val MISC = 128
}

data class DeviceInfo(
    val id: String,
    val name: String,
    val platform: String,
    val model: String = "",
    val os: String = "",
    val app: String = "",
) {
    fun toJson(): JsonObject = buildJsonObject {
        put("id", id)
        put("name", name.truncateCodePoints(StrLimit.DEVICE_NAME))
        put("platform", platform)
        put("model", model.truncateCodePoints(StrLimit.MISC))
        put("os", os.truncateCodePoints(StrLimit.MISC))
        put("app", app.truncateCodePoints(StrLimit.MISC))
    }

    companion object {
        fun parse(o: JsonObject): DeviceInfo {
            val id = o.requireString("id")
            if (!Hex.isLowerHex(id, 32)) throw BadMessageException("device.id must be 32 hex chars")
            return DeviceInfo(
                id = id,
                name = o.requireString("name", StrLimit.DEVICE_NAME),
                platform = o.requireString("platform", 32),
                model = o.optString("model", StrLimit.MISC) ?: "",
                os = o.optString("os", StrLimit.MISC) ?: "",
                app = o.optString("app", StrLimit.MISC) ?: "",
            )
        }
    }
}

data class ArtworkRef(val hash: String, val mime: String, val size: Int) {
    fun toJson(): JsonObject = buildJsonObject {
        put("hash", hash)
        put("mime", mime)
        put("size", size)
    }

    companion object {
        fun parse(o: JsonObject): ArtworkRef {
            val hash = o.requireString("hash")
            if (!Hex.isLowerHex(hash, 64)) throw BadMessageException("artwork.hash must be sha256 hex")
            val size = o.requireLong("size")
            if (size < 0 || size > Limits.MAX_ARTWORK_SIZE) throw BadMessageException("artwork.size out of range")
            return ArtworkRef(hash, o.optString("mime", 64) ?: "image/jpeg", size.toInt())
        }
    }
}

object PlayStatus {
    const val PLAYING = "playing"
    const val PAUSED = "paused"
    const val STOPPED = "stopped"
    const val BUFFERING = "buffering"
    const val UNKNOWN = "unknown"
    val ALL = setOf(PLAYING, PAUSED, STOPPED, BUFFERING, UNKNOWN)
}

object Action {
    const val PLAY = "play"
    const val PAUSE = "pause"
    const val TOGGLE = "toggle"
    const val NEXT = "next"
    const val PREVIOUS = "previous"
    const val SEEK = "seek"
    const val STOP = "stop"
    val ALL = listOf(PLAY, PAUSE, TOGGLE, NEXT, PREVIOUS, SEEK, STOP)
}

object MediaKind {
    const val MUSIC = "music"
    const val VIDEO = "video"
    const val OTHER = "other"
    val ALL = setOf(MUSIC, VIDEO, OTHER)
}

data class MediaSessionInfo(
    val key: String,
    val appId: String,
    val appName: String,
    val title: String,
    val artist: String,
    val album: String,
    val albumArtist: String,
    val trackId: String,
    val durationMs: Long?,
    val positionMs: Long?,
    val rate: Double,
    val status: String,
    val actions: List<String>,
    val artwork: ArtworkRef?,
    val kind: String,
    /** The app's own volume (Windows per-app mixer), when the sender can control it. */
    val volume: AppVolume? = null,
    /**
     * Estimated delay (ms, 0..[MAX_OUTPUT_LATENCY_MS]) between the reported position and the sound
     * being audible on the device that plays it (e.g. Bluetooth headphones). Optional; null = unknown.
     */
    val outputLatencyMs: Int? = null,
) {
    val isPlaying: Boolean get() = status == PlayStatus.PLAYING

    fun toJson(): JsonObject = buildJsonObject {
        put("key", key.truncateCodePoints(StrLimit.SESSION_KEY))
        put("app", buildJsonObject {
            put("id", appId.truncateCodePoints(StrLimit.APP_ID))
            put("name", appName.truncateCodePoints(StrLimit.APP_NAME))
        })
        put("title", title.truncateCodePoints(StrLimit.TEXT))
        put("artist", artist.truncateCodePoints(StrLimit.TEXT))
        put("album", album.truncateCodePoints(StrLimit.TEXT))
        put("albumArtist", albumArtist.truncateCodePoints(StrLimit.TEXT))
        put("trackId", trackId)
        if (durationMs != null) put("durationMs", durationMs) else put("durationMs", JsonNull)
        if (positionMs != null) put("positionMs", positionMs) else put("positionMs", JsonNull)
        put("rate", rate)
        put("status", status)
        put("actions", buildJsonArray { actions.forEach { add(JsonPrimitive(it)) } })
        if (artwork != null) put("artwork", artwork.toJson()) else put("artwork", JsonNull)
        put("kind", kind)
        if (volume != null) put("volume", volume.toJson())
        if (outputLatencyMs != null) put("outputLatencyMs", outputLatencyMs.coerceIn(0, MAX_OUTPUT_LATENCY_MS))
    }

    companion object {
        const val MAX_OUTPUT_LATENCY_MS = 2_000

        /** Optional field: absent, null, non-integer or out-of-range values are ignored. */
        fun parseOutputLatency(o: JsonObject): Int? = try {
            o.optLong("outputLatencyMs")?.takeIf { it in 0..MAX_OUTPUT_LATENCY_MS.toLong() }?.toInt()
        } catch (e: BadMessageException) {
            null
        }

        fun parse(o: JsonObject): MediaSessionInfo {
            val app = o.optObject("app")
            val duration = o.optLong("durationMs")?.takeIf { it >= 0 }
            val position = o.optLong("positionMs")?.takeIf { it >= 0 }
            val trackId = o.optString("trackId", 64) ?: ""
            val status = o.optString("status", 32)?.takeIf { it in PlayStatus.ALL } ?: PlayStatus.UNKNOWN
            val kind = o.optString("kind", 32)?.takeIf { it in MediaKind.ALL } ?: MediaKind.OTHER
            // Unknown action strings are ignored for forward compatibility.
            val actions = (o.optStringList("actions", 32, 32) ?: emptyList()).filter { it in Action.ALL }.distinct()
            val rate = o.optDouble("rate") ?: 1.0
            return MediaSessionInfo(
                key = o.requireString("key", StrLimit.SESSION_KEY),
                appId = app?.optString("id", StrLimit.APP_ID) ?: "",
                appName = app?.optString("name", StrLimit.APP_NAME) ?: "",
                title = o.optString("title", StrLimit.TEXT) ?: "",
                artist = o.optString("artist", StrLimit.TEXT) ?: "",
                album = o.optString("album", StrLimit.TEXT) ?: "",
                albumArtist = o.optString("albumArtist", StrLimit.TEXT) ?: "",
                trackId = trackId,
                durationMs = duration,
                positionMs = position,
                rate = if (rate.isFinite() && rate >= 0) rate else 1.0,
                status = status,
                actions = actions,
                artwork = o.optObject("artwork")?.let { ArtworkRef.parse(it) },
                kind = kind,
                volume = AppVolume.parseLenient(o["volume"]),
                outputLatencyMs = parseOutputLatency(o),
            )
        }
    }
}

data class MediaSnapshot(val gen: Long, val active: String?, val sessions: List<MediaSessionInfo>) {
    val activeSession: MediaSessionInfo?
        get() = sessions.firstOrNull { it.key == active } ?: sessions.firstOrNull()

    fun toJson(): JsonObject = buildJsonObject {
        put("gen", gen)
        if (active != null) put("active", active) else put("active", JsonNull)
        put("sessions", JsonArray(sessions.take(MAX_SESSIONS).map { it.toJson() }))
    }

    companion object {
        const val MAX_SESSIONS = 16

        fun parse(o: JsonObject): MediaSnapshot {
            val gen = o.requireLong("gen")
            val array = o.optArray("sessions") ?: JsonArray(emptyList())
            if (array.size > MAX_SESSIONS) throw BadMessageException("too many sessions")
            val sessions = array.map {
                MediaSessionInfo.parse(it as? JsonObject ?: throw BadMessageException("session must be an object"))
            }
            return MediaSnapshot(gen, o.optString("active", StrLimit.SESSION_KEY), sessions)
        }
    }
}

data class MediaCommand(
    val key: String?,
    val action: String,
    val positionMs: Long?,
    val origin: String,
    val reason: String?,
) {
    fun toJson(): JsonObject = buildJsonObject {
        if (key != null) put("key", key) else put("key", JsonNull)
        put("action", action)
        if (positionMs != null) put("positionMs", positionMs)
        put("origin", origin)
        if (reason != null) put("reason", reason.truncateCodePoints(StrLimit.MISC))
    }

    companion object {
        const val ORIGIN_USER = "user"
        const val ORIGIN_POLICY = "policy"

        fun parse(o: JsonObject): MediaCommand {
            val action = o.requireString("action", 32)
            val position = o.optLong("positionMs")
            if (action == Action.SEEK && (position == null || position < 0)) {
                throw BadMessageException("seek requires positionMs")
            }
            val origin = o.optString("origin", 32) ?: ORIGIN_USER
            return MediaCommand(
                key = o.optString("key", StrLimit.SESSION_KEY),
                action = action,
                positionMs = position,
                origin = if (origin == ORIGIN_POLICY) ORIGIN_POLICY else ORIGIN_USER,
                reason = o.optString("reason", StrLimit.MISC),
            )
        }
    }
}

data class ErrorInfo(val code: String, val message: String? = null) {
    fun toJson(): JsonObject = buildJsonObject {
        put("code", code)
        if (message != null) put("message", message.truncateCodePoints(StrLimit.ERROR_MESSAGE))
    }

    companion object {
        fun parse(o: JsonObject) = ErrorInfo(
            o.requireString("code", 64),
            o.optString("message", StrLimit.ERROR_MESSAGE),
        )
    }
}

data class CommandResult(val ok: Boolean, val error: ErrorInfo? = null) {
    fun toJson(): JsonObject = buildJsonObject {
        put("ok", ok)
        if (error != null) put("error", error.toJson())
    }

    companion object {
        val OK = CommandResult(true)
        fun failure(code: String, message: String? = null) = CommandResult(false, ErrorInfo(code, message))
        fun parse(o: JsonObject) = CommandResult(o.requireBool("ok"), o.optObject("error")?.let { ErrorInfo.parse(it) })
    }
}

data class PolicyAction(val what: String, val reason: String, val app: String, val at: Long?)

data class PolicyStatus(
    val mode: String,
    val enabled: Boolean,
    val remote: String,
    val local: String,
    val lastAction: PolicyAction?,
) {
    companion object {
        val MODES = listOf("off", "windows_first", "android_first", "latest", "custom")

        fun parse(o: JsonObject): PolicyStatus {
            val last = o.optObject("lastAction")?.let {
                PolicyAction(
                    what = it.optString("what", StrLimit.MISC) ?: "",
                    reason = it.optString("reason", StrLimit.MISC) ?: "",
                    app = it.optString("app", StrLimit.APP_ID) ?: "",
                    at = it.optLong("at"),
                )
            }
            return PolicyStatus(
                mode = o.requireString("mode", 32),
                enabled = o.optBool("enabled") ?: true,
                remote = o.optString("remote", StrLimit.MISC) ?: "",
                local = o.optString("local", StrLimit.MISC) ?: "",
                lastAction = last,
            )
        }
    }
}

data class AudioOffer(
    val streamId: String,
    val direction: String,
    val codec: String,
    val sampleRate: Int,
    val channels: Int,
    val frameMs: Int,
    val transport: String,
) {
    companion object {
        fun parse(o: JsonObject): AudioOffer {
            val streamId = o.requireString("streamId")
            if (!Hex.isLowerHex(streamId, 32)) throw BadMessageException("streamId must be 32 hex chars")
            val rate = o.requireLong("sampleRate")
            val channels = o.requireLong("channels")
            val frameMs = o.optLong("frameMs") ?: 10
            if (rate !in 8000..192000 || channels !in 1..8 || frameMs !in 1..1000) {
                throw BadMessageException("audio parameters out of range")
            }
            return AudioOffer(
                streamId = streamId,
                direction = o.requireString("direction", 64),
                codec = o.requireString("codec", 64),
                sampleRate = rate.toInt(),
                channels = channels.toInt(),
                frameMs = frameMs.toInt(),
                transport = o.optString("transport", 32) ?: "tcp",
            )
        }
    }
}

/** `lyrics.push`; [translation] is an optional LRC whose timestamps match [text]'s lines. */
data class LyricsPush(val trackId: String, val format: String, val source: String, val text: String, val translation: String? = null) {
    companion object {
        fun parse(o: JsonObject): LyricsPush {
            val text = o.requireString("text")
            if (text.toByteArray(Charsets.UTF_8).size > Limits.MAX_LYRICS_TEXT) throw BadMessageException("lyrics too large")
            val translation = o.optString("translation")
            if (translation != null && translation.toByteArray(Charsets.UTF_8).size > Limits.MAX_LYRICS_TEXT) {
                throw BadMessageException("translation too large")
            }
            return LyricsPush(
                trackId = o.requireString("trackId", 64),
                format = o.optString("format", 32) ?: "lrc",
                source = o.optString("source", StrLimit.MISC) ?: "",
                text = text,
                translation = translation?.takeIf { it.isNotBlank() },
            )
        }
    }
}

data class LanEndpoint(val host: String, val port: Int)

/** Media output volume of a device (§8.2 `volume.state`). [level] is 0..1. */
data class VolumeState(val level: Double, val muted: Boolean, val steps: Int? = null) {
    fun toJson(): JsonObject = buildJsonObject {
        put("level", level)
        put("muted", muted)
        if (steps != null) put("steps", steps)
    }

    companion object {
        fun parse(o: JsonObject): VolumeState {
            val level = o.optDouble("level") ?: throw BadMessageException("missing level")
            if (level < 0.0 || level > 1.0) throw BadMessageException("level out of range")
            val steps = o.optLong("steps")
            if (steps != null && (steps < 1 || steps > 10_000)) throw BadMessageException("steps out of range")
            return VolumeState(level, o.optBool("muted") ?: false, steps?.toInt())
        }
    }
}

/** Per-app volume of a media session (§8.3 optional `volume`). */
data class AppVolume(val level: Double, val muted: Boolean) {
    fun toJson(): JsonObject = buildJsonObject {
        put("level", level)
        put("muted", muted)
    }

    companion object {
        /** Optional field: absent, null or malformed values are ignored (forward compatibility). */
        fun parseLenient(e: kotlinx.serialization.json.JsonElement?): AppVolume? {
            val o = e as? JsonObject ?: return null
            return try {
                val level = o.optDouble("level") ?: return null
                if (level < 0.0 || level > 1.0) return null
                AppVolume(level, o.optBool("muted") ?: false)
            } catch (ex: BadMessageException) {
                null
            }
        }
    }
}

/**
 * Requested volume change (§8.2 `volume.set`): any combination of level, muted and delta (steps).
 * With [key], only that media session's app volume changes (Windows per-app mixer).
 */
data class VolumeSet(val level: Double? = null, val muted: Boolean? = null, val delta: Int? = null, val key: String? = null) {
    fun toJson(): JsonObject = buildJsonObject {
        if (level != null) put("level", level)
        if (muted != null) put("muted", muted)
        if (delta != null) put("delta", delta)
        if (key != null) put("key", key)
    }

    companion object {
        fun parse(o: JsonObject): VolumeSet {
            val level = o.optDouble("level")
            if (level != null && (level < 0.0 || level > 1.0)) throw BadMessageException("level out of range")
            val delta = o.optLong("delta")
            if (delta != null && (delta < -100 || delta > 100)) throw BadMessageException("delta out of range")
            val set = VolumeSet(level, o.optBool("muted"), delta?.toInt(), o.optString("key", StrLimit.SESSION_KEY))
            if (set.level == null && set.muted == null && set.delta == null) throw BadMessageException("empty volume.set")
            return set
        }
    }
}
