package app.mediaduet.media

import app.mediaduet.protocol.Action
import app.mediaduet.protocol.ArtworkRef
import app.mediaduet.protocol.CustomAction
import app.mediaduet.protocol.QueueHint
import app.mediaduet.protocol.MediaKind
import app.mediaduet.protocol.MediaSessionInfo
import app.mediaduet.protocol.MediaSnapshot
import app.mediaduet.protocol.PlayStatus
import app.mediaduet.protocol.TrackId
import kotlin.math.abs

/**
 * Values of android.media.session.PlaybackState / AudioAttributes, mirrored here so that the
 * mapping logic stays a pure function that can be unit-tested on the JVM.
 */
object PlaybackConst {
    const val STATE_NONE = 0
    const val STATE_STOPPED = 1
    const val STATE_PAUSED = 2
    const val STATE_PLAYING = 3
    const val STATE_FAST_FORWARDING = 4
    const val STATE_REWINDING = 5
    const val STATE_BUFFERING = 6
    const val STATE_ERROR = 7
    const val STATE_CONNECTING = 8
    const val STATE_SKIPPING_TO_PREVIOUS = 9
    const val STATE_SKIPPING_TO_NEXT = 10
    const val STATE_SKIPPING_TO_QUEUE_ITEM = 11

    const val ACTION_STOP = 1L
    const val ACTION_PAUSE = 2L
    const val ACTION_PLAY = 4L
    const val ACTION_SKIP_TO_PREVIOUS = 16L
    const val ACTION_SKIP_TO_NEXT = 32L
    const val ACTION_SEEK_TO = 256L
    const val ACTION_PLAY_PAUSE = 512L

    const val CONTENT_TYPE_SPEECH = 1
    const val CONTENT_TYPE_MUSIC = 2
    const val CONTENT_TYPE_MOVIE = 3
}

/** Raw state captured from one MediaController (no Android types). */
data class LocalSessionState(
    val key: String,
    val packageName: String,
    val appName: String,
    val title: String,
    val artist: String,
    val album: String,
    val albumArtist: String,
    val durationMs: Long?,
    val hasPlaybackState: Boolean,
    val state: Int,
    val positionMs: Long,
    val lastPositionUpdateMs: Long,
    val speed: Float,
    val actions: Long,
    val contentType: Int,
    val artwork: ArtworkRef?,
    /** Monotonic time of the last metadata/state change (used to pick the active session). */
    val lastChangeMs: Long,
    /** The player's own buttons (PlaybackState.CustomAction). */
    val customActions: List<CustomAction> = emptyList(),
    /** The player's queue, when it exposes one. */
    val queue: QueueHint? = null,
) {
    val status: String
        get() = if (!hasPlaybackState) PlayStatus.UNKNOWN else when (state) {
            PlaybackConst.STATE_PLAYING, PlaybackConst.STATE_FAST_FORWARDING, PlaybackConst.STATE_REWINDING -> PlayStatus.PLAYING
            PlaybackConst.STATE_PAUSED -> PlayStatus.PAUSED
            PlaybackConst.STATE_STOPPED, PlaybackConst.STATE_NONE, PlaybackConst.STATE_ERROR -> PlayStatus.STOPPED
            PlaybackConst.STATE_BUFFERING, PlaybackConst.STATE_CONNECTING,
            PlaybackConst.STATE_SKIPPING_TO_NEXT, PlaybackConst.STATE_SKIPPING_TO_PREVIOUS,
            PlaybackConst.STATE_SKIPPING_TO_QUEUE_ITEM -> PlayStatus.BUFFERING
            else -> PlayStatus.UNKNOWN
        }

    val isActivelyPlaying: Boolean get() = status == PlayStatus.PLAYING || status == PlayStatus.BUFFERING

    fun protocolActions(): List<String> {
        if (!hasPlaybackState) return emptyList()
        val a = actions
        val out = ArrayList<String>(7)
        val play = a and PlaybackConst.ACTION_PLAY != 0L
        val pause = a and PlaybackConst.ACTION_PAUSE != 0L
        if (play) out += Action.PLAY
        if (pause) out += Action.PAUSE
        if (a and PlaybackConst.ACTION_PLAY_PAUSE != 0L || (play && pause)) out += Action.TOGGLE
        if (a and PlaybackConst.ACTION_SKIP_TO_NEXT != 0L) out += Action.NEXT
        if (a and PlaybackConst.ACTION_SKIP_TO_PREVIOUS != 0L) out += Action.PREVIOUS
        if (a and PlaybackConst.ACTION_SEEK_TO != 0L && durationMs != null) out += Action.SEEK
        if (a and PlaybackConst.ACTION_STOP != 0L) out += Action.STOP
        return out
    }

    /** Position estimated at [nowMs] (elapsedRealtime) as documented for PlaybackState. */
    fun estimatedPosition(nowMs: Long): Long? {
        if (!hasPlaybackState || positionMs < 0) return null
        var pos = positionMs
        if (status == PlayStatus.PLAYING && lastPositionUpdateMs > 0 && nowMs > lastPositionUpdateMs) {
            pos += ((nowMs - lastPositionUpdateMs) * speed.toDouble()).toLong()
        }
        if (pos < 0) pos = 0
        durationMs?.let { if (pos > it) pos = it }
        return pos
    }

    fun kind(): String = when (contentType) {
        PlaybackConst.CONTENT_TYPE_MUSIC -> MediaKind.MUSIC
        PlaybackConst.CONTENT_TYPE_MOVIE -> MediaKind.VIDEO
        PlaybackConst.CONTENT_TYPE_SPEECH -> MediaKind.OTHER
        else -> if (artist.isNotEmpty() || album.isNotEmpty()) MediaKind.MUSIC else MediaKind.OTHER
    }

    fun toSessionInfo(nowMs: Long, outputLatencyMs: Int? = null): MediaSessionInfo {
        val rate = speed.toDouble().takeIf { it > 0 && it.isFinite() } ?: 1.0
        return MediaSessionInfo(
            key = key,
            appId = packageName,
            appName = appName,
            title = title,
            artist = artist,
            album = album,
            albumArtist = albumArtist,
            trackId = if (title.isEmpty() && artist.isEmpty()) "" else TrackId.of(title, artist, album, durationMs),
            durationMs = durationMs,
            positionMs = estimatedPosition(nowMs),
            rate = rate,
            status = status,
            actions = protocolActions(),
            artwork = artwork,
            kind = kind(),
            outputLatencyMs = outputLatencyMs,
            customActions = customActions,
            queue = queue,
        )
    }
}

/**
 * Which of a player's custom actions are real controls (like / favourite, shuffle, repeat, …).
 * Some players also publish promotions there — NetEase Cloud Music lists playlists and campaigns
 * such as "中文乐队 | 自驾…" or "2026洛天依纯蓝…": text without a glyph, long titles, separators.
 * Those are dropped at the source, so neither the PC nor this app shows them.
 */
object CustomActionFilter {
    /** Display width: CJK counts 2, everything else 1 ("收藏到歌单" = 10, "Repeat one" = 10). */
    private const val MAX_WIDTH = 12
    private val separators = charArrayOf('|', '｜', '丨', '·', '•', '#', '【', '】', '《', '》')

    fun isControl(title: String?, hasIcon: Boolean): Boolean {
        val t = title.orEmpty().trim()
        if (!hasIcon) return false                       // controls have a glyph; promotions are text
        var width = 0
        var i = 0
        while (i < t.length) {
            val cp = t.codePointAt(i)
            width += if (Character.isIdeographic(cp) || cp in 0x3000..0x30FF || cp in 0xFF00..0xFFEF) 2 else 1
            i += Character.charCount(cp)
        }
        if (width > MAX_WIDTH) return false
        if (t.any { it in separators }) return false
        if (Regex("\\d{4}").containsMatchIn(t)) return false // "2026…" campaigns
        return true
    }
}

object SnapshotBuilder {
    /**
     * Active session: actively playing sessions win; among equals the most recently changed one.
     * [sessions] is in MediaSessionManager priority order, used as the final tie-break.
     */
    fun pickActive(sessions: List<LocalSessionState>): LocalSessionState? =
        sessions.withIndex().maxWithOrNull(
            compareBy<IndexedValue<LocalSessionState>> { it.value.isActivelyPlaying }
                .thenBy { it.value.lastChangeMs }
                .thenBy { -it.index },
        )?.value

    /** [outputLatencyMs]: the phone's current media output latency, applied to every session. */
    fun build(gen: Long, sessions: List<LocalSessionState>, nowMs: Long, outputLatencyMs: Int? = null): MediaSnapshot {
        val capped = sessions.take(MediaSnapshot.MAX_SESSIONS)
        val active = pickActive(capped)
        return MediaSnapshot(gen, active?.key, capped.map { it.toSessionInfo(nowMs, outputLatencyMs) })
    }
}

/**
 * §8.3: suppress snapshots equal to the previous one apart from the naturally advancing
 * position. A position jump beyond [toleranceMs] (a seek) is a meaningful change.
 */
class SnapshotSuppressor(private val toleranceMs: Long = 1_500) {
    private var last: MediaSnapshot? = null
    private var lastAtMs: Long = 0

    fun shouldSend(candidate: MediaSnapshot, nowMs: Long): Boolean {
        val prev = last
        val send = prev == null || changed(prev, candidate, nowMs - lastAtMs)
        if (send) {
            last = candidate
            lastAtMs = nowMs
        }
        return send
    }

    fun reset() {
        last = null
    }

    private fun normalized(s: MediaSnapshot) =
        s.copy(gen = 0, sessions = s.sessions.map { it.copy(positionMs = it.positionMs?.let { 0L }) })

    private fun changed(prev: MediaSnapshot, next: MediaSnapshot, elapsedMs: Long): Boolean {
        if (normalized(prev) != normalized(next)) return true
        for ((i, p) in prev.sessions.withIndex()) {
            val n = next.sessions[i]
            val pp = p.positionMs ?: continue
            val np = n.positionMs ?: continue
            var expected = pp + if (p.isPlaying) (elapsedMs * p.rate).toLong() else 0L
            p.durationMs?.let { if (expected > it) expected = it }
            if (abs(np - expected) > toleranceMs) return true
        }
        return false
    }
}
