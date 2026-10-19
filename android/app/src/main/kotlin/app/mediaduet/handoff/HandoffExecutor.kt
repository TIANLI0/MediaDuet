package app.mediaduet.handoff

import android.app.ActivityManager
import android.app.SearchManager
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import androidx.core.net.toUri
import android.os.Bundle
import android.provider.MediaStore
import android.os.SystemClock
import app.mediaduet.lyrics.OnlineLyrics
import app.mediaduet.lyrics.SongMatch
import app.mediaduet.media.LocalSessionState
import app.mediaduet.media.MediaSessionMonitor
import app.mediaduet.protocol.Action
import app.mediaduet.protocol.MediaCommand
import app.mediaduet.service.CompanionHelper
import app.mediaduet.service.Notifications
import app.mediaduet.util.MdLog
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.withContext

/**
 * Plays a song handed over from the PC (`handoff.play`, spec §8.7) in a music app on this phone,
 * and reports once the app's media session plays it:
 *  * the song is resolved to the app's catalogue id (from the PC's `ids`, else a lookup by title /
 *    artist / album / duration, the same rules as the lyrics);
 *  * Spotify: the session's playFromUri / playFromSearch when it supports them (no activity);
 *  * otherwise the app's deep link (NetEase `orpheus://song/<id>`, QQ Music `playSonglist`). While
 *    MediaDuet is in the background Android only lets it open another app with a companion-device
 *    association; without one (or when the launch is silently blocked) it posts a notification
 *    to tap instead and tells the PC (`handoff.progress tap_to_continue`).
 */
class HandoffExecutor(
    private val context: Context,
    private val media: MediaSessionMonitor,
    private val sessions: StateFlow<List<LocalSessionState>>,
) {
    /** Music apps installed on this phone that can take a song, in preference order. */
    fun installedApps(): List<HandoffApp> = HandoffApp.entries.filter { installed(it) }

    private fun installed(app: HandoffApp): Boolean = try {
        context.packageManager.getPackageInfo(app.packageName, 0)
        true
    } catch (e: PackageManager.NameNotFoundException) {
        false
    }

    private sealed interface Plan {
        /** Open [uri] (a deep link) or [intent]; [exact]: the song itself, not a search. */
        data class Open(val uri: Uri?, val intent: Intent, val exact: Boolean) : Plan
        data class Fail(val code: String) : Plan
    }

    suspend fun play(req: HandoffPlay, progress: suspend (String) -> Unit): PlayResult {
        if (!installed(req.app)) return PlayResult.failed(HandoffErrors.APP_UNAVAILABLE)
        val plan = withContext(Dispatchers.IO) { resolve(req) }
        if (plan is Plan.Fail) return PlayResult.failed(plan.code)
        plan as Plan.Open
        val pkg = req.app.packageName
        val playingBefore = sessions.value.any { it.packageName == pkg && it.isActivelyPlaying }

        // 1. No activity needed: the app's own session (Spotify).
        var via = withContext(Dispatchers.Main) {
            val extras = searchExtras(req.song)
            if (media.playFrom(pkg, plan.uri, if (plan.exact) null else SongMatch.searchQuery(req.song.title, req.song.artist), extras)) VIA_SESSION else null
        }
        // 2. Open the app on the song.
        if (via == null) {
            via = if (mayOpenActivities() && startActivity(plan.intent)) VIA_ACTIVITY else null
        }
        if (via == null) {
            if (!notify(plan.intent, req)) return PlayResult.failed(HandoffErrors.LAUNCH_FAILED)
            via = VIA_NOTIFICATION
            progress(PROGRESS_TAP)
        }
        MdLog.i(TAG, "handoff: ${req.app.id} via $via (${if (plan.exact) "song" else "search"})")

        val started = SystemClock.elapsedRealtime()
        var deadline = started + if (via == VIA_NOTIFICATION) Notifications.HANDOFF_NOTICE_MS else WAIT_MS
        var nudged = false
        try {
            while (SystemClock.elapsedRealtime() < deadline) {
                val now = SystemClock.elapsedRealtime()
                val target = Handoff.findTarget(sessions.value, req.app, req.song)
                if (target != null && Handoff.isPlaying(target)) return PlayResult.playing(target.key)
                if (target != null && !nudged && now - started >= NUDGE_MS) {
                    // The app shows the song but did not start it (it was loaded, paused).
                    nudged = true
                    MdLog.i(TAG, "handoff: ${target.key} shows the song but is paused: play")
                    withContext(Dispatchers.Main) { media.execute(MediaCommand(target.key, Action.PLAY, null, MediaCommand.ORIGIN_USER, "handoff")) }
                }
                val reacted = sessions.value.any { it.packageName == pkg && it.lastChangeMs >= started }
                if (via == VIA_ACTIVITY && target == null && !reacted && now - started >= BLOCKED_MS && !inForeground()) {
                    // Android may block background launches silently: fall back to a tap.
                    if (notify(plan.intent, req)) {
                        MdLog.i(TAG, "handoff: no reaction to the launch, asking for a tap")
                        via = VIA_NOTIFICATION
                        deadline = now + Notifications.HANDOFF_NOTICE_MS
                        progress(PROGRESS_TAP)
                    }
                }
                delay(POLL_MS)
            }
        } finally {
            if (via == VIA_NOTIFICATION) Notifications.cancel(context, Notifications.ID_HANDOFF)
        }
        val other = sessions.value.firstOrNull { it.packageName == pkg && it.isActivelyPlaying }
        if (!plan.exact && other != null && !playingBefore) {
            // A search started something else: undo it, the song is not there.
            MdLog.i(TAG, "handoff: the search played \"${other.title}\" instead; pausing it")
            withContext(Dispatchers.Main) { media.execute(MediaCommand(other.key, Action.PAUSE, null, MediaCommand.ORIGIN_USER, "handoff")) }
            return PlayResult.failed(HandoffErrors.NOT_FOUND)
        }
        return if (via == VIA_NOTIFICATION) PlayResult.failed(HandoffErrors.TIMEOUT) else PlayResult.OPENED
    }

    private fun resolve(req: HandoffPlay): Plan {
        val want = req.song.candidate
        return when (req.app) {
            HandoffApp.NETEASE -> {
                val id = req.ids?.netease ?: OnlineLyrics.findNetease(want).let { f ->
                    if (!f.answered) return Plan.Fail(HandoffErrors.NETWORK)
                    f.hit?.toString() ?: return Plan.Fail(HandoffErrors.NOT_FOUND)
                }
                val uri = Handoff.neteaseUri(id).toUri()
                Plan.Open(uri, view(uri, req.app), exact = true)
            }
            HandoffApp.QQMUSIC -> {
                val mid = req.ids?.qqmusic ?: OnlineLyrics.findQq(want).let { f ->
                    if (!f.answered) return Plan.Fail(HandoffErrors.NETWORK)
                    f.hit?.mid ?: return Plan.Fail(HandoffErrors.NOT_FOUND)
                }
                val uri = Handoff.qqMusicUri(mid).toUri()
                Plan.Open(uri, view(uri, req.app), exact = true)
            }
            HandoffApp.SPOTIFY -> {
                // No anonymous Spotify catalogue: the track itself only with its id.
                val id = req.ids?.spotify
                if (id != null) {
                    val uri = Handoff.spotifyTrackUri(id).toUri()
                    Plan.Open(uri, view(uri, req.app), exact = true)
                } else {
                    val intent = Intent(MediaStore.INTENT_ACTION_MEDIA_PLAY_FROM_SEARCH)
                        .setPackage(req.app.packageName)
                        .putExtras(searchExtras(req.song))
                        .putExtra(SearchManager.QUERY, SongMatch.searchQuery(req.song.title, req.song.artist))
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    Plan.Open(null, intent, exact = false)
                }
            }
        }
    }

    private fun view(uri: Uri, app: HandoffApp) =
        Intent(Intent.ACTION_VIEW, uri).setPackage(app.packageName).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            .putExtra(Intent.EXTRA_REFERRER, "android-app://${context.packageName}".toUri())

    private fun searchExtras(song: HandoffSong) = Bundle().apply {
        putString(MediaStore.EXTRA_MEDIA_FOCUS, MediaStore.Audio.Media.ENTRY_CONTENT_TYPE)
        putString(MediaStore.EXTRA_MEDIA_TITLE, song.title)
        if (song.artist.isNotEmpty()) putString(MediaStore.EXTRA_MEDIA_ARTIST, song.artist)
        if (song.album.isNotEmpty()) putString(MediaStore.EXTRA_MEDIA_ALBUM, song.album)
    }

    /** Android lets a background app open activities with a companion-device association. */
    private fun mayOpenActivities(): Boolean =
        inForeground() || runCatching { CompanionHelper(context).associations().isNotEmpty() }.getOrDefault(false)

    private fun inForeground(): Boolean {
        val info = ActivityManager.RunningAppProcessInfo()
        ActivityManager.getMyMemoryState(info)
        return info.importance <= ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND
    }

    private fun startActivity(intent: Intent): Boolean = try {
        context.startActivity(intent)
        true
    } catch (e: ActivityNotFoundException) {
        MdLog.w(TAG, "handoff: no activity for ${intent.data ?: intent.action}")
        false
    } catch (e: SecurityException) {
        MdLog.w(TAG, "handoff: launch refused: ${e.message}")
        false
    }

    private fun notify(intent: Intent, req: HandoffPlay): Boolean {
        val label = runCatching {
            val pm = context.packageManager
            pm.getApplicationLabel(pm.getApplicationInfo(req.app.packageName, 0)).toString()
        }.getOrDefault(req.app.id)
        return Notifications.showHandoff(context, intent, req.song.title, label)
    }

    companion object {
        private const val TAG = "MD/Handoff"
        const val PROGRESS_TAP = "tap_to_continue"
        private const val VIA_SESSION = "session"
        private const val VIA_ACTIVITY = "activity"
        private const val VIA_NOTIFICATION = "notification"
        /** How long the opened app may take to play the song (cold start, buffering). */
        private const val WAIT_MS = 20_000L
        /** A background launch without any reaction by then was probably blocked. */
        private const val BLOCKED_MS = 6_000L
        private const val NUDGE_MS = 2_500L
        private const val POLL_MS = 250L
    }
}
