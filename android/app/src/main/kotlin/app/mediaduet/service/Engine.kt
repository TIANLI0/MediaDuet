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
import app.mediaduet.protocol.Action
import app.mediaduet.protocol.AudioOffer
import app.mediaduet.protocol.BlobChunk
import app.mediaduet.protocol.Caps
import app.mediaduet.protocol.CommandResult
import app.mediaduet.protocol.DeviceInfo
import app.mediaduet.protocol.ErrorCode
import app.mediaduet.protocol.LanEndpoint
import app.mediaduet.protocol.LyricsPush
import app.mediaduet.protocol.MediaCommand
import app.mediaduet.protocol.MediaSnapshot
import app.mediaduet.protocol.PeerPolicy
import app.mediaduet.protocol.PolicyStatus
import app.mediaduet.protocol.SinkState
import app.mediaduet.media.CallMonitor
import app.mediaduet.policy.ArbiterAction
import app.mediaduet.policy.ArbiterLastAction
import app.mediaduet.policy.ArbiterSession
import app.mediaduet.policy.Cancellable
import app.mediaduet.policy.PeerArbiter
import app.mediaduet.policy.PeerArbiterConfig
import app.mediaduet.policy.PolicyTimer
import app.mediaduet.transport.PeerDiscovery
import app.mediaduet.protocol.MediaQueue
import app.mediaduet.lyrics.LyricsNotifier
import app.mediaduet.lyrics.LyricsRepository
import app.mediaduet.handoff.HandoffApp
import app.mediaduet.handoff.HandoffErrors
import app.mediaduet.handoff.HandoffExecutor
import app.mediaduet.handoff.HandoffOutcome
import app.mediaduet.handoff.HandoffPlay
import app.mediaduet.handoff.PlayResult
import app.mediaduet.update.Updater
import app.mediaduet.unlock.UnlockController
import app.mediaduet.unlock.UnlockEnroll
import app.mediaduet.unlock.UnlockRequest
import kotlinx.coroutines.sync.Mutex
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import app.mediaduet.media.TargetChoice
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
        onLanChanged = {
            link.broadcastTransportInfo()
            discovery.restart()
            dialer.nudge()
        },
    )
    val remote = RemoteMediaStore(scope, clock, link, app.cacheDir)
    /** Other Android devices nearby (DNS-SD), for pairing and reconnecting (§13). */
    val discovery = PeerDiscovery(app, settings.deviceId)
    /** Dials paired Android devices (the one with the smaller id dials). */
    val dialer = PeerDialer(scope, settings.deviceId, link, trust, discovery, settings)
    private val calls = CallMonitor(app, scope)
    /** Lyrics from the PC and the phone's own lookup (see [LyricsRepository]). */
    val lyrics = LyricsRepository(scope, app.filesDir, settings.onlineLyrics, onFound = { trackId, lrc, tr, source ->
        // The PC's own lookup may miss what the phone found (other match rules / endpoints).
        scope.launch { if (link.sendLyrics(trackId, lrc, tr, source)) MdLog.i(TAG, "shared lyrics for $trackId with the PC") }
    })
    val audio = AudioReceiver(scope, settings.audioReceiveEnabled) { peer, stream, reason -> link.sendAudioStop(peer, stream, reason) }
    val mirror = MirrorSessionController(app, scope, remote, settings.mirrorEnabled)
    /** The current lyric line as a notification (Settings > Lyrics, default off). */
    val lyricsNotifier by lazy { LyricsNotifier(app, scope, settings, target, remote, localStates, lyrics, artwork, localOutputLatencyMs) }
    val volume = VolumeController(app)
    /** Software updates from GitHub (Settings > About). */
    val updater = Updater(app, scope, settings)
    /** Unlocking paired PCs with this phone's fingerprint (docs/unlock.md). */
    val unlock = UnlockController(app)
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

    /**
     * The user changed the manual lyrics offset of player [appId] (Now Playing, Settings): stored
     * here and sent to the linked devices so a song shows with the same timing on both (§8.2).
     */
    fun setLyricOffset(appId: String, ms: Int) {
        if (appId.isEmpty()) return
        settings.setLyricOffset(appId, ms)
        link.broadcastLyricsOffset(appId, settings.lyricOffsets.value[appId] ?: 0)
    }

    private fun startTargetResolver() {
        scope.launch {
            var state: AutoState? = null
            var recheck: kotlinx.coroutines.Job? = null
            val inputs = combine(settings.target, remote.state, _localStates, link.status, settings.mode) { choice, rem, local, status, mode ->
                // This phone only: the PC side does not exist for the player.
                if (!mode.linked) Triple(TargetChoice.AUTO, null, local)
                else Triple(choice, rem?.takeIf { it.connected && status.current != null }, local)
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

    /** Asks the lyrics repository for the player's current track (works without the UI, e.g. for the mirror). */
    private fun startLyricsRequests() {
        scope.launch {
            combine(_target, remote.state, _localStates, link.status) { t, rem, local, status ->
                when (t?.side) {
                    Side.PC -> rem?.snapshot?.sessions?.firstOrNull { it.key == t.key }?.let { Triple(it, Side.PC, rem.connected && status.current != null) }
                    Side.PHONE -> local.firstOrNull { it.key == t.key }?.let { Triple(it.toSessionInfo(clock.nowMs()), Side.PHONE, false) }
                    null -> null
                }
            }
                .distinctUntilChanged { a, b -> a?.first?.trackId == b?.first?.trackId && a?.second == b?.second && a?.third == b?.third }
                .collect { cur -> cur?.let { (info, side, pc) -> lyrics.request(info, side, pc) } }
        }
    }

    private val _localStates = MutableStateFlow<List<LocalSessionState>>(emptyList())
    /** Local sessions as currently captured (for the "Phone media" screen). */
    val localStates: StateFlow<List<LocalSessionState>> = _localStates

    // ---- Arbitration with other Android devices (§13) --------------------------------------------

    /** All arbiter calls and its timers run here (it is not thread-safe). */
    private val arbiterDispatcher = Dispatchers.Default.limitedParallelism(1)
    private val _peerState = MutableStateFlow<PeerPolicy?>(null)
    private val _peerPolicies = MutableStateFlow<Map<String, PeerPolicy>>(emptyMap())
    /** The latest `policy.peer` of each connected Android device. */
    val peerPolicies: StateFlow<Map<String, PeerPolicy>> = _peerPolicies
    private val _arbiterLastAction = MutableStateFlow<ArbiterLastAction?>(null)
    /** What the Android arbitration did last on this device (Settings › Arbitration). */
    val arbiterLastAction: StateFlow<ArbiterLastAction?> = _arbiterLastAction

    private val arbiter: PeerArbiter = PeerArbiter(
        selfId = settings.deviceId,
        timer = PolicyTimer { delayMs, block ->
            val job = scope.launch(arbiterDispatcher) {
                delay(delayMs)
                block()
                _arbiterLastAction.value = arbiterLastActionNow()
            }
            Cancellable { job.cancel() }
        },
        nowMs = clock::nowMs,
        wallMs = System::currentTimeMillis,
        execute = ::executeArbiterAction,
        publish = { p ->
            _peerState.value = p
            link.broadcastPeerPolicy(p)
        },
    )

    /** Only call on [arbiterDispatcher]. */
    private fun arbiterLastActionNow(): ArbiterLastAction? = arbiter.lastAction

    private fun onArbiter(block: PeerArbiter.() -> Unit) {
        scope.launch(arbiterDispatcher) {
            arbiter.block()
            _arbiterLastAction.value = arbiter.lastAction
        }
    }

    private fun executeArbiterAction(a: ArbiterAction) {
        MdLog.i(TAG, "peer arbitration: ${a.kind} ${a.key} (${a.reason})")
        scope.launch(Dispatchers.Main) {
            val action = if (a.kind == ArbiterAction.Kind.PAUSE) Action.PAUSE else Action.PLAY
            var r = media.execute(MediaCommand(a.key, action, null, MediaCommand.ORIGIN_POLICY, a.reason))
            // Players that only take play/pause as a toggle.
            if (!r.ok && r.error?.code == ErrorCode.ACTION_UNSUPPORTED) {
                r = media.execute(MediaCommand(a.key, Action.TOGGLE, null, MediaCommand.ORIGIN_POLICY, a.reason))
            }
            if (!r.ok) MdLog.w(TAG, "peer arbitration ${a.kind} failed: ${r.error?.code}")
            requestSnapshot()
        }
    }

    private fun startPeerArbitration() {
        scope.launch {
            settings.peerPolicy.collect { p ->
                onArbiter { setConfig(PeerArbiterConfig(mode = p.mode, priority = p.priority, autoResume = p.autoResume, pauseForCalls = p.pauseForCalls)) }
            }
        }
        scope.launch {
            _localStates.collect { states ->
                val now = clock.nowMs()
                val sessions = states.map { ArbiterSession(it.key, it.packageName, it.toSessionInfo(now).trackId, it.status) }
                onArbiter { onLocalSessions(sessions) }
            }
        }
        calls.start()
        scope.launch { calls.active.collect { active -> onArbiter { onLocalCall(active) } } }
    }

    /** A command for a local session from the UI or a peer; [key] null = the active session. */
    private fun noteLocalCommand(key: String?, action: String, policy: Boolean) {
        val k = key ?: SnapshotBuilder.pickActive(media.states)?.key ?: return
        onArbiter { onLocalCommand(k, action, policy) }
    }

    // ---- Peer focus: which connected device the player shows ---------------------------------

    /** The last state of each peer, so the player can switch between connected devices. */
    private class PeerCache {
        var snapshot: Pair<MediaSnapshot, Long>? = null
        var volume: VolumeState? = null
        var policy: PolicyStatus? = null
        var sink: SinkState? = null
        var handoff: List<HandoffApp> = emptyList()
    }

    private val routeLock = Any()
    private val peerCache = HashMap<String, PeerCache>()
    /** The peer whose state [remote] currently holds. */
    private var routedPeer: String? = null

    /**
     * Caches [update] for [peerId] and applies it to the player's state when that peer is the
     * focused one (switching the player over to it first if needed).
     */
    private fun routePeer(peerId: String, update: PeerCache.() -> Unit, apply: () -> Unit) = synchronized(routeLock) {
        peerCache.getOrPut(peerId) { PeerCache() }.update()
        if (peerId != link.status.value.current?.peer?.id) return@synchronized
        if (routedPeer != peerId) switchRemoteTo(peerId) else apply()
    }

    private fun switchRemoteTo(peerId: String) {
        routedPeer = peerId
        remote.onSessionStarted(peerId, resumed = false)
        val c = peerCache[peerId] ?: PeerCache()
        c.snapshot?.let { (s, at) -> remote.onSnapshot(peerId, s, at) }
        c.volume?.let(remote::onPeerVolume)
        c.policy?.let(remote::onPolicyStatus)
        c.sink?.let(remote::onSinkState)
        _pcHandoffApps.value = c.handoff
        MdLog.i(TAG, "player follows ${link.status.value.current?.peer?.name}")
    }

    private fun startPeerFocus() {
        scope.launch {
            link.status.map { it.current?.peer?.id }.distinctUntilChanged().collect { id ->
                synchronized(routeLock) { if (id != null && id != routedPeer) switchRemoteTo(id) }
            }
        }
        // Forget the addresses (and unlock keys) of devices that are no longer paired.
        scope.launch {
            trust.peers.collect { peers ->
                val ids = peers.map { it.id }.toSet()
                settings.forgetPeerEndpoints(ids)
                unlock.store.retainPeers(ids)
            }
        }
    }

    /** Shows [peerId]'s media in the player (null: automatic, a PC first). */
    fun focusPeer(peerId: String?) = link.setFocus(peerId)

    // ---- Handoff (§8.7): continue a song on the other device -----------------------------------

    private val handoffExecutor by lazy { HandoffExecutor(app, media, _localStates) }
    private val handoffMutex = Mutex()
    private val _phoneHandoffApps = MutableStateFlow<List<HandoffApp>>(emptyList())
    /** Music apps on this phone that can take a song from the PC. */
    val phoneHandoffApps: StateFlow<List<HandoffApp>> = _phoneHandoffApps
    private val _pcHandoffApps = MutableStateFlow<List<HandoffApp>>(emptyList())
    /** Music apps on the connected PC that can take a song from this phone (`handoff.apps`). */
    val pcHandoffApps: StateFlow<List<HandoffApp>> = _pcHandoffApps
    private val _handoffBusy = MutableStateFlow(false)
    /** A handoff started here is running (the PC coordinates it). */
    val handoffBusy: StateFlow<Boolean> = _handoffBusy

    private fun refreshHandoffApps() {
        val apps = handoffExecutor.installedApps()
        if (apps == _phoneHandoffApps.value) return
        _phoneHandoffApps.value = apps
        MdLog.i(TAG, "music apps for handoff: ${apps.joinToString { it.id }.ifEmpty { "none" }}")
        link.broadcastHandoffApps()
    }

    /**
     * Moves session [key] of [side] to the other device, in [app] (null: the PC picks). The PC
     * coordinates and answers when the song plays there (or why it could not).
     */
    suspend fun startHandoff(side: Side, key: String, app: HandoffApp?): HandoffOutcome {
        if (!_handoffBusy.compareAndSet(false, true)) return HandoffOutcome(false, null, HandoffErrors.BUSY, null, app, "")
        try {
            val body = buildJsonObject {
                put("from", if (side == Side.PC) "pc" else "phone")
                put("key", key)
                if (app != null) put("app", app.id)
            }
            val answer = link.sendHandoffStart(body) ?: return HandoffOutcome(false, null, HandoffErrors.LINK_LOST, null, app, "")
            return HandoffOutcome.parse(answer).also { MdLog.i(TAG, "handoff to the ${if (side == Side.PC) "phone" else "PC"}: $it") }
        } finally {
            _handoffBusy.value = false
        }
    }

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
        refreshHandoffApps()
        updater.start()
        // Music apps installed / removed: tell the PC which ones can take a song.
        val packages = android.content.IntentFilter().apply {
            addAction(android.content.Intent.ACTION_PACKAGE_ADDED)
            addAction(android.content.Intent.ACTION_PACKAGE_REMOVED)
            addAction(android.content.Intent.ACTION_PACKAGE_REPLACED)
            addDataScheme("package")
        }
        androidx.core.content.ContextCompat.registerReceiver(app, object : android.content.BroadcastReceiver() {
            override fun onReceive(context: Context, intent: android.content.Intent) = refreshHandoffApps()
        }, packages, androidx.core.content.ContextCompat.RECEIVER_NOT_EXPORTED)
        startTargetResolver()
        startLyricsRequests()
        startPeerFocus()
        startPeerArbitration()
        dialer.start()
        lyricsNotifier.start()
        // Work mode: listen for / talk to PCs only when linked.
        scope.launch {
            var first = true
            settings.mode.map { it.linked }.distinctUntilChanged().collect { linked ->
                if (linked) {
                    transports.start()
                    dialer.nudge()
                    // At start-up the service is started by its usual triggers (activity, listener, boot).
                    if (!first) LinkService.start(app, "mode")
                } else {
                    MdLog.i(TAG, "standalone mode: transports off")
                    link.disconnectAll("standalone")
                    transports.stop()
                    LinkService.stop(app)
                    audio.stop(null, "standalone", notifyPeer = false)
                }
                first = false
            }
        }
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
        add(Caps.HANDOFF)
        add(Caps.POLICY_PEER)
        add(Caps.LYRICS_OFFSET)
        add(Caps.UNLOCK)
        if (settings.audioReceiveEnabled.value) add(Caps.AUDIO_RECV_PCM)
    }

    override fun handoffApps(): List<String> = _phoneHandoffApps.value.map { it.id }

    override fun onPeerHandoffApps(peerId: String, apps: JsonElement?) {
        val list = HandoffApp.listFrom(apps)
        MdLog.i(TAG, "PC music apps for handoff: ${list.joinToString { it.id }.ifEmpty { "none" }}")
        routePeer(peerId, { handoff = list }) { _pcHandoffApps.value = list }
    }

    override suspend fun onHandoffPlay(peerId: String, body: JsonObject, progress: suspend (String) -> Unit): JsonObject {
        val req = HandoffPlay.parse(body) ?: return PlayResult.failed(ErrorCode.BAD_MESSAGE).toJson()
        if (!settings.mode.value.linked) return PlayResult.failed(HandoffErrors.UNSUPPORTED).toJson()
        MdLog.i(TAG, "PC hands over \"${req.song.title}\" to ${req.app.id}")
        // One at a time; a second request while one runs is refused.
        if (!handoffMutex.tryLock()) return PlayResult.failed(HandoffErrors.BUSY).toJson()
        return try {
            handoffExecutor.play(req, progress).also {
                MdLog.i(TAG, "handoff result: $it")
                requestSnapshot()
            }.toJson()
        } finally {
            handoffMutex.unlock()
        }
    }

    override fun freshSnapshot(): MediaSnapshot =
        SnapshotBuilder.build(gen.incrementAndGet(), media.states, clock.nowMs(), currentOutputLatency()).also { _lastSentSnapshot.value = it }

    override suspend fun executeCommand(peerId: String, command: MediaCommand): CommandResult {
        noteLocalCommand(command.key, command.action, policy = command.origin == MediaCommand.ORIGIN_POLICY)
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
    override fun onPeerVolume(peerId: String, state: VolumeState) = routePeer(peerId, { volume = state }) { remote.onPeerVolume(state) }
    override fun bluetoothName(): String? = transports.bluetoothName()

    /** Executes a user command on a phone session directly through its MediaController. */
    suspend fun executeLocal(
        key: String?,
        action: String,
        positionMs: Long? = null,
        customAction: String? = null,
        queueId: Long? = null,
    ): CommandResult {
        noteLocalCommand(key, action, policy = false)
        val result = withContext(Dispatchers.Main) {
            media.execute(MediaCommand(key, action, positionMs, MediaCommand.ORIGIN_USER, "android_ui", customAction, queueId))
        }
        requestSnapshot()
        return result
    }

    override fun lanEndpoints(): List<LanEndpoint> = transports.lanEndpoints()

    override fun onSessionStarted(peer: DeviceInfo, resumed: Boolean, peerCaps: Set<String>) = synchronized(routeLock) {
        // §7: a new session discards the peer's state; it re-sends everything.
        if (!resumed) peerCache.remove(peer.id)
        if (peer.id != link.status.value.current?.peer?.id) return@synchronized
        if (routedPeer == peer.id) remote.onSessionStarted(peer.id, resumed) else switchRemoteTo(peer.id)
    }

    override fun onPeerDisconnected(peerId: String) {
        synchronized(routeLock) {
            if (routedPeer == peerId) {
                _pcHandoffApps.value = emptyList()
                remote.onDisconnected(peerId)
            }
        }
        audio.stopForPeer(peerId)
        _peerPolicies.value = _peerPolicies.value - peerId
        onArbiter { onPeerGone(peerId) }
    }

    override fun peerPolicy(): PeerPolicy? = _peerState.value

    override fun onPeerPolicy(peerId: String, policy: PeerPolicy) {
        _peerPolicies.value = _peerPolicies.value + (peerId to policy)
        onArbiter { onPeer(peerId, policy) }
    }

    override suspend fun onUnlockEnroll(peerId: String, msgId: Long, req: UnlockEnroll): JsonObject = unlock.enroll(peerId, msgId, req)
    override suspend fun onUnlockRequest(peerId: String, msgId: Long, req: UnlockRequest): JsonObject = unlock.request(peerId, msgId, req)
    override fun onUnlockCancel(peerId: String, re: Long) = unlock.cancel(peerId, re)
    override fun onUnlockRevoke(peerId: String, enrollment: String) = unlock.revoke(peerId, enrollment)

    /** Settings: this phone no longer unlocks that PC; tells the PC when it is connected. */
    fun removeUnlockPc(enrollment: String) {
        val pc = unlock.store.remove(enrollment) ?: return
        scope.launch { if (!link.sendUnlockRevoke(pc.peerId, enrollment)) MdLog.i(TAG, "${pc.pcName} not connected: it learns at its next unlock") }
    }

    override fun onPeerTransportInfo(peerId: String, endpoints: List<LanEndpoint>) {
        if (endpoints.isNotEmpty()) settings.rememberPeerEndpoints(peerId, endpoints)
    }

    override fun onPeerSnapshot(peerId: String, snapshot: MediaSnapshot, positionAtMs: Long) =
        routePeer(peerId, { this.snapshot = snapshot to positionAtMs }) { remote.onSnapshot(peerId, snapshot, positionAtMs) }
    override fun onBlobChunk(peerId: String, chunk: BlobChunk) = remote.onChunk(chunk)
    override fun onArtworkNotFound(peerId: String, hash: String) = remote.onArtworkNotFound(hash)
    override fun onPeerQueue(peerId: String, queue: MediaQueue) {
        if (peerId == routedPeer) remote.onQueue(queue)
    }
    override fun onLyrics(peerId: String, lyrics: LyricsPush) =
        this.lyrics.onPcLyrics(lyrics.trackId, remote.parseLyrics(lyrics), remote.sessionForTrack(lyrics.trackId)?.let(LyricsRepository::songOf))
    override fun onLyricsNone(peerId: String, trackId: String) =
        lyrics.onPcNone(trackId, remote.sessionForTrack(trackId)?.let(LyricsRepository::songOf))
    // Stored only: a remote change is never sent on (no echo, see setLyricOffset).
    override fun onPeerLyricsOffset(peerId: String, appId: String, offsetMs: Int) = settings.setLyricOffset(appId, offsetMs)
    override fun onPolicyStatus(peerId: String, status: PolicyStatus) = routePeer(peerId, { policy = status }) { remote.onPolicyStatus(status) }
    override fun onSinkState(peerId: String, state: SinkState) = routePeer(peerId, { sink = state }) { remote.onSinkState(state) }
    override fun mediaQueue(key: String): MediaQueue? = media.queues.value[key]?.let { fitRecord(it) }

    /** Shrinks the queue window (keeping the current entry) until it fits one control record. */
    private fun fitRecord(queue: MediaQueue): MediaQueue {
        var q = queue
        while (q.items.size > 20 && q.toJson().toString().toByteArray(Charsets.UTF_8).size > 56_000) {
            val keep = q.items.size * 3 / 4
            val at = q.items.indexOfFirst { it.id == q.activeId }.coerceAtLeast(0)
            val start = (at - 10).coerceIn(0, q.items.size - keep)
            q = q.copy(offset = q.offset + start, items = q.items.subList(start, start + keep))
        }
        return q
    }

    /**
     * "Play on the PC": asks the PC to receive this phone's Bluetooth audio (Windows A2DP sink).
     * Returns false when no PC that supports it is connected.
     */
    suspend fun setPcSpeaker(enable: Boolean): Boolean {
        val sent = link.sendSinkSet(enable)
        if (sent) remote.onSinkRequested(enable)
        return sent
    }
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
