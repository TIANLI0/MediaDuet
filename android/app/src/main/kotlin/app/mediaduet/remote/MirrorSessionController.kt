package app.mediaduet.remote

import android.app.Notification
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.drawable.Icon
import android.media.MediaMetadata
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.os.SystemClock
import app.mediaduet.R
import app.mediaduet.protocol.Action
import app.mediaduet.protocol.MediaSessionInfo
import app.mediaduet.protocol.PlayStatus
import app.mediaduet.service.Engine
import app.mediaduet.service.Notifications
import app.mediaduet.util.MdLog
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch

/**
 * Optional (default off): mirrors the Windows session into a local MediaSession + MediaStyle
 * notification so it shows up in the system media controls. This session belongs to our own
 * package and is therefore excluded from the snapshots we send (no echo loops).
 */
class MirrorSessionController(
    private val context: Context,
    private val scope: CoroutineScope,
    private val remote: RemoteMediaStore,
    private val enabled: StateFlow<Boolean>,
) {
    private var session: MediaSession? = null
    private var lastKey: String? = null

    fun start() {
        scope.launch(Dispatchers.Main) {
            combine(enabled, remote.state, remote.selectedKey, remote.artwork) { on, st, sel, art ->
                if (!on || st == null || !st.connected) null else st.session(sel)?.let { it to art[it.artwork?.hash] }
            }.collect { target ->
                if (target == null) release() else show(target.first, target.second)
            }
        }
    }

    private fun ensureSession(): MediaSession {
        session?.let { return it }
        val s = MediaSession(context, "MediaDuet-mirror")
        s.setCallback(object : MediaSession.Callback() {
            override fun onPlay() = send(Action.PLAY)
            override fun onPause() = send(Action.PAUSE)
            override fun onSkipToNext() = send(Action.NEXT)
            override fun onSkipToPrevious() = send(Action.PREVIOUS)
            override fun onStop() = send(Action.STOP)
            override fun onSeekTo(pos: Long) = send(Action.SEEK, pos)
        })
        s.isActive = true
        session = s
        return s
    }

    private fun send(action: String, position: Long? = null) {
        scope.launch {
            val r = remote.command(action, position)
            if (!r.ok) MdLog.w(TAG, "mirror command $action failed: ${r.error?.code}")
        }
    }

    private fun show(info: MediaSessionInfo, art: Bitmap?) {
        val s = ensureSession()
        s.setMetadata(
            MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, info.title)
                .putString(MediaMetadata.METADATA_KEY_ARTIST, info.artist)
                .putString(MediaMetadata.METADATA_KEY_ALBUM, info.album)
                .apply { info.durationMs?.let { putLong(MediaMetadata.METADATA_KEY_DURATION, it) } }
                .apply { if (art != null) putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, art) }
                .build(),
        )
        var actions = 0L
        if (Action.PLAY in info.actions || Action.TOGGLE in info.actions) actions = actions or PlaybackState.ACTION_PLAY
        if (Action.PAUSE in info.actions || Action.TOGGLE in info.actions) actions = actions or PlaybackState.ACTION_PAUSE
        if (Action.TOGGLE in info.actions) actions = actions or PlaybackState.ACTION_PLAY_PAUSE
        if (Action.NEXT in info.actions) actions = actions or PlaybackState.ACTION_SKIP_TO_NEXT
        if (Action.PREVIOUS in info.actions) actions = actions or PlaybackState.ACTION_SKIP_TO_PREVIOUS
        if (Action.SEEK in info.actions) actions = actions or PlaybackState.ACTION_SEEK_TO
        if (Action.STOP in info.actions) actions = actions or PlaybackState.ACTION_STOP
        val state = when (info.status) {
            PlayStatus.PLAYING -> PlaybackState.STATE_PLAYING
            PlayStatus.PAUSED -> PlaybackState.STATE_PAUSED
            PlayStatus.BUFFERING -> PlaybackState.STATE_BUFFERING
            PlayStatus.STOPPED -> PlaybackState.STATE_STOPPED
            else -> PlaybackState.STATE_NONE
        }
        val position = remote.positionNow(info) ?: PlaybackState.PLAYBACK_POSITION_UNKNOWN
        s.setPlaybackState(
            PlaybackState.Builder()
                .setActions(actions)
                .setState(state, position, if (info.isPlaying) info.rate.toFloat() else 0f, SystemClock.elapsedRealtime())
                .build(),
        )
        postNotification(s, info, art)
        lastKey = info.key
    }

    private fun postNotification(s: MediaSession, info: MediaSessionInfo, art: Bitmap?) {
        if (!Notifications.canPost(context)) return
        fun action(icon: Int, label: Int, action: String) = Notification.Action.Builder(
            Icon.createWithResource(context, icon),
            context.getString(label),
            PendingIntent.getBroadcast(
                context, action.hashCode(),
                Intent(context, MirrorActionReceiver::class.java).setAction(ACTION_PREFIX + action),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            ),
        ).build()
        val n = Notification.Builder(context, Notifications.CHANNEL_MIRROR)
            .setSmallIcon(R.drawable.ic_stat_link)
            .setContentTitle(info.title.ifEmpty { context.getString(R.string.unknown_title) })
            .setContentText(listOf(info.artist, info.appName).filter { it.isNotEmpty() }.joinToString(context.getString(R.string.list_separator)))
            .setSubText(context.getString(R.string.mirror_subtext))
            .setLargeIcon(art)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .setContentIntent(Notifications.openAppIntent(context, "now"))
            .addAction(action(R.drawable.ic_skip_previous, R.string.action_previous, Action.PREVIOUS))
            .addAction(
                if (info.isPlaying) action(R.drawable.ic_pause, R.string.action_pause, Action.PAUSE)
                else action(R.drawable.ic_play, R.string.action_play, Action.PLAY),
            )
            .addAction(action(R.drawable.ic_skip_next, R.string.action_next, Action.NEXT))
            .setStyle(Notification.MediaStyle().setMediaSession(s.sessionToken).setShowActionsInCompactView(0, 1, 2))
            .build()
        try {
            context.getSystemService(NotificationManager::class.java).notify(Notifications.ID_MIRROR, n)
        } catch (e: SecurityException) {
            MdLog.w(TAG, "cannot post mirror notification")
        }
    }

    private fun release() {
        val s = session ?: return
        session = null
        lastKey = null
        s.isActive = false
        s.release()
        Notifications.cancel(context, Notifications.ID_MIRROR)
    }

    companion object {
        private const val TAG = "MD/Mirror"
        const val ACTION_PREFIX = "app.mediaduet.mirror."
    }
}

/** Receives the mirror notification's buttons (pre-Android 13 media controls use these). */
class MirrorActionReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val action = intent.action?.removePrefix(MirrorSessionController.ACTION_PREFIX) ?: return
        if (action !in Action.ALL) return
        val engine = Engine.get(context)
        val pending = goAsync()
        engine.scope.launch {
            try {
                engine.remote.command(action)
            } finally {
                pending.finish()
            }
        }
    }
}
