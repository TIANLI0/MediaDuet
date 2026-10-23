package app.mediaduet.lyrics

import android.app.Notification
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Build
import android.os.SystemClock
import app.mediaduet.R
import app.mediaduet.media.LocalSessionState
import app.mediaduet.media.ResolvedTarget
import app.mediaduet.media.Side
import app.mediaduet.media.ArtworkProcessor
import app.mediaduet.protocol.MediaSessionInfo
import app.mediaduet.protocol.PlayStatus
import app.mediaduet.remote.LyricRow
import app.mediaduet.remote.LyricRows
import app.mediaduet.remote.LyricsDelay
import app.mediaduet.remote.LyricsState
import app.mediaduet.remote.Lyrics
import app.mediaduet.remote.RemoteMediaStore
import app.mediaduet.remote.RemotePosition
import app.mediaduet.service.AppSettings
import app.mediaduet.service.Engine
import app.mediaduet.service.Notifications
import app.mediaduet.util.MdLog
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch

/**
 * Optional (default off): the current lyric line as its own ongoing notification, shown in the
 * notification shade and on the lock screen. On Android 16+ it asks to be promoted to a Live
 * Update, which ColorOS 16 shows as a Fluid Cloud capsule (and AOSP as a status bar chip).
 * Media notifications cannot be promoted, hence a notification of its own next to the player's.
 *
 * Follows the engine's player target (phone or PC) with the same lyrics delay as lyrics mode.
 */
class LyricsNotifier(
    private val context: Context,
    private val scope: CoroutineScope,
    private val settings: AppSettings,
    private val target: StateFlow<ResolvedTarget?>,
    private val remote: RemoteMediaStore,
    private val local: StateFlow<List<LocalSessionState>>,
    private val lyrics: LyricsRepository,
    private val artwork: ArtworkProcessor,
    private val localLatencyMs: StateFlow<Int>,
) {
    /** What is playing, with everything needed to pick its current line. */
    private class Current(
        val side: Side,
        val info: MediaSessionInfo,
        val lyrics: Lyrics,
        val rows: List<LyricRow>,
        val endMs: Long,
        val playing: Boolean,
        val positionAt: (Long) -> Long?,
        val outputLatencyMs: Int,
        val artHash: String?,
    )

    private class Timing(val autoLatency: Boolean, val offsets: Map<String, Int>, val translations: Boolean, val localLatencyMs: Int)

    /**
     * The song whose notification the user swiped away: not shown again until the next song (by
     * title + artist, since players re-report a song under a changing track id).
     */
    private val dismissedSong = MutableStateFlow<String?>(null)
    private var posted: LyricsFrame? = null
    private var postedSong: String? = null
    private var lastPostMs = Long.MIN_VALUE / 2
    private var art: Pair<String, Bitmap?>? = null
    /** Rows of the last lyrics (rebuilt only when the lyrics change, not with every position update). */
    private var rowsCache: Pair<Lyrics, List<LyricRow>>? = null

    private fun rowsOf(l: Lyrics): List<LyricRow> {
        rowsCache?.let { (k, v) -> if (k === l) return v }
        return LyricRows.build(l).also { rowsCache = l to it }
    }

    fun start() {
        val current = combine(settings.lyricsNotification, target, remote.state, local, lyrics.entries) { on, t, rem, loc, _ ->
            if (!on || t == null) return@combine null
            when (t.side) {
                Side.PC -> {
                    val st = rem?.takeIf { it.connected } ?: return@combine null
                    val s = st.snapshot.sessions.firstOrNull { it.key == t.key } ?: return@combine null
                    val l = lyrics.resolve(s)?.syncedOrNull() ?: return@combine null
                    val rows = rowsOf(l)
                    Current(Side.PC, s, l, rows, LyricRows.endMs(rows), s.playingOrBuffering, { now -> RemotePosition.at(s, st.positionAtMs, now) }, s.outputLatencyMs ?: 0, s.artwork?.hash)
                }
                Side.PHONE -> {
                    val s = loc.firstOrNull { it.key == t.key } ?: return@combine null
                    val info = s.toSessionInfo(SystemClock.elapsedRealtime())
                    val l = lyrics.resolve(info)?.syncedOrNull() ?: return@combine null
                    val rows = rowsOf(l)
                    Current(Side.PHONE, info, l, rows, LyricRows.endMs(rows), s.isActivelyPlaying, s::estimatedPosition, -1, s.artwork?.hash)
                }
            }
        }
        val timing = combine(settings.lyricsAutoLatency, settings.lyricOffsets, settings.showTranslations, localLatencyMs, ::Timing)
        scope.launch {
            combine(current, timing, dismissedSong) { c, t, d -> Triple(c, t, d) }.collectLatest { (cur, t, dismissed) ->
                if (cur == null || cur.info.songKey == dismissed) {
                    cancel()
                    return@collectLatest
                }
                if (!cur.playing) {
                    // Paused: keep the line through short stops (skips, buffering), then go away.
                    delay(PAUSE_GRACE_MS)
                    cancel()
                    return@collectLatest
                }
                val latency = if (cur.outputLatencyMs >= 0) cur.outputLatencyMs else t.localLatencyMs
                val delayMs = LyricsDelay.totalMs(t.autoLatency, latency, t.offsets[cur.info.appId] ?: 0)
                val rate = cur.info.rate.takeIf { it > 0 && it.isFinite() } ?: 1.0
                while (true) {
                    val sinceLast = SystemClock.elapsedRealtime() - lastPostMs
                    if (sinceLast < MIN_POST_INTERVAL_MS) delay(MIN_POST_INTERVAL_MS - sinceLast)
                    val now = SystemClock.elapsedRealtime()
                    val pos = LyricsDelay.displayPosition(cur.positionAt(now) ?: 0L, delayMs)
                    val frame = LyricsFrame.at(cur.rows, cur.endMs, pos, t.translations)
                    post(cur, frame)
                    val next = frame.nextMs ?: awaitCancellation()
                    delay((((next - pos) / rate).toLong() + 1).coerceIn(1L, MAX_WAIT_MS))
                }
            }
        }
    }

    /** The user swiped the notification away (its delete intent). */
    fun onDismissed() {
        val song = postedSong ?: return
        MdLog.i(TAG, "lyrics notification dismissed")
        posted = null
        dismissedSong.value = song
    }

    private fun post(cur: Current, frame: LyricsFrame) {
        // Re-post an unchanged line now and then only to renew the timeout.
        if (frame == posted && cur.info.songKey == postedSong && SystemClock.elapsedRealtime() - lastPostMs < REFRESH_MS) return
        if (!Notifications.canPost(context)) return
        val info = cur.info
        val title = info.title.ifEmpty { context.getString(R.string.unknown_title) }
        val song = listOf(title, info.artist).filter { it.isNotEmpty() }.joinToString(context.getString(R.string.list_separator))
        val line = frame.text
        val b = Notification.Builder(context, Notifications.CHANNEL_LYRICS)
            .setSmallIcon(R.drawable.ic_mc_music)
            .setContentTitle(line ?: title)
            .setContentText(if (line == null) info.artist.ifEmpty { null } else frame.translation ?: song)
            .setSubText(if (cur.side == Side.PC) context.getString(R.string.mirror_subtext) else info.appName.ifEmpty { null })
            .setLargeIcon(artwork(cur))
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .setTimeoutAfter(TIMEOUT_MS)
            .setContentIntent(Notifications.openAppIntent(context, "now"))
            .setDeleteIntent(
                PendingIntent.getBroadcast(
                    context, Notifications.ID_LYRICS,
                    Intent(context, LyricsDismissReceiver::class.java),
                    PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
                ),
            )
        if (frame.translation != null) b.setStyle(Notification.BigTextStyle().bigText(frame.translation))
        if (Build.VERSION.SDK_INT >= 36) {
            b.setRequestPromotedOngoing(true)
            b.setShortCriticalText(line ?: NOTE)
        }
        try {
            context.getSystemService(NotificationManager::class.java).notify(Notifications.ID_LYRICS, b.build())
            posted = frame
            postedSong = info.songKey
            lastPostMs = SystemClock.elapsedRealtime()
        } catch (e: SecurityException) {
            MdLog.w(TAG, "cannot post lyrics notification")
        }
    }

    /** The cover as a small large-icon bitmap, decoded and scaled once per artwork. */
    private fun artwork(cur: Current): Bitmap? {
        val hash = cur.artHash ?: return null
        art?.let { (h, bmp) -> if (h == hash) return bmp }
        val full = when (cur.side) {
            Side.PC -> remote.artwork.value[hash]
            Side.PHONE -> artwork.bytes(hash)?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }
        }
        // PC artwork may not have arrived yet: try again with the next line.
        if (full == null) return null
        val edge = context.resources.getDimensionPixelSize(android.R.dimen.notification_large_icon_width).coerceAtLeast(64)
        val scaled = if (full.width <= edge && full.height <= edge) full else Bitmap.createScaledBitmap(full, edge, edge * full.height / full.width.coerceAtLeast(1), true)
        art = hash to scaled
        return scaled
    }

    private fun cancel() {
        if (posted == null && postedSong == null) return
        posted = null
        postedSong = null
        Notifications.cancel(context, Notifications.ID_LYRICS)
    }

    companion object {
        private const val TAG = "MD/LyricsNote"
        private const val NOTE = "♪"
        /** Android drops updates beyond a few per second per app; lyric lines are rarely faster. */
        private const val MIN_POST_INTERVAL_MS = 350L
        private const val MAX_WAIT_MS = 60_000L
        private const val PAUSE_GRACE_MS = 3_000L
        /** A process killed mid-song must not leave a stale line behind for long. */
        private const val TIMEOUT_MS = 10 * 60_000L
        private const val REFRESH_MS = 5 * 60_000L

        private fun LyricsState.syncedOrNull(): Lyrics? = lyrics?.takeIf { !it.isEmpty }
    }
}

/**
 * The lyric line shown at one position (the shared rows, [LyricRows]): [text] null before the
 * lyrics, in instrumental gaps and after their end (the song title is shown instead); [nextMs] =
 * position of the next change, null when nothing changes any more.
 */
data class LyricsFrame(val text: String?, val translation: String?, val nextMs: Long?) {
    companion object {
        fun at(rows: List<LyricRow>, endMs: Long, positionMs: Long, translations: Boolean): LyricsFrame {
            val row = LyricRows.lineAt(rows, endMs, positionMs)
            val text = row?.line?.text?.ifEmpty { null }
            val tr = if (translations && text != null) row.translation?.trim()?.ifEmpty { null } else null
            return LyricsFrame(text, tr, LyricRows.nextChange(rows, endMs, positionMs))
        }
    }
}

/** The lyrics notification's delete intent: the user swiped it away. */
class LyricsDismissReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        Engine.get(context).lyricsNotifier.onDismissed()
    }
}

private val MediaSessionInfo.playingOrBuffering: Boolean
    get() = status == PlayStatus.PLAYING || status == PlayStatus.BUFFERING

private val MediaSessionInfo.songKey: String
    get() = "$title\n$artist"
