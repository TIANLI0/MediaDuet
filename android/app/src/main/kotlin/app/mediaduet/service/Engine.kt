package app.mediaduet.service

import android.annotation.SuppressLint
import android.content.Context
import android.os.Build
import android.os.SystemClock
import app.mediaduet.BuildConfig
import app.mediaduet.audio.AudioReceiver
import app.mediaduet.crypto.KeystoreTrustStore
import app.mediaduet.link.LinkHost
import app.mediaduet.link.LinkManager
import app.mediaduet.link.VolumeRestrictedException
import app.mediaduet.link.PairingController
import app.mediaduet.media.ArtworkProcessor
import app.mediaduet.media.AudioRouteMonitor
import app.mediaduet.media.OutputRoutes
import app.mediaduet.media.LocalSessionState
import app.mediaduet.media.MediaSessionMonitor
import app.mediaduet.media.SnapshotBuilder
import app.mediaduet.media.SnapshotSuppressor
import app.mediaduet.media.AutoState
import app.mediaduet.media.ResolvedTarget
import app.mediaduet.media.Side
import app.mediaduet.media.TargetResolver
import app.mediaduet.media.UserControl
import app.mediaduet.media.VolumeController
import app.mediaduet.protocol.VolumeSet
import app.mediaduet.protocol.VolumeState
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.stateIn
import app.mediaduet.protocol.AudioOffer
import app.mediaduet.protocol.BlobChunk
import app.mediaduet.protocol.Caps
import app.mediaduet.protocol.CommandResult
import app.mediaduet.protocol.DeviceInfo
import app.mediaduet.protocol.LanEndpoint
import app.mediaduet.protocol.LyricsPush
import app.mediaduet.protocol.MediaCommand
import app.mediaduet.protocol.MediaSnapshot
import app.mediaduet.protocol.PolicyStatus
import app.mediaduet.remote.MirrorSessionController
import app.mediaduet.remote.RemoteMediaStore
import app.mediaduet.transport.TransportController
import app.mediaduet.util.MdLog
import app.mediaduet.util.MonoClock
import kotlinx.coroutines.CoroutineExceptionHandler
import kotlinx.coroutines.CoroutineScope
import app.mediaduet.util.CrashLog
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.JsonObject
import java.util.concurrent.atomic.AtomicLong

/**
 * Process-wide engine owned by the Application. It runs as long as the process lives; the
 * anchors keeping the process alive are the system-bound NotificationListenerService, the
 * connectedDevice foreground service and companion-device presence.
 */
class Engine private constructor(private val app: Context) : LinkHost {

    /** Long-lived scope: a failing child is logged (and recorded), never fatal to the process. */
    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default + CoroutineExceptionHandler { _, e ->
        MdLog.e(TAG, "uncaught coroutine failure", e)
        CrashLog.instance?.recordNonFatal("engine coroutine", e)
    })
    val clock = MonoClock { SystemClock.elapsedRealtime() }
    val settings = AppSettings(app)
    val trust = KeystoreTrustStore(app)
    val pairing = PairingController(scope, clock)
    val link = LinkManager(scope, this, trust, pairing, clock)
    val artwork = ArtworkProcessor(app, scope)
    val media = MediaSessionMonitor(app, artwork) { requestSnapshot() }
    val transports = TransportController(
        context = app,
        scope = scope,
        deviceId = settings.deviceId,
        deviceName = { settings.deviceName.value },
        onAccept = link::accept,
        onLanChanged = link::broadcastTransportInfo,
    )
    val remote = RemoteMediaStore(scope, clock, link, app.cacheDir)
    val audio = AudioReceiver(scope, settings.audioReceiveEnabled) { peer, stream, reason -> link.sendAudioStop(peer, stream, reason) }
    val mirror = MirrorSessionController(app, scope, remote, settings.mirrorEnabled)
    val volume = VolumeController(app)
    /** Where the phone's media audio plays; a change re-sends the snapshot (outputLatencyMs). */
    val outputRoute = AudioRouteMonitor(app) { requestSnapshot() }

    /** The phone's estimated media output latency (Bluetooth delay setting, or 0). */
    val localOutputLatencyMs: StateFlow<Int> by lazy {
        combine(outputRoute.state, settings.bluetoothLatencyMs) { out, bt -> OutputRoutes.latencyMs(out.route, bt) }
            .stateIn(scope, SharingStarted.Eagerly, OutputRoutes.latencyMs(outputRoute.state.value.route, settings.bluetoothLatencyMs.value))
    }

    private val _target = MutableStateFlow<ResolvedTarget?>(null)
    /**
     * The player's resolved control target (auto / phone / PC), see [TargetResolver]. Resolved in
     * the engine from snapshots and session callbacks with timestamps (works in the background and
     * across UI restarts), re-evaluated when a hold expires.
     */
    val target: StateFlow<ResolvedTarget?> = _target

    private val userControl = MutableStateFlow<UserControl?>(null)
    private val autoTick = MutableStateFlow(0L)

    /** Play / pause / seek / skip / volume on [side]/[key] from MediaDuet's UI (Auto hold). */
    fun noteUserControl(side: Side, key: String) {
        userControl.value = UserControl(side, key, clock.nowMs())
    }

    private fun startTargetResolver() {
        scope.launch {
            var state: AutoState? = null
            var recheck: kotlinx.coroutines.Job? = null
            val inputs = combine(settings.target, remote.state, _localStates, link.status) { choice, rem, local, status ->
                Triple(choice, rem?.takeIf { it.connected && status.current != null }, local)
            }
            combine(inputs, userControl, autoTick) { i, uc, _ -> i to uc }.collect { (i, uc) ->
                val (choice, rem, local) = i
                val now = clock.nowMs()
                val next = TargetResolver.resolve(choice, rem?.snapshot, rem != null, local, state, now, uc)
                if (next.target != state?.target) MdLog.i(TAG, "player target: ${next.target}")
                state = next
                _target.value = next.target
                recheck?.cancel()
                recheck = next.recheckAtMs?.let { at ->
                    launch {
                        delay((at - now).coerceAtLeast(0) + 20)
                        autoTick.value = clock.nowMs()
                    }
                }
            }
        }
    }

    private val _localStates = MutableStateFlow<List<LocalSessionState>>(emptyList())
    /** Local sessions as currently captured (for the "Phone media" screen). */
    val localStates: StateFlow<List<LocalSessionState>> = _localStates

    private val _lastSentSnapshot = MutableStateFlow<MediaSnapshot?>(null)
    val lastSentSnapshot: StateFlow<MediaSnapshot?> = _lastSentSnapshot

    private val _listenerConnected = MutableStateFlow(false)
    val listenerConnected: StateFlow<Boolean> = _listenerConnected

    private val _started = MutableStateFlow(false)
    val started: StateFlow<Boolean> = _started

    @Volatile var startedAtMs: Long = 0
        private set

    private val snapshotRequests = Channel<Unit>(Channel.CONFLATED)
    private val suppressor = SnapshotSuppressor()
    private val gen = AtomicLong()

    @Synchronized
    fun start() {
        if (_started.value) return
        _started.value = true
        startedAtMs = clock.nowMs()
        MdLog.i(TAG, "engine starting (device ${settings.deviceId.take(8)}, ${settings.deviceName.value})")
        Notifications.createChannels(app)
        transports.start()
        startTargetResolver()
        scope.launch(Dispatchers.Main) { media.start() }
        mirror.start()
        volume.start()
        scope.launch(Dispatchers.Main) { outputRoute.start() }
        scope.launch { localOutputLatencyMs.drop(1).collect { requestSnapshot() } }
        scope.launch { volume.state.collect { link.broadcastVolume(it) } }
        // Keep the mirror / PC focus in sync with an explicit PC choice.
        scope.launch {
            settings.target.collect { c -> remote.select(if (c.side == Side.PC) c.key else null) }
        }

        scope.launch {
            for (request in snapshotRequests) {
                delay(SNAPSHOT_DEBOUNCE_MS) // coalesce bursts of controller callbacks
                publishSnapshot()
            }
        }
        scope.launch {
            combine(_localStates, remote.state) { local, rem ->
                local.any { it.isActivelyPlaying } || rem?.snapshot?.sessions?.any { it.isPlaying } == true
            }.distinctUntilChanged().collect { link.anyPlaying.value = it }
        }
        scope.launch {
            pairing.state.map { it.prompt }.distinctUntilChanged().collect { prompt ->
                if (prompt != null && !prompt.localAccepted) {
                    Notifications.showPairingRequest(app, prompt.peerName, prompt.sas)
                } else {
                    Notifications.cancel(app, Notifications.ID_PAIRING)
                }
            }
        }
        scope.launch {
            settings.deviceName.drop(1).collect {
                transports.onDeviceNameChanged()
                link.broadcastDeviceInfo()
            }
        }
        scope.launch {
            settings.audioReceiveEnabled.drop(1).collect { on -> if (!on) audio.stop(null, "disabled", notifyPeer = true) }
        }
    }

    fun onListenerConnected() {
        _listenerConnected.value = true
        start()
        scope.launch(Dispatchers.Main) { media.recheck() }
    }

    fun onListenerDisconnected() {
        _listenerConnected.value = false
    }

    /** Re-evaluates permissions and access (UI resume, permission results). */
    fun refreshPermissions() {
        transports.refresh()
        scope.launch(Dispatchers.Main) { media.recheck() }
    }

    private fun requestSnapshot() {
        _localStates.value = media.states
        snapshotRequests.trySend(Unit)
    }

    /** Current media output latency (re-reads the route: Android has no callback for route-only changes). */
    private fun currentOutputLatency(): Int {
        outputRoute.refresh()
        return OutputRoutes.latencyMs(outputRoute.state.value.route, settings.bluetoothLatencyMs.value)
    }

    private fun publishSnapshot() = try {
        publishSnapshotUnsafe()
    } catch (e: Exception) {
        MdLog.e(TAG, "snapshot failed", e)
    }

    private fun publishSnapshotUnsafe() {
        val now = clock.nowMs()
        val candidate = SnapshotBuilder.build(0, media.states, now, currentOutputLatency())
        val send = synchronized(suppressor) { suppressor.shouldSend(candidate, now) }
        if (!send) return
        val snapshot = candidate.copy(gen = gen.incrementAndGet())
        _lastSentSnapshot.value = snapshot
        val active = snapshot.activeSession
        MdLog.i(TAG, "snapshot gen=${snapshot.gen} sessions=${snapshot.sessions.size} active=${snapshot.active} " +
            "status=${active?.status} title=\"${active?.title}\" art=${active?.artwork?.hash?.take(12)}")
        link.broadcastSnapshot(snapshot)
    }

    // ------------------------------------------------------------------------------------
    // LinkHost
    // ------------------------------------------------------------------------------------

    override fun localDevice() = DeviceInfo(
        id = settings.deviceId,
        name = settings.deviceName.value,
        platform = "android",
        model = "${Build.MANUFACTURER} ${Build.MODEL}",
        os = "Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})",
        app = BuildConfig.VERSION_NAME,
    )

    override fun capabilities(): List<String> = buildList {
        add(Caps.MEDIA_SOURCE)
        add(Caps.MEDIA_CONTROL)
        add(Caps.ARTWORK)
        add(Caps.LYRICS)
        add(Caps.POLICY_CLIENT)
        add(Caps.VOLUME)
        if (settings.audioReceiveEnabled.value) add(Caps.AUDIO_RECV_PCM)
    }

    override fun freshSnapshot(): MediaSnapshot =
        SnapshotBuilder.build(gen.incrementAndGet(), media.states, clock.nowMs(), currentOutputLatency()).also { _lastSentSnapshot.value = it }

    override suspend fun executeCommand(peerId: String, command: MediaCommand): CommandResult {
        val result = withContext(Dispatchers.Main) { media.execute(command) }
        requestSnapshot()
        return result
    }

    override fun artwork(hash: String): ByteArray? = artwork.bytes(hash)

    override fun volumeState(): VolumeState = volume.state.value
    override suspend fun applyVolume(peerId: String, request: VolumeSet): VolumeState {
        val r = withContext(Dispatchers.Main) { volume.apply(request) { index -> media.setActiveSessionVolume(index) } }
        if (r.ignored) {
            Notifications.showVolumeRestricted(app)
            throw VolumeRestrictedException(r.state)
        }
        Notifications.cancel(app, Notifications.ID_VOLUME)
        return r.state
    }
    override fun onPeerVolume(peerId: String, state: VolumeState) = remote.onPeerVolume(state)
    override fun bluetoothName(): String? = transports.bluetoothName()

    /** Executes a user command on a phone session directly through its MediaController. */
    suspend fun executeLocal(key: String?, action: String, positionMs: Long? = null): CommandResult {
        val result = withContext(Dispatchers.Main) {
            media.execute(MediaCommand(key, action, positionMs, MediaCommand.ORIGIN_USER, "android_ui"))
        }
        requestSnapshot()
        return result
    }

    override fun lanEndpoints(): List<LanEndpoint> = transports.lanEndpoints()

    override fun onSessionStarted(peer: DeviceInfo, resumed: Boolean, peerCaps: Set<String>) =
        remote.onSessionStarted(peer.id, resumed)

    override fun onPeerDisconnected(peerId: String) {
        remote.onDisconnected(peerId)
        audio.stopForPeer(peerId)
    }

    override fun onPeerSnapshot(peerId: String, snapshot: MediaSnapshot, rttMs: Long?) = remote.onSnapshot(peerId, snapshot, rttMs)
    override fun onBlobChunk(peerId: String, chunk: BlobChunk) = remote.onChunk(chunk)
    override fun onArtworkNotFound(peerId: String, hash: String) = remote.onArtworkNotFound(hash)
    override fun onLyrics(peerId: String, lyrics: LyricsPush) = remote.onLyrics(lyrics)
    override fun onLyricsNone(peerId: String, trackId: String) = remote.onLyricsNone(trackId)
    override fun onPolicyStatus(peerId: String, status: PolicyStatus) = remote.onPolicyStatus(status)
    override suspend fun onAudioOffer(peerId: String, offer: AudioOffer, baseKey: ByteArray): JsonObject =
        withContext(Dispatchers.IO) { audio.handleOffer(peerId, offer, baseKey) }

    override fun onAudioStop(peerId: String, streamId: String, reason: String) = audio.stop(streamId, reason, notifyPeer = false)

    companion object {
        private const val TAG = "MD/Engine"
        private const val SNAPSHOT_DEBOUNCE_MS = 150L

        @SuppressLint("StaticFieldLeak") // holds the application context only
        @Volatile private var instance: Engine? = null

        fun get(context: Context): Engine =
            instance ?: synchronized(this) {
                instance ?: Engine(context.applicationContext).also { instance = it }
            }
    }
}
