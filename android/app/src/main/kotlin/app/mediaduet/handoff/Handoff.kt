package app.mediaduet.handoff

import app.mediaduet.lyrics.SongMatch
import app.mediaduet.media.LocalSessionState
import app.mediaduet.protocol.CatalogIds
import app.mediaduet.protocol.PlayStatus
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put
import java.net.URLEncoder
import kotlin.math.abs

/**
 * Handoff ("continue on the other device", spec §8.7), the pure parts: the music apps, their deep
 * links, and when a session shows the song that was handed over. Mirrors the host's
 * `windows/core/include/md/handoff/Handoff.h`.
 */
enum class HandoffApp(val id: String, val packageName: String) {
    NETEASE("netease", "com.netease.cloudmusic"),
    QQMUSIC("qqmusic", "com.tencent.qqmusic"),
    SPOTIFY("spotify", "com.spotify.music");

    companion object {
        fun of(id: String?): HandoffApp? = entries.firstOrNull { it.id == id }

        /** The handoff app of a player (Android package or Windows app id), null for any other. */
        fun ofSession(appId: String): HandoffApp? {
            val id = appId.lowercase()
            return when {
                "cloudmusic" in id -> NETEASE
                "qqmusic" in id -> QQMUSIC
                "spotify" in id -> SPOTIFY
                else -> null
            }
        }

        fun toJson(apps: List<HandoffApp>) = JsonArray(apps.map { JsonPrimitive(it.id) })

        /** Lenient: unknown ids skipped, duplicates removed, order kept. */
        fun listFrom(e: kotlinx.serialization.json.JsonElement?): List<HandoffApp> =
            (e as? JsonArray).orEmpty().mapNotNull { (it as? JsonPrimitive)?.contentOrNull?.let(::of) }.distinct()
    }
}

data class HandoffSong(val title: String, val artist: String, val album: String, val durationMs: Long?) {
    fun toJson(): JsonObject = buildJsonObject {
        put("title", title)
        put("artist", artist)
        put("album", album)
        if (durationMs != null) put("durationMs", durationMs) else put("durationMs", JsonNull)
    }

    val candidate: SongMatch.Candidate get() = SongMatch.Candidate(title, artist, album, durationMs)

    companion object {
        fun parse(e: kotlinx.serialization.json.JsonElement?): HandoffSong? {
            val o = e as? JsonObject ?: return null
            fun str(k: String) = ((o[k] as? JsonPrimitive)?.takeIf { it.isString }?.content ?: "").take(512)
            val title = str("title").takeIf { it.isNotBlank() } ?: return null
            val d = (o["durationMs"] as? JsonPrimitive)?.longOrNull?.takeIf { it in 1 until 1_000_000_000 }
            return HandoffSong(title, str("artist"), str("album"), d)
        }
    }
}

/** `handoff.play` from the PC: play [song] in [app] on this phone. */
data class HandoffPlay(val app: HandoffApp, val song: HandoffSong, val ids: CatalogIds?) {
    companion object {
        fun parse(o: JsonObject): HandoffPlay? {
            val app = HandoffApp.of((o["app"] as? JsonPrimitive)?.contentOrNull) ?: return null
            val song = HandoffSong.parse(o["song"]) ?: return null
            return HandoffPlay(app, song, CatalogIds.parseLenient(o["ids"]))
        }
    }
}

/** The answer to `handoff.play` ([STATE_PLAYING]: [key] plays the song; [STATE_OPENED]: shown, not confirmed). */
data class PlayResult(val ok: Boolean, val state: String? = null, val key: String? = null, val error: String? = null) {
    fun toJson(): JsonObject = buildJsonObject {
        put("ok", ok)
        if (ok) {
            put("state", state ?: STATE_OPENED)
            if (key != null) put("key", key)
        } else {
            put("error", buildJsonObject { put("code", error ?: "internal") })
        }
    }

    companion object {
        const val STATE_PLAYING = "playing"
        const val STATE_OPENED = "opened"
        fun playing(key: String) = PlayResult(true, STATE_PLAYING, key)
        val OPENED = PlayResult(true, STATE_OPENED)
        fun failed(code: String) = PlayResult(false, error = code)
    }
}

/** The PC's answer to `handoff.start` (it coordinates both directions). */
data class HandoffOutcome(val ok: Boolean, val state: String?, val error: String?, val to: String?, val app: HandoffApp?, val title: String) {
    companion object {
        fun parse(o: JsonObject?): HandoffOutcome {
            if (o == null) return HandoffOutcome(false, null, HandoffErrors.TIMEOUT, null, null, "")
            fun str(k: String) = (o[k] as? JsonPrimitive)?.contentOrNull
            val ok = (o["ok"] as? JsonPrimitive)?.contentOrNull == "true"
            val err = ((o["error"] as? JsonObject)?.get("code") as? JsonPrimitive)?.contentOrNull
            return HandoffOutcome(ok, str("state"), if (ok) null else err ?: "internal", str("to"), HandoffApp.of(str("app")), str("title").orEmpty())
        }
    }
}

object HandoffErrors {
    const val APP_UNAVAILABLE = "app_unavailable"
    const val NOT_FOUND = "not_found"
    const val LAUNCH_FAILED = "launch_failed"
    const val TIMEOUT = "timeout"
    const val NO_SESSION = "no_session"
    const val BUSY = "busy"
    const val LINK_LOST = "link_lost"
    const val UNSUPPORTED = "unsupported"
    const val NETWORK = "network"
}

object Handoff {
    private val DIGITS = Regex("\\d{1,19}")
    private val SONG_MID = Regex("[0-9A-Za-z]{14}")
    private val SPOTIFY_TRACK = Regex("spotify:track:([0-9A-Za-z]{22})")

    /**
     * Catalogue ids from a player's `METADATA_KEY_MEDIA_ID`, only in the forms we can be sure of:
     * NetEase a decimal song id, QQ Music a 14-character songmid, Spotify a track URI.
     */
    fun idsFrom(packageName: String, mediaId: String): CatalogIds? {
        val m = mediaId.trim()
        if (m.isEmpty()) return null
        return when (HandoffApp.ofSession(packageName)) {
            HandoffApp.NETEASE -> m.takeIf { DIGITS.matches(it) }?.let { CatalogIds(netease = it) }
            HandoffApp.QQMUSIC -> m.takeIf { SONG_MID.matches(it) && it.any(Char::isLetter) }?.let { CatalogIds(qqmusic = it) }
            HandoffApp.SPOTIFY -> SPOTIFY_TRACK.find(m)?.groupValues?.get(1)?.let { CatalogIds(spotify = it) }
            null -> null
        }
    }

    /**
     * True when a session shows [want]: equal titles after [SongMatch.normalize] (or one starts
     * with the other, at least two characters), durations within 5 s when both are known. Artists
     * are not compared: catalogues spell them differently.
     */
    fun shows(title: String, durationMs: Long?, want: HandoffSong): Boolean {
        if (durationMs != null && want.durationMs != null && durationMs > 0 && abs(durationMs - want.durationMs) > 5_000) return false
        val a = SongMatch.normalize(title)
        val b = SongMatch.normalize(want.title)
        if (a.isEmpty() || b.isEmpty()) return false
        if (a == b) return true
        val (shorter, longer) = if (a.length < b.length) a to b else b to a
        return shorter.codePointCount(0, shorter.length) >= 2 && longer.startsWith(shorter)
    }

    /** The session of [app] that shows [want], preferring one that plays. */
    fun findTarget(sessions: List<LocalSessionState>, app: HandoffApp, want: HandoffSong): LocalSessionState? =
        sessions.filter { it.packageName == app.packageName && shows(it.title, it.durationMs, want) }
            .maxByOrNull { if (it.isActivelyPlaying) 1 else 0 }

    fun isPlaying(s: LocalSessionState) = s.status == PlayStatus.PLAYING

    // ---- Deep links (checked against the apps: NetEase 9.6, QQ Music 20.9) ---------------------

    /** NetEase Cloud Music: opens the player on the song and plays it (`RedirectActivity`). */
    fun neteaseUri(songId: String) = "orpheus://song/$songId?switch=true"

    /** QQ Music: the app's own "play this song" link (`playSonglist`, action play). */
    fun qqMusicUri(songMid: String): String {
        val p = "{\"song\":[{\"type\":\"0\",\"songid\":\"\",\"songmid\":\"$songMid\"}],\"action\":\"play\"}"
        return "qqmusic://qq.com/media/playSonglist?p=" + enc(p) + "&source=" + enc("https://i.y.qq.com/v8/playsong.html")
    }

    fun spotifyTrackUri(trackId: String) = "spotify:track:$trackId"

    private fun enc(s: String) = URLEncoder.encode(s, "UTF-8").replace("+", "%20")
}
