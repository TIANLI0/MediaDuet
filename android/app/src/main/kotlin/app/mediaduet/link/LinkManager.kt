package app.mediaduet.link

import app.mediaduet.crypto.Rand
import app.mediaduet.protocol.ArtworkRef
import app.mediaduet.protocol.AudioOffer
import app.mediaduet.protocol.BadMessageException
import app.mediaduet.protocol.BlobChunk
import app.mediaduet.protocol.Caps
import app.mediaduet.protocol.Channel
import app.mediaduet.protocol.CommandResult
import app.mediaduet.protocol.DeviceInfo
import app.mediaduet.protocol.Envelope
import app.mediaduet.protocol.EnvelopeCodec
import app.mediaduet.protocol.ErrorCode
import app.mediaduet.protocol.ErrorInfo
import app.mediaduet.protocol.FrameException
import app.mediaduet.protocol.FrameType
import app.mediaduet.protocol.Hex
import app.mediaduet.protocol.LanEndpoint
import app.mediaduet.protocol.Limits
import app.mediaduet.protocol.LyricsPush
import app.mediaduet.protocol.MediaCommand
import app.mediaduet.protocol.MediaSnapshot
import app.mediaduet.protocol.MsgType
import app.mediaduet.protocol.PeerPolicy
import app.mediaduet.protocol.PolicyStatus
import app.mediaduet.protocol.SinkState
import app.mediaduet.protocol.MediaQueue
import app.mediaduet.protocol.StrLimit
import app.mediaduet.protocol.VolumeSet
import app.mediaduet.protocol.VolumeState
import app.mediaduet.protocol.truncateCodePoints
import app.mediaduet.protocol.optLong
import app.mediaduet.protocol.optArray
import app.mediaduet.protocol.optString
import app.mediaduet.protocol.requireString
import app.mediaduet.util.MdLog
import app.mediaduet.util.MonoClock
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.merge
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.security.GeneralSecurityException

/** Everything the link layer needs from (and reports to) the rest of the app. */
interface LinkHost {
    fun localDevice(): DeviceInfo
    fun capabilities(): List<String>

    /** A fresh full snapshot with a new `gen` (sent to a newly attached session). */
    fun freshSnapshot(): MediaSnapshot?
    suspend fun executeCommand(peerId: String, command: MediaCommand): CommandResult
    fun artwork(hash: String): ByteArray?
    fun lanEndpoints(): List<LanEndpoint>

    fun onSessionStarted(peer: DeviceInfo, resumed: Boolean, peerCaps: Set<String>)
    fun onPeerDisconnected(peerId: String)
    /** [positionAtMs]: our monotonic clock at which the snapshot's positions hold (§11.1). */
    fun onPeerSnapshot(peerId: String, snapshot: MediaSnapshot, positionAtMs: Long)
    fun onBlobChunk(peerId: String, chunk: BlobChunk)
    fun onArtworkNotFound(peerId: String, hash: String)
    fun onLyrics(peerId: String, lyrics: LyricsPush)
    fun onLyricsNone(peerId: String, trackId: String)
    fun onPolicyStatus(peerId: String, status: PolicyStatus)
    /** The PC's Bluetooth audio receiver changed (`audio.sink.state`). */
    fun onSinkState(peerId: String, state: SinkState) = Unit

    /** The play queue of our session [key] for `media.queue.get`, or null (answered not_found). */
    fun mediaQueue(key: String): MediaQueue? = null
    /** The peer answered our `media.queue.get` (the play queue of one of its sessions). */
    fun onPeerQueue(peerId: String, queue: MediaQueue) = Unit
    suspend fun onAudioOffer(peerId: String, offer: AudioOffer, baseKey: ByteArray): JsonObject
    fun onAudioStop(peerId: String, streamId: String, reason: String)

    /** Our media output volume, or null when unavailable. */
    fun volumeState(): VolumeState?

    /** Applies a peer's `volume.set`; returns the resulting state (answered with `volume.state`). */
    suspend fun applyVolume(peerId: String, request: VolumeSet): VolumeState?
    fun onPeerVolume(peerId: String, state: VolumeState)

    /** Local Bluetooth adapter name for `transport.info.btName` (null without permission). */
    fun bluetoothName(): String?

    /** Handoff (§8.7): our music apps that can take a song (`handoff.apps`, ids). */
    fun handoffApps(): List<String> = emptyList()
    /** The PC's music apps (`handoff.apps`). */
    fun onPeerHandoffApps(peerId: String, apps: kotlinx.serialization.json.JsonElement?) = Unit
    /**
     * `handoff.play`: play the song here; the body of `handoff.result`. [progress] sends
     * `handoff.progress {state}` (e.g. tap_to_continue).
     */
    suspend fun onHandoffPlay(peerId: String, body: JsonObject, progress: suspend (String) -> Unit): JsonObject =
        buildJsonObject {
            put("ok", false)
            put("error", ErrorInfo(ErrorCode.UNSUPPORTED).toJson())
        }

    /** Our arbitration state for Android peers (`policy.peer`, §13.2), null when not arbitrating. */
    fun peerPolicy(): PeerPolicy? = null
    fun onPeerPolicy(peerId: String, policy: PeerPolicy) = Unit
    /** The LAN endpoints of a peer we dialled (`transport.info`), to find it again. */
    fun onPeerTransportInfo(peerId: String, endpoints: List<LanEndpoint>) = Unit
}

data class TransportStatus(
    val connectionId: Int,
    val kind: TransportKind,
    val remote: String,
    val primary: Boolean,
    val connectedAtMs: Long,
)

data class SessionStatus(
    val sid: String,
    val peer: DeviceInfo,
    val peerCaps: Set<String>,
    val transports: List<TransportStatus>,
    val rttMs: Long?,
    val attachedAtMs: Long,
    val resumeCount: Int,
    /** We dialled this peer (another Android device). */
    val outgoing: Boolean = false,
) {
    val connected: Boolean get() = transports.isNotEmpty()
}

data class LinkStatus(
    val sessions: List<SessionStatus> = emptyList(),
    /** The peer the user chose to show in the player ([LinkManager.setFocus]), if any. */
    val focusId: String? = null,
) {
    /**
     * The session the UI focuses on (player, mirror, PC features): the chosen peer when connected,
     * else the most recently attached PC, else the most recently attached device.
     */
    val current: SessionStatus?
        get() {
            val connected = sessions.filter { it.connected }
            return connected.firstOrNull { it.peer.id == focusId }
                ?: connected.filter { it.peer.platform == PLATFORM_WINDOWS }.maxByOrNull { it.attachedAtMs }
                ?: connected.maxByOrNull { it.attachedAtMs }
        }

    val connectedSessions: List<SessionStatus> get() = sessions.filter { it.connected }

    companion object {
        const val PLATFORM_WINDOWS = "windows"
        const val PLATFORM_ANDROID = "android"
    }
}

/**
 * Owns link sessions and connections (§7, §11). All session state is confined to a
 * single-threaded dispatcher; transports only hand over byte streams via [accept].
 */
@OptIn(ExperimentalCoroutinesApi::class)
class LinkManager(
    private val scope: CoroutineScope,
    private val host: LinkHost,
    private val trust: TrustStore,
    val pairing: PairingController,
    private val clock: MonoClock,
    private val wallClock: () -> Long = System::currentTimeMillis,
) : SessionResolver {

    private val dispatcher = Dispatchers.Default.limitedParallelism(1)
    private val sessions = LinkedHashMap<String, LinkSession>()

    val counters = TrafficCounters()

    private val _status = MutableStateFlow(LinkStatus())
    val status: StateFlow<LinkStatus> = _status

    /** True while media plays on either side; shortens the primary heartbeat interval (§11). */
    val anyPlaying = MutableStateFlow(false)

    /** Dispatcher only: the peer the UI focuses on (see [LinkStatus.current]). */
    private var focusId: String? = null

    /** Dispatcher only: the last session id per peer we dialled, offered as `resume` (§7). */
    private val dialledSids = HashMap<String, String>()

    /** Accepts a freshly connected transport stream (called from any thread). */
    fun accept(stream: TransportStream) {
        val conn = Connection(stream, scope, counters, clock)
        MdLog.i(TAG, "accepted $conn")
        conn.start()
        scope.launch(dispatcher) { handle(conn) }
    }

    /**
     * Dials the paired Android device [peerId] over [stream] (client role, §13). Returns the
     * handshake result; on success the session keeps running in the background.
     */
    suspend fun dial(stream: TransportStream, peerId: String): HandshakeResult =
        outgoing(stream) { conn ->
            val r = clientHandshake().session(conn, peerId, dialledSids[peerId])
            if (r is HandshakeResult.Failed && r.reason == ErrorCode.NOT_PAIRED) {
                // §3.4: the peer forgot us; stop reconnecting.
                MdLog.w(TAG, "peer $peerId no longer knows us: removing the pairing")
                trust.remove(peerId)
            }
            r
        }

    /** Asks the Android device on [stream] to pair (it must have pairing mode open, §13). */
    suspend fun pairWith(stream: TransportStream): HandshakeResult =
        outgoing(stream) { conn -> clientHandshake().pair(conn) }

    private suspend fun outgoing(stream: TransportStream, handshake: suspend (Connection) -> HandshakeResult): HandshakeResult {
        val conn = Connection(stream, scope, counters, clock)
        MdLog.i(TAG, "dialled $conn")
        conn.start()
        val result = CompletableDeferred<HandshakeResult>()
        scope.launch(dispatcher) {
            val r = try {
                handshake(conn)
            } catch (e: CancellationException) {
                conn.close("cancelled")
                result.complete(HandshakeResult.Failed("cancelled"))
                throw e
            } catch (e: Exception) {
                MdLog.e(TAG, "$conn client handshake crashed", e)
                conn.close("handshake error")
                HandshakeResult.Failed("internal")
            }
            result.complete(r)
            when (r) {
                is HandshakeResult.Failed -> counters.handshakesFailed.incrementAndGet()
                is HandshakeResult.Paired -> counters.handshakesOk.incrementAndGet()
                is HandshakeResult.Session -> {
                    counters.handshakesOk.incrementAndGet()
                    dialledSids[r.peer.id] = r.sid
                    runSession(conn, r, outgoing = true)
                }
            }
        }
        return result.await()
    }

    private fun clientHandshake() = ClientHandshake(
        localDevice = host::localDevice,
        trust = trust,
        pairing = pairing,
        capabilities = host::capabilities,
        clock = clock,
        wallClock = wallClock,
    )

    /** Chooses the peer the player and the PC features follow (null: automatic). */
    fun setFocus(peerId: String?) {
        scope.launch(dispatcher) {
            focusId = peerId
            publishStatus()
        }
    }

    /** True while a connected session with [peerId] exists. */
    fun isConnected(peerId: String): Boolean = _status.value.sessions.any { it.peer.id == peerId && it.connected }

    // ----------------------------------------------------------------------------------------
    // SessionResolver
    // ----------------------------------------------------------------------------------------

    override fun resolve(resumeSid: String?, peerId: String): Pair<String, Boolean> {
        purgeExpired()
        if (resumeSid != null) {
            val s = sessions[resumeSid]
            if (s != null && s.peer.id == peerId && clock.nowMs() - s.lastActiveMs < RESUME_WINDOW_MS) {
                return resumeSid to true
            }
        }
        return Hex.encode(Rand.bytes(16)) to false
    }

    private fun purgeExpired() {
        val now = clock.nowMs()
        val it = sessions.values.iterator()
        while (it.hasNext()) {
            val s = it.next()
            if (!s.isConnected && now - s.lastActiveMs >= RESUME_WINDOW_MS) {
                failPending(s, ErrorCode.LINK_LOST)
                it.remove()
            }
        }
    }

    // ----------------------------------------------------------------------------------------
    // Connection lifecycle
    // ----------------------------------------------------------------------------------------

    private suspend fun handle(conn: Connection) {
        val handshake = ServerHandshake(
            localDevice = host::localDevice,
            trust = trust,
            pairing = pairing,
            sessions = this,
            capabilities = host::capabilities,
            clock = clock,
            wallClock = wallClock,
        )
        val result = try {
            handshake.run(conn)
        } catch (e: CancellationException) {
            conn.close("cancelled")
            throw e
        } catch (e: Exception) {
            MdLog.e(TAG, "$conn handshake crashed", e)
            conn.close("handshake error")
            HandshakeResult.Failed("internal")
        }
        when (result) {
            is HandshakeResult.Failed -> counters.handshakesFailed.incrementAndGet()
            is HandshakeResult.Paired -> counters.handshakesOk.incrementAndGet()
            is HandshakeResult.Session -> {
                counters.handshakesOk.incrementAndGet()
                runSession(conn, result)
            }
        }
    }

    private suspend fun runSession(conn: Connection, hs: HandshakeResult.Session, outgoing: Boolean = false) {
        val now = clock.nowMs()
        val existing = sessions[hs.sid]
        val session: LinkSession
        if (hs.resumed && existing != null) {
            session = existing
            session.peer = hs.peer
            session.peerCaps = hs.peerCaps
            session.resumeCount++
        } else {
            sessions[hs.sid]?.let { failPending(it, ErrorCode.LINK_LOST) }
            // A new session supersedes older sessions of the same peer.
            sessions.values.filter { it.peer.id == hs.peer.id && it.sid != hs.sid }.forEach { old ->
                failPending(old, ErrorCode.LINK_LOST)
                old.connections.forEach { it.closeAfterFlush("superseded by new session") }
            }
            sessions.entries.removeAll { it.value.peer.id == hs.peer.id && it.key != hs.sid }
            session = LinkSession(hs.sid, hs.peer, hs.version, hs.peerCaps, now, outgoing)
            sessions[hs.sid] = session
        }
        session.connections.add(conn)
        session.lastActiveMs = now
        session.attachedAtMs = now
        if (session.primary?.isClosed != false) session.setPrimary(conn)
        trust.updateSeen(hs.peer.id, hs.peer.name, wallClock(), hs.peer.app)
        MdLog.i(TAG, "session ${hs.sid.take(8)} ${if (hs.resumed) "resumed" else "new"} with ${hs.peer.name} via $conn")
        publishStatus()
        host.onSessionStarted(hs.peer, hs.resumed, hs.peerCaps)

        if (outgoing) {
            // As the client we designate the primary transport (§7); the server greets with session.ready.
            send(session, conn, MsgType.LINK_PRIMARY, buildJsonObject { })
        } else {
            send(session, conn, MsgType.SESSION_READY, buildJsonObject { put("sid", session.sid) })
            sendTransportInfo(session, conn)
        }
        host.freshSnapshot()?.let { send(session, conn, MsgType.MEDIA_SNAPSHOT, it.toJson()) }
        if (Caps.VOLUME in session.peerCaps) host.volumeState()?.let { send(session, conn, MsgType.VOLUME_STATE, it.toJson()) }
        if (Caps.HANDOFF in session.peerCaps) send(session, conn, MsgType.HANDOFF_APPS, handoffAppsBody())
        if (Caps.POLICY_PEER in session.peerCaps) host.peerPolicy()?.let { send(session, conn, MsgType.POLICY_PEER, it.toJson()) }

        val heartbeat = scope.launch(dispatcher) { heartbeat(session, conn) }
        try {
            for (frame in conn.frames) {
                if (frame.type != FrameType.SECURE) {
                    counters.protocolErrors.incrementAndGet()
                    conn.close("handshake frame after keys")
                    break
                }
                val (channel, data) = try {
                    conn.openRecord(frame)
                } catch (e: GeneralSecurityException) {
                    counters.protocolErrors.incrementAndGet()
                    conn.close("record authentication failed")
                    break
                } catch (e: FrameException) {
                    counters.protocolErrors.incrementAndGet()
                    conn.close("bad record: ${e.message}")
                    break
                }
                when (channel) {
                    Channel.CONTROL -> onControl(session, conn, data)
                    Channel.BLOB -> try {
                        counters.blobChunksIn.incrementAndGet()
                        session.lastActiveMs = clock.nowMs()
                        host.onBlobChunk(session.peer.id, BlobChunk.decode(data))
                    } catch (e: FrameException) {
                        counters.protocolErrors.incrementAndGet()
                        MdLog.w(TAG, "dropping malformed blob chunk: ${e.message}")
                    }
                    else -> MdLog.d(TAG, "ignoring record on unknown channel $channel")
                }
            }
        } finally {
            heartbeat.cancel()
            conn.close("session loop ended")
            withContext(kotlinx.coroutines.NonCancellable) { detach(session, conn) }
        }
    }

    private fun detach(session: LinkSession, conn: Connection) {
        session.connections.remove(conn)
        session.lastActiveMs = clock.nowMs()
        if (session.primary === conn) session.setPrimary(session.connections.lastOrNull { !it.isClosed })
        // Re-send or fail commands that were in flight on this transport (§7).
        val now = clock.nowMs()
        val replacement = session.primaryOrAny()
        for ((id, p) in session.pending.entries.toList()) {
            if (p.connection !== conn) continue
            if (replacement != null && now - p.sentAtMs < RESEND_WINDOW_MS) {
                p.connection = replacement
                replacement.sendControl(p.envelope)
            } else {
                session.pending.remove(id)
                p.result.complete(CommandResult.failure(ErrorCode.LINK_LOST))
            }
        }
        if (!session.isConnected) {
            MdLog.i(TAG, "session ${session.sid.take(8)} has no transports left")
            host.onPeerDisconnected(session.peer.id)
        }
        publishStatus()
    }

    private fun failPending(session: LinkSession, code: String) {
        handoffWaits.keys.filter { it.first == session.sid }.forEach { k ->
            handoffWaits.remove(k)?.complete(buildJsonObject {
                put("ok", false)
                put("error", ErrorInfo(code).toJson())
            })
        }
        session.pending.values.forEach { it.result.complete(CommandResult.failure(code)) }
        session.pending.clear()
    }

    // ----------------------------------------------------------------------------------------
    // Inbound control messages
    // ----------------------------------------------------------------------------------------

    private fun onControl(session: LinkSession, conn: Connection, data: ByteArray) {
        val env = try {
            EnvelopeCodec.decode(data, Limits.MAX_CONTROL_DATA)
        } catch (e: BadMessageException) {
            counters.protocolErrors.incrementAndGet()
            MdLog.w(TAG, "bad message: ${e.message}")
            send(session, conn, MsgType.ERROR, ErrorInfo(e.code, e.message).toJson(), e.re)
            return
        }
        if (env.v != session.version || env.src != session.peer.id || env.sid != session.sid) {
            counters.protocolErrors.incrementAndGet()
            send(session, conn, MsgType.ERROR, ErrorInfo(ErrorCode.BAD_MESSAGE, "envelope mismatch").toJson(), env.id)
            return
        }
        if (!session.dedup.accept(env.id)) {
            if (env.type == MsgType.MEDIA_COMMAND) {
                val cached = session.cachedResult(env.id)
                if (cached != null) {
                    send(session, conn, MsgType.MEDIA_COMMAND_RESULT, cached, env.id)
                } else {
                    session.inFlight[env.id]?.let { waiting -> if (conn !in waiting) waiting.add(conn) }
                }
            }
            MdLog.d(TAG, "duplicate ${env.type} id=${env.id} ignored")
            return
        }
        counters.messagesIn.incrementAndGet()
        session.lastActiveMs = clock.nowMs()
        try {
            dispatch(session, conn, env)
        } catch (e: BadMessageException) {
            counters.protocolErrors.incrementAndGet()
            MdLog.w(TAG, "bad ${env.type}: ${e.message}")
            send(session, conn, MsgType.ERROR, ErrorInfo(e.code, e.message).toJson(), env.id)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            // A bug in one handler must never take the link (or the process) down.
            MdLog.e(TAG, "handler for ${env.type} failed", e)
            send(session, conn, MsgType.ERROR, ErrorInfo(ErrorCode.INTERNAL).toJson(), env.id)
        }
    }

    private fun dispatch(session: LinkSession, conn: Connection, env: Envelope) {
        val peerId = session.peer.id
        when (env.type) {
            MsgType.PING -> {
                val t = env.body.optLong("t")
                // rt: our clock as we answer, for the peer's clock estimate (§11.1).
                send(session, conn, MsgType.PONG, buildJsonObject {
                    if (t != null) put("t", t)
                    put("rt", clock.nowMs())
                }, env.id)
            }
            MsgType.PONG -> {
                val t = env.body.optLong("t") ?: return
                val now = clock.nowMs()
                val rtt = now - t
                if (rtt in 0..120_000) {
                    session.rttMs = rtt
                    val rt = try {
                        env.body.optLong("rt")
                    } catch (e: BadMessageException) {
                        null // odd peer: RTT only
                    }
                    if (rt != null) session.clockSync.addSample(t, rt, now)
                    publishStatus()
                }
            }
            MsgType.SESSION_READY -> MdLog.d(TAG, "session ${session.sid.take(8)} ready")
            MsgType.TRANSPORT_INFO -> if (session.outgoing) host.onPeerTransportInfo(peerId, parseLanEndpoints(env.body))
            MsgType.POLICY_PEER -> host.onPeerPolicy(peerId, PeerPolicy.parse(env.body))
            MsgType.LINK_PRIMARY -> {
                session.setPrimary(conn)
                MdLog.i(TAG, "primary transport is now $conn")
                publishStatus()
            }
            MsgType.DEVICE_INFO -> {
                val info = DeviceInfo.parse(env.body)
                if (info.id != peerId) throw BadMessageException("device.info id mismatch")
                session.peer = info
                trust.updateSeen(peerId, info.name, wallClock(), info.app)
                publishStatus()
            }
            MsgType.MEDIA_SNAPSHOT -> {
                val snapshot = MediaSnapshot.parse(env.body)
                if (snapshot.gen <= session.lastPeerGen) return
                session.lastPeerGen = snapshot.gen
                // Positions hold at the PC's `at` on our clock; older PCs: now − RTT/2.
                val anchor = session.clockSync.anchor(snapshot.atMs, clock.nowMs(), (session.rttMs ?: 0) / 2)
                host.onPeerSnapshot(peerId, snapshot, anchor)
            }
            MsgType.MEDIA_COMMAND -> {
                val command = MediaCommand.parse(env.body)
                if (Caps.MEDIA_CONTROL !in host.capabilities()) {
                    val r = CommandResult.failure(ErrorCode.UNSUPPORTED)
                    session.cacheResult(env.id, r.toJson())
                    send(session, conn, MsgType.MEDIA_COMMAND_RESULT, r.toJson(), env.id)
                    return
                }
                session.inFlight[env.id] = mutableListOf(conn)
                scope.launch(dispatcher) {
                    val result = try {
                        host.executeCommand(peerId, command)
                    } catch (e: CancellationException) {
                        throw e
                    } catch (e: Exception) {
                        MdLog.e(TAG, "command execution failed", e)
                        CommandResult.failure(ErrorCode.INTERNAL, e.javaClass.simpleName)
                    }
                    val body = result.toJson()
                    session.cacheResult(env.id, body)
                    val waiting = session.inFlight.remove(env.id).orEmpty().filter { !it.isClosed }
                    val targets = waiting.ifEmpty { listOfNotNull(session.primaryOrAny()) }
                    targets.forEach { send(session, it, MsgType.MEDIA_COMMAND_RESULT, body, env.id) }
                }
            }
            MsgType.MEDIA_COMMAND_RESULT -> {
                val re = env.re ?: throw BadMessageException("result without re")
                val result = CommandResult.parse(env.body)
                session.pending.remove(re)?.result?.complete(result)
            }
            MsgType.ARTWORK_GET -> {
                val hash = env.body.requireString("hash", 64)
                if (!Hex.isLowerHex(hash, 64)) throw BadMessageException("bad hash")
                val bytes = host.artwork(hash)
                if (bytes == null) {
                    send(session, conn, MsgType.ERROR, ErrorInfo(ErrorCode.NOT_FOUND).toJson(), env.id)
                    return
                }
                val digest = Hex.decode(hash)
                scope.launch {
                    for (chunk in BlobChunk.split(digest, bytes)) {
                        if (!conn.sendBlob(chunk.encode())) break
                    }
                }
            }
            MsgType.LYRICS_PUSH -> host.onLyrics(peerId, LyricsPush.parse(env.body))
            MsgType.LYRICS_NONE -> host.onLyricsNone(peerId, env.body.requireString("trackId", 64))
            MsgType.POLICY_STATUS -> host.onPolicyStatus(peerId, PolicyStatus.parse(env.body))
            MsgType.AUDIO_SINK_STATE -> host.onSinkState(peerId, SinkState.parse(env.body))
            MsgType.MEDIA_QUEUE -> host.onPeerQueue(peerId, MediaQueue.parse(env.body))
            MsgType.MEDIA_QUEUE_GET -> {
                val queue = host.mediaQueue(env.body.requireString("key", StrLimit.SESSION_KEY))
                if (queue == null) send(session, conn, MsgType.ERROR, ErrorInfo(ErrorCode.NOT_FOUND).toJson(), env.id)
                else send(session, conn, MsgType.MEDIA_QUEUE, queue.toJson(), env.id)
            }
            MsgType.AUDIO_OFFER -> {
                val offer = AudioOffer.parse(env.body)
                val base = conn.sessionKeys?.keyC2s ?: throw BadMessageException("no keys")
                scope.launch(dispatcher) {
                    val answer = try {
                        host.onAudioOffer(peerId, offer, base)
                    } catch (e: CancellationException) {
                        throw e
                    } catch (e: Exception) {
                        MdLog.e(TAG, "audio offer failed", e)
                        buildJsonObject {
                            put("ok", false)
                            put("error", ErrorInfo(ErrorCode.INTERNAL).toJson())
                        }
                    }
                    send(session, conn.takeIf { !it.isClosed }, MsgType.AUDIO_ANSWER, answer, env.id)
                }
            }
            MsgType.AUDIO_STOP -> host.onAudioStop(
                peerId,
                env.body.requireString("streamId", 64),
                env.body.optString("reason", 128) ?: "",
            )
            MsgType.ERROR -> {
                val error = ErrorInfo.parse(env.body)
                val re = env.re
                MdLog.w(TAG, "peer error ${error.code} re=$re ${error.message ?: ""}")
                if (re != null) {
                    session.pending.remove(re)?.result?.complete(CommandResult(false, error))
                    session.artworkRequests.remove(re)?.let { host.onArtworkNotFound(peerId, it) }
                }
            }
            MsgType.VOLUME_STATE -> host.onPeerVolume(peerId, VolumeState.parse(env.body))
            MsgType.VOLUME_SET -> {
                val request = VolumeSet.parse(env.body)
                // Android has no per-app volume: a session-scoped request is unsupported here.
                if (Caps.VOLUME !in host.capabilities() || request.key != null) {
                    send(session, conn, MsgType.ERROR, ErrorInfo(ErrorCode.UNSUPPORTED).toJson(), env.id)
                    return
                }
                scope.launch(dispatcher) {
                    val state = try {
                        host.applyVolume(peerId, request)
                    } catch (e: CancellationException) {
                        throw e
                    } catch (e: VolumeRestrictedException) {
                        // The OS ignored the change: say so, and report the real (unchanged) state.
                        send(session, conn.takeIf { !it.isClosed }, MsgType.ERROR, ErrorInfo(ErrorCode.BACKGROUND_RESTRICTED, e.message).toJson(), env.id)
                        send(session, conn.takeIf { !it.isClosed }, MsgType.VOLUME_STATE, e.state.toJson())
                        return@launch
                    } catch (e: Exception) {
                        MdLog.e(TAG, "volume.set failed", e)
                        null
                    }
                    if (state == null) {
                        send(session, conn.takeIf { !it.isClosed }, MsgType.ERROR, ErrorInfo(ErrorCode.UNSUPPORTED).toJson(), env.id)
                    } else {
                        send(session, conn.takeIf { !it.isClosed }, MsgType.VOLUME_STATE, state.toJson(), env.id)
                    }
                }
            }
            MsgType.HANDOFF_APPS -> host.onPeerHandoffApps(peerId, env.body["apps"])
            MsgType.HANDOFF_PLAY -> {
                if (Caps.HANDOFF !in host.capabilities()) {
                    send(session, conn, MsgType.HANDOFF_RESULT, unsupportedResult(), env.id)
                    return
                }
                scope.launch(dispatcher) {
                    val result = try {
                        host.onHandoffPlay(peerId, env.body) { state ->
                            withContext(dispatcher) {
                                send(session, null, MsgType.HANDOFF_PROGRESS, buildJsonObject { put("state", state) }, env.id)
                            }
                        }
                    } catch (e: CancellationException) {
                        throw e
                    } catch (e: Exception) {
                        MdLog.e(TAG, "handoff failed", e)
                        buildJsonObject {
                            put("ok", false)
                            put("error", ErrorInfo(ErrorCode.INTERNAL).toJson())
                        }
                    }
                    send(session, null, MsgType.HANDOFF_RESULT, result, env.id)
                }
            }
            MsgType.HANDOFF_RESULT -> {
                val re = env.re ?: throw BadMessageException("result without re")
                handoffWaits.remove(session.sid to re)?.complete(env.body)
            }
            // Only the PC coordinates a handoff.
            MsgType.HANDOFF_START -> send(session, conn, MsgType.HANDOFF_RESULT, unsupportedResult(), env.id)
            MsgType.BYE -> {
                MdLog.i(TAG, "peer said bye: ${env.body.optString("reason", 128)}")
                session.connections.toList().forEach { it.closeAfterFlush("peer bye") }
            }
            else -> MdLog.d(TAG, "ignoring message type ${env.type}")
        }
    }

    // ----------------------------------------------------------------------------------------
    // Outbound
    // ----------------------------------------------------------------------------------------

    private fun send(session: LinkSession, conn: Connection?, type: String, body: JsonObject, re: Long? = null): Envelope? {
        val target = conn?.takeIf { !it.isClosed } ?: session.primaryOrAny() ?: return null
        val env = Envelope(session.version, type, session.nextId(), wallClock(), host.localDevice().id, session.sid, re, body)
        target.sendControl(env)
        return env
    }

    private fun sendTransportInfo(session: LinkSession, conn: Connection?) {
        val body = buildJsonObject {
            put("lan", JsonArray(host.lanEndpoints().map { e ->
                buildJsonObject {
                    put("host", e.host)
                    put("port", e.port)
                }
            }))
            host.bluetoothName()?.takeIf { it.isNotBlank() }?.let { put("btName", it.truncateCodePoints(StrLimit.DEVICE_NAME)) }
        }
        send(session, conn, MsgType.TRANSPORT_INFO, body)
    }

    /** Sends a snapshot to every connected session (on its primary transport). */
    fun broadcastSnapshot(snapshot: MediaSnapshot) {
        scope.launch(dispatcher) {
            sessions.values.filter { it.isConnected }.forEach { send(it, null, MsgType.MEDIA_SNAPSHOT, snapshot.toJson()) }
        }
    }

    /** Sends our arbitration state to every connected Android peer that announced `policy.peer`. */
    fun broadcastPeerPolicy(policy: PeerPolicy) {
        scope.launch(dispatcher) {
            sessions.values.filter { it.isConnected && Caps.POLICY_PEER in it.peerCaps }
                .forEach { send(it, null, MsgType.POLICY_PEER, policy.toJson()) }
        }
    }

    /** Sends our volume to every connected peer that announced `volume`. */
    fun broadcastVolume(state: VolumeState) {
        scope.launch(dispatcher) {
            sessions.values.filter { it.isConnected && Caps.VOLUME in it.peerCaps }
                .forEach { send(it, null, MsgType.VOLUME_STATE, state.toJson()) }
        }
    }

    /** Asks the focused peer to change its volume; false when it does not support `volume`. */
    suspend fun sendVolumeSet(request: VolumeSet): Boolean = withContext(dispatcher) {
        val session = currentSession() ?: return@withContext false
        if (Caps.VOLUME !in session.peerCaps) return@withContext false
        send(session, null, MsgType.VOLUME_SET, request.toJson()) != null
    }

    /** Re-announces LAN endpoints after a network change. */
    fun broadcastTransportInfo() {
        scope.launch(dispatcher) { sessions.values.filter { it.isConnected }.forEach { sendTransportInfo(it, null) } }
    }

    /** Re-sends our device info (e.g. after a rename). */
    fun broadcastDeviceInfo() {
        scope.launch(dispatcher) {
            sessions.values.filter { it.isConnected }.forEach {
                send(it, null, MsgType.DEVICE_INFO, host.localDevice().toJson())
            }
        }
    }

    private fun currentSession(): LinkSession? =
        _status.value.current?.let { sessions[it.sid] }?.takeIf { it.isConnected }

    private sealed interface Prepared {
        class Immediate(val result: CommandResult) : Prepared
        class Waiting(val session: LinkSession, val id: Long, val result: CompletableDeferred<CommandResult>) : Prepared
    }

    /** Sends a media command to the connected peer and waits up to 5 s for its result (§8.4). */
    suspend fun sendCommand(command: MediaCommand): CommandResult {
        val prepared: Prepared = withContext(dispatcher) {
            val session = currentSession() ?: return@withContext Prepared.Immediate(CommandResult.failure(ErrorCode.NO_SESSION))
            if (Caps.MEDIA_CONTROL !in session.peerCaps) {
                return@withContext Prepared.Immediate(CommandResult.failure(ErrorCode.UNSUPPORTED))
            }
            val conn = session.primaryOrAny() ?: return@withContext Prepared.Immediate(CommandResult.failure(ErrorCode.LINK_LOST))
            val env = Envelope(
                session.version, MsgType.MEDIA_COMMAND, session.nextId(), wallClock(),
                host.localDevice().id, session.sid, null, command.toJson(),
            )
            val d = CompletableDeferred<CommandResult>()
            session.pending[env.id] = LinkSession.PendingCommand(env, clock.nowMs(), conn, d)
            conn.sendControl(env)
            Prepared.Waiting(session, env.id, d)
        }
        return when (prepared) {
            is Prepared.Immediate -> prepared.result
            is Prepared.Waiting -> withTimeoutOrNull(COMMAND_TIMEOUT_MS) { prepared.result.await() }
                ?: run {
                    withContext(dispatcher) { prepared.session.pending.remove(prepared.id) }
                    CommandResult.failure(ErrorCode.TIMEOUT)
                }
        }
    }

    /** Requests artwork by hash from the current peer if it announced `artwork`. */
    fun requestArtwork(peerId: String, ref: ArtworkRef) {
        scope.launch(dispatcher) {
            val session = sessions.values.firstOrNull { it.peer.id == peerId && it.isConnected } ?: return@launch
            if (Caps.ARTWORK !in session.peerCaps) return@launch
            val env = send(session, null, MsgType.ARTWORK_GET, buildJsonObject { put("hash", ref.hash) }) ?: return@launch
            session.artworkRequests[env.id] = ref.hash
            if (session.artworkRequests.size > 64) session.artworkRequests.remove(session.artworkRequests.keys.min())
        }
    }

    /** Asks the peer for the play queue of its session [key] (`media.queue.get`); the answer comes as `media.queue`. */
    fun requestQueue(peerId: String, key: String) {
        scope.launch(dispatcher) {
            val session = sessions.values.firstOrNull { it.peer.id == peerId && it.isConnected } ?: return@launch
            send(session, null, MsgType.MEDIA_QUEUE_GET, buildJsonObject { put("key", key) })
        }
    }

    /**
     * Asks the PC to start / stop playing this phone's Bluetooth audio (§8.2 audio.sink.set); the
     * PC answers with `audio.sink.state`.
     */
    suspend fun sendSinkSet(enable: Boolean): Boolean = withContext(dispatcher) {
        val session = currentSession() ?: return@withContext false
        if (Caps.AUDIO_A2DP_SINK !in session.peerCaps) return@withContext false
        send(session, null, MsgType.AUDIO_SINK_SET, buildJsonObject { put("enable", enable) }) != null
    }

    /**
     * Lyrics the phone found itself for [trackId] (a phone or PC track), for the PC to show and
     * cache (`lyrics.push`, §8.2). Only when the PC takes lyrics and the record fits in one frame.
     */
    suspend fun sendLyrics(trackId: String, lrc: String, translation: String?, source: String): Boolean = withContext(dispatcher) {
        val session = currentSession() ?: return@withContext false
        if (Caps.LYRICS !in session.peerCaps) return@withContext false
        val body = buildJsonObject {
            put("trackId", trackId)
            put("format", "lrc")
            put("source", source.take(StrLimit.MISC))
            put("text", lrc)
            if (!translation.isNullOrBlank()) put("translation", translation)
        }
        if (body.toString().toByteArray(Charsets.UTF_8).size > MAX_LYRICS_RECORD) return@withContext false
        send(session, null, MsgType.LYRICS_PUSH, body) != null
    }

    private fun handoffAppsBody() = buildJsonObject { put("apps", JsonArray(host.handoffApps().map { kotlinx.serialization.json.JsonPrimitive(it) })) }

    private fun unsupportedResult() = buildJsonObject {
        put("ok", false)
        put("error", ErrorInfo(ErrorCode.UNSUPPORTED).toJson())
    }

    /** Handoff answers we wait for, by (session id, `handoff.start` id). Dispatcher only. */
    private val handoffWaits = HashMap<Pair<String, Long>, CompletableDeferred<JsonObject>>()

    /** Re-announces our music apps (`handoff.apps`) after one was installed or removed. */
    fun broadcastHandoffApps() {
        scope.launch(dispatcher) {
            sessions.values.filter { it.isConnected && Caps.HANDOFF in it.peerCaps }
                .forEach { send(it, null, MsgType.HANDOFF_APPS, handoffAppsBody()) }
        }
    }

    /**
     * Asks the PC to move a session to the other device (`handoff.start`, §8.7; the PC
     * coordinates both directions). Returns the PC's `handoff.result` body, null when no PC with
     * `handoff` is connected or it did not answer in time.
     */
    suspend fun sendHandoffStart(body: JsonObject): JsonObject? {
        val wait = withContext(dispatcher) {
            val session = currentSession() ?: return@withContext null
            if (Caps.HANDOFF !in session.peerCaps) return@withContext null
            val env = send(session, null, MsgType.HANDOFF_START, body) ?: return@withContext null
            CompletableDeferred<JsonObject>().also { handoffWaits[session.sid to env.id] = it } to (session.sid to env.id)
        } ?: return null
        val (deferred, key) = wait
        return withTimeoutOrNull(HANDOFF_TIMEOUT_MS) { deferred.await() }.also {
            withContext(dispatcher) { handoffWaits.remove(key) }
        }
    }

    /** Asks the Windows arbitration engine to change mode (§8.2 policy.config). */
    suspend fun sendPolicyConfig(mode: String): Boolean = withContext(dispatcher) {
        val session = currentSession() ?: return@withContext false
        if (Caps.POLICY_HOST !in session.peerCaps) return@withContext false
        send(session, null, MsgType.POLICY_CONFIG, buildJsonObject { put("mode", mode) }) != null
    }

    fun sendAudioStop(peerId: String, streamId: String, reason: String) {
        scope.launch(dispatcher) {
            val session = sessions.values.firstOrNull { it.peer.id == peerId && it.isConnected } ?: return@launch
            send(session, null, MsgType.AUDIO_STOP, buildJsonObject {
                put("streamId", streamId)
                put("reason", reason)
            })
        }
    }

    /** Orderly disconnect of every session (`bye`). Sessions stay resumable. */
    fun disconnectAll(reason: String) {
        scope.launch(dispatcher) {
            sessions.values.forEach { s ->
                s.connections.toList().forEach { c ->
                    send(s, c, MsgType.BYE, buildJsonObject { put("reason", reason) })
                    c.closeAfterFlush("bye: $reason")
                }
            }
        }
    }

    /** Revocation (§3.4): forget K and drop the peer's sessions. */
    fun revoke(peerId: String) {
        trust.remove(peerId)
        scope.launch(dispatcher) {
            sessions.values.filter { it.peer.id == peerId }.forEach { s ->
                failPending(s, ErrorCode.NOT_PAIRED)
                s.connections.toList().forEach { c ->
                    send(s, c, MsgType.BYE, buildJsonObject { put("reason", "unpaired") })
                    c.closeAfterFlush("revoked")
                }
            }
            sessions.entries.removeAll { it.value.peer.id == peerId }
            publishStatus()
        }
    }

    /** The capabilities announced by the currently focused peer. */
    fun currentPeerCaps(): Set<String> = _status.value.current?.peerCaps ?: emptySet()

    // ----------------------------------------------------------------------------------------
    // Heartbeat (§11)
    // ----------------------------------------------------------------------------------------

    private suspend fun heartbeat(session: LinkSession, conn: Connection) {
        var lastPingSent = clock.nowMs()
        var burstLeft = 0
        var lastEpoch = -1
        var wasPlaying = false
        while (!conn.isClosed) {
            val primary = session.primaryOrAny() === conn
            val playing = anyPlaying.value
            // A few quick pings for the clock estimate (§11.1) when this becomes the primary and when
            // media starts; liveness below still uses the regular interval.
            if (primary && (session.primaryEpoch.value != lastEpoch || (playing && !wasPlaying))) burstLeft = ClockSync.BURST_PINGS
            if (!primary) burstLeft = 0
            lastEpoch = session.primaryEpoch.value
            wasPlaying = playing
            val interval = when {
                !primary -> SECONDARY_INTERVAL_MS
                playing -> PLAYING_INTERVAL_MS
                else -> IDLE_INTERVAL_MS
            }
            val now = clock.nowMs()
            val deadline = conn.lastInboundMs + 3 * interval + 5_000
            if (now >= deadline) {
                MdLog.w(TAG, "$conn heartbeat timeout")
                conn.close("heartbeat timeout")
                return
            }
            if (now - lastPingSent >= if (burstLeft > 0) ClockSync.BURST_GAP_MS else interval) {
                send(session, conn, MsgType.PING, buildJsonObject { put("t", now) })
                lastPingSent = now
                if (burstLeft > 0) burstLeft--
            }
            val gap = if (burstLeft > 0) ClockSync.BURST_GAP_MS else interval
            val wait = (minOf(lastPingSent + gap, deadline) - clock.nowMs()).coerceAtLeast(100)
            withTimeoutOrNull(wait) {
                merge(anyPlaying.drop(1).map { }, session.primaryEpoch.drop(1).map { }).first()
            }
        }
    }

    private fun publishStatus() {
        _status.value = LinkStatus(
            sessions.values.map { s ->
                SessionStatus(
                    sid = s.sid,
                    peer = s.peer,
                    peerCaps = s.peerCaps,
                    transports = s.connections.filter { !it.isClosed }.map { c ->
                        TransportStatus(c.id, c.kind, c.stream.remoteDescription, s.primaryOrAny() === c, c.connectedAtMs)
                    },
                    rttMs = s.rttMs,
                    attachedAtMs = s.attachedAtMs,
                    resumeCount = s.resumeCount,
                    outgoing = s.outgoing,
                )
            },
            focusId,
        )
    }

    /** `transport.info.lan` of a peer we dialled; malformed entries are skipped. */
    private fun parseLanEndpoints(body: JsonObject): List<LanEndpoint> =
        body.optArray("lan").orEmpty().mapNotNull { e ->
            val o = e as? JsonObject ?: return@mapNotNull null
            val host = runCatching { o.optString("host", 64) }.getOrNull()?.takeIf { it.isNotBlank() } ?: return@mapNotNull null
            val port = runCatching { o.optLong("port") }.getOrNull()?.takeIf { it in 1..65535 } ?: return@mapNotNull null
            LanEndpoint(host, port.toInt())
        }.take(8)

    companion object {
        /** Lyrics records above this stay off the link (a control record is at most 64 KiB). */
        private const val MAX_LYRICS_RECORD = 60_000
        private const val TAG = "MD/Link"
        const val RESUME_WINDOW_MS = 10 * 60 * 1000L
        const val RESEND_WINDOW_MS = 5_000L
        const val COMMAND_TIMEOUT_MS = 5_000L
        /** The PC gives a handoff 120 s (a tap on the phone may be needed); a little more here. */
        const val HANDOFF_TIMEOUT_MS = 130_000L
        const val PLAYING_INTERVAL_MS = 15_000L
        const val IDLE_INTERVAL_MS = 60_000L
        const val SECONDARY_INTERVAL_MS = 120_000L
    }
}

/** Thrown by [LinkHost.applyVolume] when the OS silently ignored the change; [state] is the real state. */
class VolumeRestrictedException(val state: VolumeState) : Exception("volume control is restricted while MediaDuet is in the background")
