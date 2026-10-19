package app.mediaduet.media

import android.content.Context
import android.content.pm.PackageManager
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSession
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.KeyEvent
import app.mediaduet.protocol.Action
import app.mediaduet.protocol.CommandResult
import app.mediaduet.protocol.ErrorCode
import app.mediaduet.protocol.CustomAction
import app.mediaduet.protocol.MediaCommand
import app.mediaduet.protocol.MediaQueue
import app.mediaduet.protocol.QueueEntry
import app.mediaduet.protocol.QueueHint
import app.mediaduet.util.MdLog
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * Event-driven view of the phone's media sessions (no polling): an OnActiveSessionsChangedListener
 * plus one MediaController.Callback per session. All methods run on the main thread.
 *
 * Players differ a lot (missing metadata, no PlaybackState, URIs instead of bitmaps, partial
 * action sets); nothing here assumes a particular app.
 */
class MediaSessionMonitor(
    private val context: Context,
    private val artwork: ArtworkProcessor,
    private val onChanged: () -> Unit,
) {
    private val manager = context.getSystemService(MediaSessionManager::class.java)
    private val handler = Handler(Looper.getMainLooper())
    private val tracked = LinkedHashMap<MediaSession.Token, Tracked>()
    private val counters = HashMap<String, Int>()
    private val labels = HashMap<String, String>()
    private var listening = false

    private val _accessGranted = MutableStateFlow(false)
    val accessGranted: StateFlow<Boolean> = _accessGranted

    /** Latest captured states (safe to read from any thread). */
    @Volatile var states: List<LocalSessionState> = emptyList()
        private set

    private val _queues = MutableStateFlow<Map<String, MediaQueue>>(emptyMap())
    /** Play queues by session key (only sessions that expose one), for `media.queue` and the UI. */
    val queues: StateFlow<Map<String, MediaQueue>> = _queues

    data class LastCommand(val sessionKey: String, val action: String, val origin: String, val atWallMs: Long)

    private val _lastCommand = MutableStateFlow<LastCommand?>(null)
    val lastCommand: StateFlow<LastCommand?> = _lastCommand

    private val sessionsListener = MediaSessionManager.OnActiveSessionsChangedListener { controllers ->
        guard("active sessions") { update(controllers.orEmpty()) }
    }

    private inline fun guard(what: String, block: () -> Unit) {
        try {
            block()
        } catch (e: Exception) {
            MdLog.e(TAG, "$what callback failed", e)
        }
    }

    private inner class Tracked(val controller: MediaController, val key: String) : MediaController.Callback() {
        var lastChangeMs: Long = 0

        // Player callbacks arrive on the main thread: any failure is logged, never fatal.
        override fun onPlaybackStateChanged(state: PlaybackState?) = guard("playback state") {
            val wasPlaying = isPlaying(lastState)
            lastState = state
            if (wasPlaying != isPlaying(state) || lastChangeMs == 0L) lastChangeMs = SystemClock.elapsedRealtime()
            publish()
        }

        override fun onMetadataChanged(metadata: MediaMetadata?) = guard("metadata") {
            lastChangeMs = SystemClock.elapsedRealtime()
            artwork.update(key, metadata) { handler.post { guard("artwork") { publish() } } }
            publish()
        }

        override fun onSessionDestroyed() = guard("session destroyed") {
            remove(controller.sessionToken)
        }

        override fun onQueueChanged(queue: MutableList<MediaSession.QueueItem>?) = guard("queue") {
            readQueue(queue)
            publish()
        }

        override fun onQueueTitleChanged(title: CharSequence?) = guard("queue title") {
            queueTitle = title?.toString().orEmpty()
            queueRev = revision()
            publish()
        }

        var lastState: PlaybackState? = controller.playbackState
        var queue: List<QueueEntry> = emptyList()
        var queueTitle: String = runCatching { controller.queueTitle?.toString() }.getOrNull().orEmpty()
        var queueRev: String = ""

        fun readQueue(items: List<MediaSession.QueueItem>?) {
            queue = items.orEmpty().map { q ->
                val d = q.description
                QueueEntry(q.queueId, d.title?.toString().orEmpty().trim(), d.subtitle?.toString().orEmpty().trim())
            }
            queueRev = revision()
        }

        /** Content revision of the queue: receivers re-fetch only when it changes. */
        fun revision(): String {
            var h = queueTitle.hashCode().toLong()
            for (e in queue) h = h * 31 + e.id * 17 + e.title.hashCode() * 7 + e.subtitle.hashCode()
            return java.lang.Long.toHexString(h xor queue.size.toLong())
        }

        init {
            readQueue(runCatching { controller.queue }.getOrNull())
        }
    }

    private fun isPlaying(state: PlaybackState?) = state?.state == PlaybackState.STATE_PLAYING

    /** Starts listening. Returns false (and reports no access) without notification access. */
    fun start(): Boolean {
        check(Looper.myLooper() == Looper.getMainLooper())
        if (listening) return true
        val component = MediaListenerService.component(context)
        return try {
            val initial = manager.getActiveSessions(component)
            manager.addOnActiveSessionsChangedListener(sessionsListener, component, handler)
            listening = true
            _accessGranted.value = true
            update(initial)
            MdLog.i(TAG, "monitoring ${initial.size} media session(s)")
            true
        } catch (e: SecurityException) {
            MdLog.w(TAG, "no notification listener access; media sessions unavailable")
            _accessGranted.value = false
            false
        }
    }

    fun stop() {
        if (!listening) return
        runCatching { manager.removeOnActiveSessionsChangedListener(sessionsListener) }
        listening = false
        tracked.values.forEach { runCatching { it.controller.unregisterCallback(it) } }
        tracked.clear()
        _accessGranted.value = false
        publish()
    }

    /** Re-checks access (e.g. after the user granted or revoked it). */
    fun recheck() {
        if (listening) {
            try {
                manager.getActiveSessions(MediaListenerService.component(context))
            } catch (e: SecurityException) {
                MdLog.w(TAG, "notification listener access revoked")
                stop()
            }
        } else {
            start()
        }
    }

    private fun update(controllers: List<MediaController>) {
        val own = context.packageName
        val incoming = controllers.filter { it.packageName != own } // never mirror our own bridge session
        val tokens = incoming.map { it.sessionToken }.toSet()
        tracked.keys.filter { it !in tokens }.toList().forEach { remove(it, notify = false) }
        val base = SystemClock.elapsedRealtime()
        incoming.forEachIndexed { index, c ->
            if (c.sessionToken !in tracked) {
                val n = (counters[c.packageName] ?: 0) + 1
                counters[c.packageName] = n
                val t = Tracked(c, "${c.packageName}#$n")
                // Priority order from the system: earlier entries are more recent.
                t.lastChangeMs = base - index
                tracked[c.sessionToken] = t
                c.registerCallback(t, handler)
                MdLog.i(TAG, "tracking session ${t.key}")
                artwork.update(t.key, runCatching { c.metadata }.getOrNull()) { handler.post { guard("artwork") { publish() } } }
            }
        }
        publish()
    }

    private fun remove(token: MediaSession.Token, notify: Boolean = true) {
        val t = tracked.remove(token) ?: return
        runCatching { t.controller.unregisterCallback(t) }
        artwork.forget(t.key)
        if (notify) publish()
    }

    private fun appName(pkg: String): String = labels.getOrPut(pkg) {
        try {
            val pm = context.packageManager
            pm.getApplicationLabel(pm.getApplicationInfo(pkg, 0)).toString()
        } catch (e: PackageManager.NameNotFoundException) {
            pkg
        }
    }

    private fun capture(t: Tracked): LocalSessionState {
        val c = t.controller
        val md = runCatching { c.metadata }.getOrNull()
        val ps = t.lastState ?: runCatching { c.playbackState }.getOrNull()
        fun text(vararg keys: String): String {
            for (k in keys) {
                val v = runCatching { md?.getString(k) }.getOrNull()
                if (!v.isNullOrBlank()) return v.trim()
            }
            return ""
        }
        val duration = runCatching { md?.getLong(MediaMetadata.METADATA_KEY_DURATION) }.getOrNull()?.takeIf { it > 0 }
        val customActions = ps?.customActions.orEmpty().filter { CustomActionFilter.isControl(it.name?.toString(), it.icon != 0) }
            .take(CustomAction.MAX_COUNT).mapNotNull { a ->
                val id = a.action ?: return@mapNotNull null
                CustomAction(id, a.name?.toString().orEmpty().trim(), artwork.icon(c.packageName, a.icon))
            }
        val activeQueueId = ps?.activeQueueItemId?.takeIf { it != MediaSession.QueueItem.UNKNOWN_ID.toLong() }
        val queueHint = if (t.queue.isEmpty()) null else QueueHint(t.queue.size, t.queueRev, activeQueueId)
        val contentType = runCatching { c.playbackInfo.audioAttributes.contentType }.getOrDefault(0)
        return LocalSessionState(
            key = t.key,
            packageName = c.packageName,
            appName = appName(c.packageName),
            title = text(MediaMetadata.METADATA_KEY_TITLE, MediaMetadata.METADATA_KEY_DISPLAY_TITLE),
            artist = text(MediaMetadata.METADATA_KEY_ARTIST, MediaMetadata.METADATA_KEY_DISPLAY_SUBTITLE, MediaMetadata.METADATA_KEY_ALBUM_ARTIST, MediaMetadata.METADATA_KEY_AUTHOR),
            album = text(MediaMetadata.METADATA_KEY_ALBUM),
            albumArtist = text(MediaMetadata.METADATA_KEY_ALBUM_ARTIST),
            durationMs = duration,
            hasPlaybackState = ps != null,
            state = ps?.state ?: PlaybackState.STATE_NONE,
            positionMs = ps?.position ?: -1,
            lastPositionUpdateMs = ps?.lastPositionUpdateTime ?: 0,
            speed = ps?.playbackSpeed ?: 0f,
            actions = ps?.actions ?: 0,
            contentType = contentType,
            artwork = artwork.refFor(t.key),
            lastChangeMs = t.lastChangeMs,
            customActions = customActions,
            queue = queueHint,
            mediaId = text(MediaMetadata.METADATA_KEY_MEDIA_ID),
        )
    }

    private fun publish() {
        // A session that fails to capture is skipped instead of breaking every other one.
        states = tracked.values.mapNotNull { t ->
            try {
                capture(t)
            } catch (e: Exception) {
                MdLog.w(TAG, "cannot capture ${t.key}: $e")
                null
            }
        }
        val queues = HashMap<String, MediaQueue>()
        for (t in tracked.values) {
            if (t.queue.isEmpty()) continue
            val active = states.firstOrNull { it.key == t.key }?.queue?.activeId
            val (offset, items) = MediaQueue.window(t.queue, active)
            queues[t.key] = MediaQueue(t.key, t.queueRev, t.queueTitle, active, t.queue.size, offset, items)
        }
        if (queues != _queues.value) _queues.value = queues
        onChanged()
    }

    /** Executes a protocol command via TransportControls after checking supported actions (§8.4). */
    fun execute(command: MediaCommand): CommandResult {
        if (!listening) return CommandResult.failure(ErrorCode.UNSUPPORTED, "no media session access")
        val current = tracked.values.associateBy { it.key }
        val target = if (command.key == null) {
            SnapshotBuilder.pickActive(tracked.values.map { capture(it) })?.let { current[it.key] }
        } else {
            current[command.key]
        } ?: return CommandResult.failure(ErrorCode.NOT_FOUND, "no such session")
        val state = capture(target)
        val controls = target.controller.transportControls
        when (command.action) {
            Action.CUSTOM -> {
                val ps = target.lastState ?: runCatching { target.controller.playbackState }.getOrNull()
                val action = ps?.customActions?.firstOrNull { it.action == command.customAction }
                    ?: return CommandResult.failure(ErrorCode.ACTION_UNSUPPORTED)
                controls.sendCustomAction(action, action.extras)
                return executed(target, command)
            }
            Action.PLAY_QUEUE_ITEM -> {
                val id = command.queueId ?: return CommandResult.failure(ErrorCode.BAD_MESSAGE)
                if (target.queue.none { it.id == id }) return CommandResult.failure(ErrorCode.NOT_FOUND, "no such queue item")
                controls.skipToQueueItem(id)
                return executed(target, command)
            }
        }
        if (command.action !in state.protocolActions()) return CommandResult.failure(ErrorCode.ACTION_UNSUPPORTED)
        when (command.action) {
            Action.PLAY -> controls.play()
            Action.PAUSE -> controls.pause()
            Action.TOGGLE -> {
                val hasBoth = state.actions and PlaybackConst.ACTION_PLAY != 0L && state.actions and PlaybackConst.ACTION_PAUSE != 0L
                if (hasBoth) {
                    if (state.isActivelyPlaying) controls.pause() else controls.play()
                } else {
                    val down = KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE)
                    target.controller.dispatchMediaButtonEvent(down)
                    target.controller.dispatchMediaButtonEvent(KeyEvent.changeAction(down, KeyEvent.ACTION_UP))
                }
            }
            Action.NEXT -> controls.skipToNext()
            Action.PREVIOUS -> controls.skipToPrevious()
            Action.SEEK -> {
                val pos = (command.positionMs ?: 0L).coerceIn(0L, state.durationMs ?: Long.MAX_VALUE)
                controls.seekTo(pos)
            }
            Action.STOP -> controls.stop()
            else -> return CommandResult.failure(ErrorCode.ACTION_UNSUPPORTED)
        }
        return executed(target, command)
    }

    private fun executed(target: Tracked, command: MediaCommand): CommandResult {
        _lastCommand.value = LastCommand(target.key, command.action, command.origin, System.currentTimeMillis())
        MdLog.i(TAG, "executed ${command.action} on ${target.key} (origin ${command.origin})")
        return CommandResult.OK
    }

    /** PlaybackState actions of [packageName]'s most recent session, or null when it has none. Main thread. */
    fun actionsOf(packageName: String): Long? =
        tracked.values.filter { it.controller.packageName == packageName }.maxByOrNull { it.lastChangeMs }
            ?.let { (it.lastState ?: runCatching { it.controller.playbackState }.getOrNull())?.actions ?: 0L }

    /**
     * Handoff without an activity: asks [packageName]'s session to play [uri] (playFromUri) or a
     * search (playFromSearch). False when it has no session that supports it. Main thread.
     */
    fun playFrom(packageName: String, uri: android.net.Uri?, query: String?, extras: android.os.Bundle): Boolean {
        val t = tracked.values.filter { it.controller.packageName == packageName }.maxByOrNull { it.lastChangeMs } ?: return false
        val actions = (t.lastState ?: runCatching { t.controller.playbackState }.getOrNull())?.actions ?: 0L
        val controls = t.controller.transportControls
        return when {
            uri != null && actions and PlaybackState.ACTION_PLAY_FROM_URI != 0L -> {
                controls.playFromUri(uri, extras)
                MdLog.i(TAG, "playFromUri on ${t.key}")
                true
            }
            query != null && actions and PlaybackState.ACTION_PLAY_FROM_SEARCH != 0L -> {
                controls.playFromSearch(query, extras)
                MdLog.i(TAG, "playFromSearch on ${t.key}")
                true
            }
            else -> false
        }
    }

    /**
     * Sets STREAM_MUSIC through the active local-playback session (`MediaController.setVolumeTo`),
     * an alternative path tried when AudioManager calls were ignored. Main thread only.
     */
    fun setActiveSessionVolume(index: Int) {
        val active = SnapshotBuilder.pickActive(states)?.key ?: return
        val t = tracked.values.firstOrNull { it.key == active } ?: return
        val info = t.controller.playbackInfo ?: return
        if (info.playbackType != MediaController.PlaybackInfo.PLAYBACK_TYPE_LOCAL) return
        t.controller.setVolumeTo(index, 0)
    }

    companion object {
        private const val TAG = "MD/Media"
    }
}
