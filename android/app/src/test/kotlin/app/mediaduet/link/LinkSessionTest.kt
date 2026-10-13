package app.mediaduet.link

import app.mediaduet.crypto.sha256
import app.mediaduet.protocol.BlobChunk
import app.mediaduet.protocol.Channel
import app.mediaduet.protocol.DeviceInfo
import app.mediaduet.protocol.Hex
import app.mediaduet.protocol.MediaCommand
import app.mediaduet.protocol.MediaSnapshot
import app.mediaduet.util.MonoClock
import app.mediaduet.protocol.VolumeSet
import app.mediaduet.protocol.VolumeState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class LinkSessionTest {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val serverDevice = DeviceInfo("fedcba9876543210fedcba9876543210", "Test Phone", "android")
    private val trust = InMemoryTrustStore()
    private val pairing = PairingController(scope, MonoClock.SYSTEM_NANO)
    private val host = FakeHost(serverDevice)
    private val link = LinkManager(scope, host, trust, pairing, MonoClock.SYSTEM_NANO)
    private val server = LoopbackServer(link::accept)
    private val peer = TestPeer()

    @Before
    fun setUp() {
        val k = ByteArray(32) { (it + 1).toByte() }
        trust.add(TrustedPeer(peer.id, peer.name, "windows", 0, 0), k)
        peer.pairingKey = k
    }

    @After
    fun tearDown() {
        server.close()
        scope.cancel()
    }

    private fun open(resume: String? = null): Pair<TestClientConnection, TestPeer.Session> {
        val c = server.connect()
        val s = peer.hello(c, resume = resume) as TestPeer.Session
        c.readUntil("media.snapshot")
        return c to s
    }

    private fun command(action: String) = MediaCommand(null, action, null, MediaCommand.ORIGIN_USER, "test").toJson()

    @Test
    fun secondTransportResumesSessionAndPrimaryFollowsLinkPrimary() {
        val (c1, s1) = open()
        val (c2, s2) = open(resume = s1.sid)
        assertTrue(s2.resumed)
        assertEquals(s1.sid, s2.sid)
        waitFor { link.status.value.current?.transports?.size?.takeIf { it == 2 } }

        c2.sendSecure(peer.envelope("link.primary", buildJsonObject { }))
        waitFor {
            val transports = link.status.value.current?.transports.orEmpty()
            transports.firstOrNull { it.primary && it.connectionId == transports.maxOf { t -> t.connectionId } }
        }

        link.broadcastSnapshot(MediaSnapshot(99, null, emptyList()))
        val snap = c2.readUntil("media.snapshot")
        assertEquals("99", (snap.body["gen"] as JsonPrimitive).content)
        // c1 is secondary: the next message it sees is the pong to its ping, not the snapshot.
        c1.sendSecure(peer.envelope("ping", buildJsonObject { put("t", 1) }))
        assertEquals("pong", c1.readControl().type)

        // Switch back to c1.
        c1.sendSecure(peer.envelope("link.primary", buildJsonObject { }))
        Thread.sleep(200)
        link.broadcastSnapshot(MediaSnapshot(100, null, emptyList()))
        assertEquals("media.snapshot", c1.readControl().type)
        c1.close()
        c2.close()
    }

    @Test
    fun duplicateCommandOverSecondTransportExecutesOnce() {
        val (c1, s1) = open()
        val (c2, _) = open(resume = s1.sid)
        val cmd = peer.envelope("media.command", command("pause"), id = 500)
        c1.sendSecure(cmd)
        val r1 = c1.readUntil("media.command.result")
        assertEquals(500L, r1.re)
        c2.sendSecure(cmd) // same id delivered again on the other transport
        val r2 = c2.readUntil("media.command.result")
        assertEquals(500L, r2.re)
        assertEquals("true", (r2.body["ok"] as JsonPrimitive).content)
        assertNotEquals(r1.id, r2.id)
        Thread.sleep(100)
        assertEquals(1, host.executed.size)
        c1.close()
        c2.close()
    }

    @Test
    fun duplicateWhileCommandStillExecutingGetsResultToo() {
        host.commandDelayMs = 400
        val (c1, s1) = open()
        val (c2, _) = open(resume = s1.sid)
        val cmd = peer.envelope("media.command", command("next"), id = 700)
        c1.sendSecure(cmd)
        Thread.sleep(50)
        c2.sendSecure(cmd)
        assertEquals(700L, c1.readUntil("media.command.result").re)
        assertEquals(700L, c2.readUntil("media.command.result").re)
        assertEquals(1, host.executed.size)
        c1.close()
        c2.close()
    }

    @Test
    fun resumeAfterReconnectAndUnknownSidCreatesNewSession() {
        val (c1, s1) = open()
        c1.close()
        waitFor { if (link.status.value.current == null) true else null }
        val (c2, s2) = open(resume = s1.sid)
        assertTrue(s2.resumed)
        assertEquals(s1.sid, s2.sid)
        c2.close()
        val (c3, s3) = open(resume = "00000000000000000000000000000000")
        assertFalse(s3.resumed)
        assertNotEquals(s1.sid, s3.sid)
        c3.close()
        assertEquals(listOf(false, true, false), host.sessionsStarted.toList())
    }

    @Test
    fun staleSnapshotGenerationsAreIgnored() {
        val (c, _) = open()
        fun snap(gen: Int) = buildJsonObject {
            put("gen", gen)
            put("sessions", kotlinx.serialization.json.JsonArray(emptyList()))
        }
        c.sendSecure(peer.envelope("media.snapshot", snap(5)))
        c.sendSecure(peer.envelope("media.snapshot", snap(3)))
        c.sendSecure(peer.envelope("media.snapshot", snap(6)))
        waitFor { host.peerSnapshots.size.takeIf { it == 2 } }
        Thread.sleep(100)
        assertEquals(listOf(5L, 6L), host.peerSnapshots.map { it.gen })
        c.close()
    }

    @Test
    fun artworkIsServedAsBlobChunks() {
        val art = ByteArray(80_000) { (it * 31).toByte() }
        val hash = Hex.encode(sha256(art))
        host.artworkBytes = mapOf(hash to art)
        val (c, _) = open()
        c.sendSecure(peer.envelope("artwork.get", buildJsonObject { put("hash", hash) }))
        val assembled = ByteArray(art.size)
        var received = 0
        while (received < art.size) {
            val (ch, data) = c.readRecord()
            if (ch != Channel.BLOB) continue
            val chunk = BlobChunk.decode(data)
            assertEquals(art.size, chunk.total)
            System.arraycopy(chunk.bytes, 0, assembled, chunk.offset, chunk.bytes.size)
            received += chunk.bytes.size
        }
        assertArrayEquals(art, assembled)
        c.sendSecure(peer.envelope("artwork.get", buildJsonObject { put("hash", "0".repeat(64)) }, id = 900))
        val err = c.readUntil("error")
        assertEquals(900L, err.re)
        assertEquals("not_found", (err.body["code"] as JsonPrimitive).content)
        c.close()
    }

    @Test
    fun serverCommandRoundTrip() = runBlocking {
        val (c, _) = open()
        val pending = async(Dispatchers.Default) {
            link.sendCommand(MediaCommand("spotify.exe", "pause", null, MediaCommand.ORIGIN_USER, "ui"))
        }
        val cmd = c.readUntil("media.command")
        assertEquals("pause", (cmd.body["action"] as JsonPrimitive).content)
        c.sendSecure(peer.envelope("media.command.result", buildJsonObject { put("ok", true) }, re = cmd.id))
        assertTrue(pending.await().ok)
        c.close()
    }

    @Test
    fun badMessageGetsErrorWithRe() {
        val (c, s) = open()
        val wrongSid = app.mediaduet.protocol.Envelope(1, "ping", 77, 0, peer.id, "0".repeat(32), null, buildJsonObject { })
        assertNotEquals(s.sid, wrongSid.sid)
        c.sendSecure(wrongSid)
        val err = c.readUntil("error")
        assertEquals(77L, err.re)
        assertEquals("bad_message", (err.body["code"] as JsonPrimitive).content)
        c.close()
    }

    @Test
    fun volumeStateAfterReadyOnlyForPeersWithVolumeCap() {
        // Peer announces "volume": session.ready, transport.info (with btName), snapshot, volume.state.
        val c = server.connect()
        peer.hello(c, caps = listOf("media.source", "media.control", "volume"))
        assertEquals("session.ready", c.readControl().type)
        val info = c.readControl()
        assertEquals("transport.info", info.type)
        assertEquals("Test Phone BT", (info.body["btName"] as JsonPrimitive).content)
        assertEquals("media.snapshot", c.readControl().type)
        val vol = c.readControl()
        assertEquals("volume.state", vol.type)
        assertEquals(VolumeState(0.5, false, 15), VolumeState.parse(vol.body))
        c.close()
        // Peer without the cap: no volume.state; the next message is the pong to our ping.
        val (c2, _) = open()
        c2.sendSecure(peer.envelope("ping", buildJsonObject { put("t", 5) }))
        assertEquals("pong", c2.readControl().type)
        c2.close()
    }

    @Test
    fun volumeSetIsAppliedAndAnsweredAndPeerVolumeIsReported() {
        val c = server.connect()
        peer.hello(c, caps = listOf("volume"))
        c.readUntil("volume.state")
        c.sendSecure(peer.envelope("volume.set", buildJsonObject { put("level", 0.8) }, id = 40))
        val answer = c.readUntil("volume.state")
        assertEquals(40L, answer.re)
        assertEquals(0.8, VolumeState.parse(answer.body).level, 0.0)
        c.sendSecure(peer.envelope("volume.set", buildJsonObject { put("muted", true) }, id = 41))
        assertTrue(VolumeState.parse(c.readUntil("volume.state").body).muted)
        c.sendSecure(peer.envelope("volume.set", buildJsonObject { }, id = 42))
        assertEquals(42L, c.readUntil("error").re)
        // Session-scoped (per-app) volume does not exist on Android: unsupported, not applied.
        c.sendSecure(peer.envelope("volume.set", buildJsonObject {
            put("level", 0.1)
            put("key", "com.x#1")
        }, id = 43))
        val err = c.readUntil("error")
        assertEquals(43L, err.re)
        assertEquals("unsupported", (err.body["code"] as JsonPrimitive).content)
        c.sendSecure(peer.envelope("volume.state", buildJsonObject {
            put("level", 0.3)
            put("muted", false)
        }))
        waitFor { host.peerVolumes.firstOrNull() }
        assertEquals(0.3, host.peerVolumes.first().level, 0.0)
        assertEquals(2, host.volumeRequests.size)
        c.close()
    }

    @Test
    fun serverVolumeSetGoesOnlyToPeersWithCap() = runBlocking {
        val (c0, _) = open() // no "volume" cap
        assertFalse(link.sendVolumeSet(VolumeSet(delta = 1)))
        c0.close()
        waitFor { if (link.status.value.current == null) true else null }
        val c = server.connect()
        peer.hello(c, caps = listOf("volume"))
        c.readUntil("volume.state")
        assertTrue(link.sendVolumeSet(VolumeSet(delta = -2)))
        val req = c.readUntil("volume.set")
        assertEquals(-2, VolumeSet.parse(req.body).delta)
        c.close()
    }

    @Test
    fun tamperedRecordClosesConnection() {
        val (c, _) = open()
        c.sendFrame(app.mediaduet.protocol.FrameType.SECURE, ByteArray(40) { 3 })
        assertTrue(c.isClosedByPeer())
    }

    @Test
    fun revocationClosesSessionAndNextHelloIsNotPaired() {
        val (c, _) = open()
        link.revoke(peer.id)
        assertEquals("bye", c.readUntil("bye").type)
        assertTrue(c.isClosedByPeer())
        server.connect().use { c2 ->
            val reply = peer.hello(c2) as app.mediaduet.protocol.Envelope
            assertEquals("not_paired", (reply.body["code"] as JsonPrimitive).content)
        }
    }
}
