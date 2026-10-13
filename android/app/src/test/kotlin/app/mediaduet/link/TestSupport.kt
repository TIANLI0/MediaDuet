package app.mediaduet.link

import app.mediaduet.crypto.KeySchedule
import app.mediaduet.crypto.P256
import app.mediaduet.crypto.Rand
import app.mediaduet.crypto.RecordCipher
import app.mediaduet.protocol.ArtworkRef
import app.mediaduet.protocol.AudioOffer
import app.mediaduet.protocol.B64
import app.mediaduet.protocol.BlobChunk
import app.mediaduet.protocol.Channel
import app.mediaduet.protocol.CommandResult
import app.mediaduet.protocol.DeviceInfo
import app.mediaduet.protocol.Envelope
import app.mediaduet.protocol.EnvelopeCodec
import app.mediaduet.protocol.Frame
import app.mediaduet.protocol.FrameCodec
import app.mediaduet.protocol.FrameDecoder
import app.mediaduet.protocol.FrameType
import app.mediaduet.protocol.LanEndpoint
import app.mediaduet.protocol.LyricsPush
import app.mediaduet.protocol.MediaCommand
import app.mediaduet.protocol.MediaSnapshot
import app.mediaduet.protocol.PolicyStatus
import app.mediaduet.protocol.VolumeSet
import app.mediaduet.protocol.VolumeState
import app.mediaduet.protocol.requireBase64
import app.mediaduet.transport.SocketTransportStream
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger

class InMemoryTrustStore : TrustStore {
    private val keys = HashMap<String, ByteArray>()
    private val _peers = MutableStateFlow<List<TrustedPeer>>(emptyList())
    override val peers: StateFlow<List<TrustedPeer>> = _peers

    @Synchronized override fun key(peerId: String): ByteArray? = keys[peerId]?.copyOf()

    @Synchronized override fun add(peer: TrustedPeer, key: ByteArray) {
        keys[peer.id] = key.copyOf()
        _peers.value = _peers.value.filter { it.id != peer.id } + peer
    }

    @Synchronized override fun remove(peerId: String) {
        keys.remove(peerId)
        _peers.value = _peers.value.filter { it.id != peerId }
    }

    @Synchronized override fun updateSeen(peerId: String, name: String, seenAtMs: Long) {
        _peers.value = _peers.value.map { if (it.id == peerId) it.copy(name = name, lastSeenMs = seenAtMs) else it }
    }
}

/** Records everything the link layer reports; executes commands by counting them. */
class FakeHost(val device: DeviceInfo) : LinkHost {
    val executed = CopyOnWriteArrayList<MediaCommand>()
    val peerSnapshots = CopyOnWriteArrayList<MediaSnapshot>()
    val chunks = CopyOnWriteArrayList<BlobChunk>()
    val sessionsStarted = CopyOnWriteArrayList<Boolean>()
    val gen = AtomicInteger()
    var artworkBytes: Map<String, ByteArray> = emptyMap()
    var commandDelayMs = 0L

    override fun localDevice() = device
    override fun capabilities() = caps
    override fun freshSnapshot() = MediaSnapshot(gen.incrementAndGet().toLong(), null, emptyList())
    override suspend fun executeCommand(peerId: String, command: MediaCommand): CommandResult {
        if (commandDelayMs > 0) kotlinx.coroutines.delay(commandDelayMs)
        executed += command
        return CommandResult.OK
    }

    override fun artwork(hash: String) = artworkBytes[hash]
    override fun lanEndpoints() = listOf(LanEndpoint("192.0.2.1", 47321))
    override fun onSessionStarted(peer: DeviceInfo, resumed: Boolean, peerCaps: Set<String>) {
        sessionsStarted += resumed
    }

    override fun onPeerDisconnected(peerId: String) = Unit
    override fun onPeerSnapshot(peerId: String, snapshot: MediaSnapshot, rttMs: Long?) {
        peerSnapshots += snapshot
    }

    override fun onBlobChunk(peerId: String, chunk: BlobChunk) {
        chunks += chunk
    }

    override fun onArtworkNotFound(peerId: String, hash: String) = Unit
    override fun onLyrics(peerId: String, lyrics: LyricsPush) = Unit
    override fun onLyricsNone(peerId: String, trackId: String) = Unit
    override fun onPolicyStatus(peerId: String, status: PolicyStatus) = Unit
    override suspend fun onAudioOffer(peerId: String, offer: AudioOffer, baseKey: ByteArray): JsonObject =
        buildJsonObject { put("ok", false) }

    override fun onAudioStop(peerId: String, streamId: String, reason: String) = Unit

    var volume = VolumeState(0.5, false, 15)
    val peerVolumes = CopyOnWriteArrayList<VolumeState>()
    val volumeRequests = CopyOnWriteArrayList<VolumeSet>()
    var btName: String? = "Test Phone BT"
    var caps = listOf("media.source", "media.control", "artwork", "volume")

    override fun volumeState() = volume
    override suspend fun applyVolume(peerId: String, request: VolumeSet): VolumeState {
        volumeRequests += request
        var v = volume
        request.level?.let { v = v.copy(level = it) }
        request.muted?.let { v = v.copy(muted = it) }
        request.delta?.let { d -> v = v.copy(level = ((v.level * 15 + d) / 15).coerceIn(0.0, 1.0)) }
        volume = v
        return v
    }

    override fun onPeerVolume(peerId: String, state: VolumeState) {
        peerVolumes += state
    }

    override fun bluetoothName() = btName
}

/** Accepts loopback connections and hands them to the LinkManager under test. */
class LoopbackServer(private val onAccept: (TransportStream) -> Unit) : AutoCloseable {
    private val server = ServerSocket(0, 50, java.net.InetAddress.getLoopbackAddress())
    val port: Int get() = server.localPort

    init {
        Thread {
            while (!server.isClosed) {
                val s = runCatching { server.accept() }.getOrNull() ?: break
                onAccept(SocketTransportStream(s))
            }
        }.apply { isDaemon = true }.start()
    }

    fun connect(kind: TransportKind = TransportKind.LAN): TestClientConnection =
        TestClientConnection(Socket(java.net.InetAddress.getLoopbackAddress(), port), kind)

    override fun close() = server.close()
}

/** Blocking, independent client-side protocol implementation (the "C" role) for tests. */
class TestClientConnection(private val socket: Socket, val kind: TransportKind) : AutoCloseable {
    private val decoder = FrameDecoder()
    private val pendingFrames = ArrayDeque<Frame>()
    private val input = socket.getInputStream()
    private val output = socket.getOutputStream()
    private var sealer: RecordCipher? = null
    private var opener: RecordCipher? = null
    var nextId = 1L

    init {
        socket.soTimeout = 5_000
    }

    fun sendFrame(type: Int, body: ByteArray) {
        output.write(FrameCodec.encode(type, body))
        output.flush()
    }

    fun sendHandshake(src: String, type: String, body: JsonObject, id: Long = nextId++) =
        sendHandshake(Envelope(1, type, id, System.currentTimeMillis(), src, "", null, body))

    fun sendHandshake(env: Envelope) = sendFrame(FrameType.HANDSHAKE, env.encode())

    /** Returns the next frame, or null on EOF. */
    fun readFrame(): Frame? {
        pendingFrames.removeFirstOrNull()?.let { return it }
        val buf = ByteArray(8192)
        while (true) {
            val n = input.read(buf)
            if (n < 0) return null
            val frames = decoder.feed(buf, 0, n)
            if (frames.isNotEmpty()) {
                pendingFrames.addAll(frames.drop(1))
                return frames[0]
            }
        }
    }

    fun readHandshake(): Envelope {
        val f = readFrame() ?: error("EOF while waiting for handshake")
        check(f.type == FrameType.HANDSHAKE) { "expected handshake frame, got ${f.type}" }
        return EnvelopeCodec.decode(f.body, 16 * 1024)
    }

    /** True when the server closed the connection (EOF) within the socket timeout. */
    fun isClosedByPeer(): Boolean = try {
        readFrame() == null
    } catch (e: SocketTimeoutException) {
        false
    } catch (e: java.io.IOException) {
        true
    }

    fun install(keys: KeySchedule.SessionKeys) {
        sealer = RecordCipher(keys.keyC2s, keys.ivC2s, FrameType.SECURE, true)
        opener = RecordCipher(keys.keyS2c, keys.ivS2c, FrameType.SECURE, false)
    }

    fun sendSecure(env: Envelope) = sendRecord(Channel.CONTROL, env.encode())

    fun sendRecord(channel: Int, data: ByteArray) {
        val ct = sealer!!.seal(byteArrayOf(channel.toByte()) + data)
        sendFrame(FrameType.SECURE, ct)
    }

    /** Reads the next SECURE record: (channel, data). */
    fun readRecord(): Pair<Int, ByteArray> {
        val f = readFrame() ?: error("EOF while waiting for record")
        check(f.type == FrameType.SECURE)
        val pt = opener!!.open(f.body)
        return (pt[0].toInt() and 0xff) to pt.copyOfRange(1, pt.size)
    }

    fun readControl(): Envelope {
        while (true) {
            val (ch, data) = readRecord()
            if (ch == Channel.CONTROL) return EnvelopeCodec.decode(data, 64 * 1024)
        }
    }

    /** Reads control messages until one of [type] arrives (skipping pings etc.). */
    fun readUntil(type: String): Envelope {
        while (true) {
            val e = readControl()
            if (e.type == type) return e
        }
    }

    override fun close() = socket.close()
}

/** Client identity + helpers to run the client side of the handshakes. */
class TestPeer(val id: String = "0123456789abcdef0123456789abcdef", val name: String = "Test PC") {
    val device = DeviceInfo(id, name, "windows", "test", "10", "1.0")
    var pairingKey: ByteArray? = null
    var sid: String? = null

    class Session(val conn: TestClientConnection, val sid: String, val resumed: Boolean, val serverCaps: List<String>)

    /** Runs pair.request -> pair.response; returns the computed SAS and secrets. */
    fun startPairing(conn: TestClientConnection): PairingState {
        val kp = P256.generate()
        val epk = P256.encodePublic(kp.public)
        val nonce = Rand.bytes(32)
        conn.sendHandshake(id, "pair.request", buildJsonObject {
            put("device", device.toJson())
            put("epk", B64.encode(epk))
            put("nonce", B64.encode(nonce))
        })
        val resp = conn.readHandshake()
        if (resp.type != "pair.response") return PairingState(resp, null, null, null)
        val epkS = resp.body.requireBase64("epk", 65)
        val nS = resp.body.requireBase64("nonce", 32)
        val serverId = (resp.body["device"] as JsonObject)["id"]!!.let { (it as JsonPrimitive).content }
        val z = P256.ecdh(kp.private, P256.decodePublic(epkS))
        val th = KeySchedule.pairingTranscript(nonce, nS, epk, epkS, id, serverId)
        return PairingState(resp, KeySchedule.pairing(z, th), th, serverId)
    }

    class PairingState(
        val response: Envelope,
        val secrets: KeySchedule.PairingSecrets?,
        val transcript: ByteArray?,
        val serverId: String?,
    )

    fun hello(
        conn: TestClientConnection,
        resume: String? = null,
        caps: List<String> = listOf("media.source", "media.control", "artwork", "lyrics", "policy.host"),
        min: Int = 1,
        max: Int = 1,
        key: ByteArray? = pairingKey,
        corruptMac: Boolean = false,
    ): Any {
        val kp = P256.generate()
        val epk = P256.encodePublic(kp.public)
        val nonce = Rand.bytes(32)
        conn.sendHandshake(id, "hello", buildJsonObject {
            put("device", device.toJson())
            put("protocol", buildJsonObject {
                put("min", min)
                put("max", max)
            })
            put("caps", JsonArray(caps.map { JsonPrimitive(it) }))
            put("epk", B64.encode(epk))
            put("nonce", B64.encode(nonce))
            put("transport", conn.kind.wire)
            if (resume != null) put("resume", resume)
        })
        val ack = conn.readHandshake()
        if (ack.type != "hello.ack") return ack
        val epkS = ack.body.requireBase64("epk", 65)
        val nS = ack.body.requireBase64("nonce", 32)
        val serverId = ack.src
        val z = P256.ecdh(kp.private, P256.decodePublic(epkS))
        val th = KeySchedule.sessionTranscript(nonce, nS, epk, epkS, id, serverId)
        val k = key!!
        check(app.mediaduet.crypto.constantTimeEquals(ack.body.requireBase64("mac", 32), KeySchedule.serverMac(k, th))) {
            "server MAC mismatch"
        }
        val mac = KeySchedule.clientMac(k, th).also { if (corruptMac) it[0] = (it[0] + 1).toByte() }
        conn.sendHandshake(id, "auth", buildJsonObject { put("mac", B64.encode(mac)) })
        conn.install(KeySchedule.session(k, z, th))
        val sid = (ack.body["sid"] as JsonPrimitive).content
        val resumed = (ack.body["resumed"] as JsonPrimitive).content == "true"
        val caps = (ack.body["caps"] as JsonArray).map { (it as JsonPrimitive).content }
        this.sid = sid
        return Session(conn, sid, resumed, caps)
    }

    private var sessionIds = 1L

    fun envelope(type: String, body: JsonObject, id: Long = sessionIds++, re: Long? = null) =
        Envelope(1, type, id, System.currentTimeMillis(), this.id, sid!!, re, body)

    companion object {
        fun artworkRef(bytes: ByteArray) = ArtworkRef(app.mediaduet.protocol.Hex.encode(app.mediaduet.crypto.sha256(bytes)), "image/jpeg", bytes.size)
    }
}
