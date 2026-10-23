package app.mediaduet.ui.screens

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.SystemClock
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.mediaduet.media.LocalSessionState
import app.mediaduet.media.Side
import app.mediaduet.protocol.Caps
import app.mediaduet.protocol.CommandResult
import app.mediaduet.protocol.MediaSessionInfo
import app.mediaduet.protocol.PlayStatus
import app.mediaduet.protocol.VolumeSet
import app.mediaduet.remote.RemotePosition
import app.mediaduet.service.Engine
import app.mediaduet.ui.UiContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** What the player shows: one session of either the phone or the PC, with uniform accessors. */
class PlayerView(
    val side: Side,
    val automatic: Boolean,
    val info: MediaSessionInfo,
    val art: Bitmap?,
    /** False when the PC side is shown while disconnected (stale state). */
    val connected: Boolean,
    val canControl: Boolean,
    /** Changes whenever the position base changes (snapshot / playback state). */
    val positionKey: Any,
    private val positionAt: (Long) -> Long?,
    /** PC name for PC sessions. */
    val deviceName: String?,
    /** The PC was chosen but is not connected; the phone's media is shown instead. */
    val fallback: Boolean = false,
    /** Delay between the reported position and audible sound on the playing device (ms). */
    val outputLatencyMs: Int = 0,
    /** `windows` / `android` for the other device's sessions. */
    val devicePlatform: String? = null,
) {
    val key: String get() = info.key
    val playing: Boolean get() = info.status == PlayStatus.PLAYING || info.status == PlayStatus.BUFFERING

    fun position(nowMs: Long = SystemClock.elapsedRealtime()): Long = positionAt(nowMs) ?: 0L
}

/** Builds the [PlayerView] for the engine's resolved control target. */
@Composable
fun rememberPlayerView(engine: Engine): PlayerView? {
    val target by engine.target.collectAsStateWithLifecycle()
    val remote by engine.remote.state.collectAsStateWithLifecycle()
    val artworks by engine.remote.artwork.collectAsStateWithLifecycle()
    val local by engine.localStates.collectAsStateWithLifecycle()
    val link by engine.link.status.collectAsStateWithLifecycle()
    val localLatency by engine.localOutputLatencyMs.collectAsStateWithLifecycle()
    val t = target ?: return null
    return when (t.side) {
        Side.PC -> {
            val st = remote ?: return null
            val s = st.snapshot.sessions.firstOrNull { it.key == t.key } ?: return null
            PlayerView(
                side = Side.PC,
                automatic = t.automatic,
                info = s,
                art = artworks[s.artwork?.hash],
                connected = st.connected,
                canControl = st.connected && Caps.MEDIA_CONTROL in link.current?.peerCaps.orEmpty(),
                positionKey = st.positionAtMs to s.key,
                positionAt = { now -> RemotePosition.at(s, st.positionAtMs, now) },
                deviceName = link.current?.peer?.name ?: link.sessions.firstOrNull { it.peer.id == st.peerId }?.peer?.name,
                outputLatencyMs = s.outputLatencyMs ?: 0,
                devicePlatform = link.sessions.firstOrNull { it.peer.id == st.peerId }?.peer?.platform,
            )
        }
        Side.PHONE -> {
            val s: LocalSessionState = local.firstOrNull { it.key == t.key } ?: return null
            val hash = s.artwork?.hash
            val art = remember(hash) { hash?.let { engine.artwork.bytes(it) }?.let { BitmapFactory.decodeByteArray(it, 0, it.size) } }
            PlayerView(
                side = Side.PHONE,
                automatic = t.automatic,
                info = s.toSessionInfo(SystemClock.elapsedRealtime()),
                art = art,
                connected = true,
                canControl = s.protocolActions().isNotEmpty(),
                positionKey = Triple(s.key, s.lastPositionUpdateMs, s.state),
                positionAt = { now -> s.estimatedPosition(now) },
                deviceName = null,
                fallback = t.fallback,
                outputLatencyMs = localLatency,
            )
        }
    }
}

/** Sends a user command to the view's device: MediaController for the phone, media.command for the PC. */
suspend fun UiContext.command(view: PlayerView, action: String, positionMs: Long? = null): CommandResult {
    // Auto keeps a target the user is controlling (see TargetResolver.USER_HOLD_MS).
    engine.noteUserControl(view.side, view.key)
    return when (view.side) {
        Side.PHONE -> engine.executeLocal(view.key, action, positionMs)
        Side.PC -> engine.remote.command(action, positionMs, view.key)
    }
}

/** Presses the player's own button [id] (like / favourite, …; phone sessions). */
suspend fun UiContext.customAction(view: PlayerView, id: String): CommandResult {
    engine.noteUserControl(view.side, view.key)
    return when (view.side) {
        Side.PHONE -> engine.executeLocal(view.key, app.mediaduet.protocol.Action.CUSTOM, customAction = id)
        Side.PC -> CommandResult.failure(app.mediaduet.protocol.ErrorCode.ACTION_UNSUPPORTED)
    }
}

/** Plays queue entry [id] of the view's session (phone sessions, NetEase on the PC). */
suspend fun UiContext.playQueueItem(view: PlayerView, id: Long): CommandResult {
    engine.noteUserControl(view.side, view.key)
    return when (view.side) {
        Side.PHONE -> engine.executeLocal(view.key, app.mediaduet.protocol.Action.PLAY_QUEUE_ITEM, queueId = id)
        Side.PC -> engine.remote.playQueueItem(view.key, id)
    }
}

/** Volume of the view's device, null when it cannot be shown / changed. */
class VolumeView(val level: Float, val muted: Boolean, val enabled: Boolean, val fixed: Boolean)

@Composable
fun rememberVolumeView(engine: Engine, view: PlayerView): VolumeView? {
    val phone by engine.volume.state.collectAsStateWithLifecycle()
    val pc by engine.remote.volume.collectAsStateWithLifecycle()
    val link by engine.link.status.collectAsStateWithLifecycle()
    return when (view.side) {
        Side.PHONE -> {
            val fixed = engine.volume.fixed
            VolumeView(phone.level.toFloat(), phone.muted, enabled = !fixed, fixed = fixed)
        }
        Side.PC -> {
            val supported = Caps.VOLUME in link.current?.peerCaps.orEmpty()
            val state = pc ?: return if (supported && view.connected) VolumeView(0f, false, enabled = false, fixed = false) else null
            VolumeView(state.level.toFloat(), state.muted, enabled = supported && view.connected, fixed = false)
        }
    }
}

/** Applies a volume change to the view's device. */
suspend fun UiContext.setVolume(view: PlayerView, request: VolumeSet) {
    engine.noteUserControl(view.side, view.key)
    when (view.side) {
        Side.PHONE -> withContext(Dispatchers.Default) { engine.volume.apply(request) }
        Side.PC -> engine.link.sendVolumeSet(request)
    }
}
