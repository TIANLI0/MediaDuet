package app.mediaduet.link

import app.mediaduet.crypto.KeySchedule
import app.mediaduet.protocol.B64
import app.mediaduet.protocol.DeviceInfo
import app.mediaduet.protocol.Envelope
import app.mediaduet.protocol.FrameType
import app.mediaduet.protocol.requireBase64
import app.mediaduet.util.MonoClock
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

fun <T : Any> waitFor(timeoutMs: Long = 5_000, block: () -> T?): T {
    val end = System.currentTimeMillis() + timeoutMs
    while (System.currentTimeMillis() < end) {
        block()?.let { return it }
        Thread.sleep(10)
    }
    error("condition not met within $timeoutMs ms")
}

class HandshakeTest {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val serverDevice = DeviceInfo("fedcba9876543210fedcba9876543210", "Test Phone", "android")
    private val trust = InMemoryTrustStore()
    private val pairing = PairingController(scope, MonoClock.SYSTEM_NANO)
    private val host = FakeHost(serverDevice)
    private val link = LinkManager(scope, host, trust, pairing, MonoClock.SYSTEM_NANO)
    private val server = LoopbackServer(link::accept)
    private val peer = TestPeer()

    @After
    fun tearDown() {
        server.close()
        scope.cancel()
    }

    private fun Envelope.str(name: String) = (body[name] as JsonPrimitive).content

    @Test
    fun pairingSucceedsWhenBothSidesAccept() {
        pairing.open()
        server.connect().use { c ->
            val st = peer.startPairing(c)
            assertEquals("pair.response", st.response.type)
            val secrets = st.secrets!!
            val prompt = waitFor { pairing.state.value.prompt }
            assertEquals(secrets.sas, prompt.sas)
            assertEquals(peer.name, prompt.peerName)
            pairing.accept(prompt.attemptId)
            c.sendHandshake(peer.id, "pair.confirm", buildJsonObject {
                put("mac", B64.encode(KeySchedule.confirmMac(secrets.pairingKey, "C", st.transcript!!)))
            })
            val confirm = c.readHandshake()
            assertEquals("pair.confirm", confirm.type)
            assertArrayEquals(KeySchedule.confirmMac(secrets.pairingKey, "S", st.transcript!!), confirm.body.requireBase64("mac", 32))
            val result = c.readHandshake()
            assertEquals("pair.result", result.type)
            assertEquals("true", result.str("ok"))
            assertTrue(c.isClosedByPeer())
            assertArrayEquals(secrets.pairingKey, trust.key(peer.id))
        }
        // The outcome is recorded after pair.result is sent and the connection closed: wait for it.
        assertEquals(true, waitFor { pairing.state.value.lastOutcome }.success)
        assertFalse(pairing.isOpen())
    }

    @Test
    fun peerConfirmBeforeLocalAcceptWaitsForUser() {
        pairing.open()
        server.connect().use { c ->
            val st = peer.startPairing(c)
            val secrets = st.secrets!!
            c.sendHandshake(peer.id, "pair.confirm", buildJsonObject {
                put("mac", B64.encode(KeySchedule.confirmMac(secrets.pairingKey, "C", st.transcript!!)))
            })
            Thread.sleep(200)
            assertNull(trust.key(peer.id)) // nothing stored before the local user accepted
            pairing.accept(waitFor { pairing.state.value.prompt }.attemptId)
            assertEquals("pair.confirm", c.readHandshake().type)
            assertEquals("pair.result", c.readHandshake().type)
        }
        assertEquals(32, waitFor { trust.key(peer.id) }.size)
    }

    @Test
    fun pairingDisabledOutsidePairingMode() {
        server.connect().use { c ->
            val st = peer.startPairing(c)
            assertEquals("pair.result", st.response.type)
            assertEquals("false", st.response.str("ok"))
            assertEquals("pairing_disabled", st.response.str("reason"))
            assertTrue(c.isClosedByPeer())
        }
        assertNull(trust.key(peer.id))
    }

    @Test
    fun pairingRejectedByLocalUser() {
        pairing.open()
        server.connect().use { c ->
            peer.startPairing(c)
            pairing.reject(waitFor { pairing.state.value.prompt }.attemptId)
            val result = c.readHandshake()
            assertEquals("pair.result", result.type)
            assertEquals("rejected", result.str("reason"))
            assertTrue(c.isClosedByPeer())
        }
        assertNull(trust.key(peer.id))
        assertEquals("rejected", waitFor { pairing.state.value.lastOutcome }.reason)
    }

    @Test
    fun pairingWithWrongConfirmMacFails() {
        pairing.open()
        server.connect().use { c ->
            val st = peer.startPairing(c)
            pairing.accept(waitFor { pairing.state.value.prompt }.attemptId)
            val wrong = KeySchedule.confirmMac(st.secrets!!.pairingKey, "S", st.transcript!!) // wrong role
            c.sendHandshake(peer.id, "pair.confirm", buildJsonObject { put("mac", B64.encode(wrong)) })
            val result = c.readHandshake()
            assertEquals("pair.result", result.type)
            assertEquals("mac_mismatch", result.str("reason"))
            assertTrue(c.isClosedByPeer())
        }
        assertNull(trust.key(peer.id))
    }

    // ---- QR pairing (§3.5) -----------------------------------------------------------------

    private val qrToken = ByteArray(QrPairing.TOKEN_BYTES) { (0x40 + it).toByte() }

    private fun armQr(peerId: String = peer.id) = pairing.armQr(peerId, "Scanned PC", qrToken)

    @Test
    fun qrPairingSucceedsWithoutSas() {
        armQr()
        server.connect().use { c ->
            val st = peer.startPairing(c, qr = true)
            assertEquals("pair.response", st.response.type)
            assertNull(pairing.state.value.prompt) // no SAS for the user to compare
            c.sendHandshake(peer.id, "pair.confirm", buildJsonObject {
                put("mac", B64.encode(KeySchedule.qrConfirmMac(qrToken, "C", st.transcript!!)))
            })
            val confirm = c.readHandshake()
            assertEquals("pair.confirm", confirm.type)
            assertArrayEquals(KeySchedule.qrConfirmMac(qrToken, "S", st.transcript!!), confirm.body.requireBase64("mac", 32))
            val result = c.readHandshake()
            assertEquals("pair.result", result.type)
            assertEquals("true", result.str("ok"))
            // K is the one of SAS pairing (§3.3), not derived from T.
            assertArrayEquals(st.secrets!!.pairingKey, trust.key(peer.id))
        }
        assertEquals(true, waitFor { pairing.state.value.lastOutcome }.success)
        assertFalse(pairing.isOpen())
        assertNull(pairing.state.value.qr) // disarmed after a success
    }

    @Test
    fun qrPairingWithWrongTokenFails() {
        armQr()
        server.connect().use { c ->
            val st = peer.startPairing(c, qr = true)
            val wrong = ByteArray(QrPairing.TOKEN_BYTES) { 1 }
            c.sendHandshake(peer.id, "pair.confirm", buildJsonObject {
                put("mac", B64.encode(KeySchedule.qrConfirmMac(wrong, "C", st.transcript!!)))
            })
            val result = c.readHandshake()
            assertEquals("pair.result", result.type)
            assertEquals("mac_mismatch", result.str("reason"))
            assertTrue(c.isClosedByPeer())
        }
        assertNull(trust.key(peer.id))
        // Still armed: the PC keeps trying while its code is shown.
        waitFor { pairing.state.value.lastOutcome }
        assertEquals(peer.id, pairing.state.value.qr?.peerId)
    }

    @Test
    fun qrPairingNeedsAScannedCode() {
        pairing.open() // plain pairing mode only
        server.connect().use { c ->
            val st = peer.startPairing(c, qr = true)
            assertEquals("pair.result", st.response.type)
            assertEquals("pairing_disabled", st.response.str("reason"))
            assertTrue(c.isClosedByPeer())
        }
        assertNull(trust.key(peer.id))
    }

    @Test
    fun qrPairingOnlyForTheScannedPc() {
        armQr(peerId = "ffffffffffffffffffffffffffffffff")
        server.connect().use { c ->
            val st = peer.startPairing(c, qr = true)
            assertEquals("pair.result", st.response.type)
            assertEquals("pairing_disabled", st.response.str("reason"))
        }
        assertNull(trust.key(peer.id))
    }

    @Test
    fun sasPairingStillWorksWhileArmedForQr() {
        armQr(peerId = "ffffffffffffffffffffffffffffffff")
        server.connect().use { c ->
            val st = peer.startPairing(c)
            val prompt = waitFor { pairing.state.value.prompt }
            assertEquals(st.secrets!!.sas, prompt.sas)
            pairing.accept(prompt.attemptId)
            c.sendHandshake(peer.id, "pair.confirm", buildJsonObject {
                put("mac", B64.encode(KeySchedule.confirmMac(st.secrets!!.pairingKey, "C", st.transcript!!)))
            })
            assertEquals("pair.confirm", c.readHandshake().type)
            assertEquals("true", c.readHandshake().str("ok"))
        }
        assertEquals(32, waitFor { trust.key(peer.id) }.size)
    }

    private fun trustPeer(): ByteArray {
        val k = ByteArray(32) { (it * 7).toByte() }
        trust.add(TrustedPeer(peer.id, peer.name, "windows", 0, 0), k)
        peer.pairingKey = k
        return k
    }

    @Test
    fun sessionHandshakeEstablishesEncryptedSession() {
        trustPeer()
        server.connect().use { c ->
            val s = peer.hello(c) as TestPeer.Session
            assertFalse(s.resumed)
            assertTrue("media.control" in s.serverCaps)
            val ready = c.readControl()
            assertEquals("session.ready", ready.type)
            assertEquals(s.sid, ready.str("sid"))
            assertEquals(s.sid, ready.sid)
            assertEquals(serverDevice.id, ready.src)
            assertEquals("transport.info", c.readControl().type)
            assertEquals("media.snapshot", c.readControl().type)
            // Encrypted ping/pong round trip.
            c.sendSecure(peer.envelope("ping", buildJsonObject { put("t", 42) }, id = 10))
            val pong = c.readUntil("pong")
            assertEquals(10L, pong.re)
            assertEquals("42", pong.str("t"))
        }
    }

    // ---- lyrics.offset (§8.2) ---------------------------------------------------------------

    private fun offsetSession(c: TestClientConnection, caps: List<String>): TestPeer.Session {
        trustPeer()
        val s = peer.hello(c, caps = caps) as TestPeer.Session
        c.readUntil("media.snapshot")
        return s
    }

    @Test
    fun peerLyricsOffsetIsStoredAndNotEchoed() {
        server.connect().use { c ->
            offsetSession(c, listOf("media.source", "lyrics", "lyrics.offset"))
            c.sendSecure(peer.envelope("lyrics.offset", buildJsonObject {
                put("appId", "cloudmusic.exe")
                put("offsetMs", -9000) // clamped to the allowed range
            }, id = 20))
            c.sendSecure(peer.envelope("ping", buildJsonObject { put("t", 1) }, id = 21))
            var e = c.readControl()
            while (e.type != "pong") {
                assertFalse("echoed ${e.type}", e.type == "lyrics.offset")
                e = c.readControl()
            }
            assertEquals("cloudmusic.exe" to -5000, waitFor { host.lyricOffsets.firstOrNull() })
        }
    }

    @Test
    fun lyricsOffsetGoesOnlyToPeersWithTheCap() {
        server.connect().use { c ->
            offsetSession(c, listOf("media.source", "lyrics", "lyrics.offset"))
            link.broadcastLyricsOffset("com.netease.cloudmusic", 300)
            val e = c.readUntil("lyrics.offset")
            assertEquals("com.netease.cloudmusic", e.str("appId"))
            assertEquals("300", e.str("offsetMs"))
        }
    }

    @Test
    fun lyricsOffsetNotSentWithoutTheCap() {
        server.connect().use { c ->
            offsetSession(c, listOf("media.source", "lyrics"))
            link.broadcastLyricsOffset("com.netease.cloudmusic", 300)
            c.sendSecure(peer.envelope("ping", buildJsonObject { put("t", 2) }, id = 30))
            var e = c.readControl()
            while (e.type != "pong") {
                assertFalse(e.type == "lyrics.offset")
                e = c.readControl()
            }
        }
    }

    @Test
    fun unpairedPeerGetsNotPaired() {
        peer.pairingKey = ByteArray(32)
        server.connect().use { c ->
            val reply = peer.hello(c) as Envelope
            assertEquals("error", reply.type)
            assertEquals("not_paired", reply.str("code"))
            assertTrue(c.isClosedByPeer())
        }
    }

    @Test
    fun wrongAuthMacIsRejected() {
        trustPeer()
        server.connect().use { c ->
            peer.hello(c, corruptMac = true)
            val f = c.readFrame()!!
            assertEquals(FrameType.HANDSHAKE, f.type)
            val err = app.mediaduet.protocol.EnvelopeCodec.decode(f.body, 16384)
            assertEquals("error", err.type)
            assertEquals("auth_failed", err.str("code"))
            assertTrue(c.isClosedByPeer())
        }
    }

    @Test
    fun serverMacDoesNotVerifyWithWrongKey() {
        trustPeer()
        server.connect().use { c ->
            val result = runCatching { peer.hello(c, key = ByteArray(32) { 1 }) }
            assertTrue(result.exceptionOrNull()?.message?.contains("server MAC mismatch") == true)
        }
    }

    @Test
    fun versionMismatchIsRejected() {
        trustPeer()
        server.connect().use { c ->
            val reply = peer.hello(c, min = 2, max = 3) as Envelope
            assertEquals("error", reply.type)
            assertEquals("version_unsupported", reply.str("code"))
            assertTrue(c.isClosedByPeer())
        }
    }

    @Test
    fun secureFrameBeforeHandshakeClosesConnection() {
        server.connect().use { c ->
            c.sendFrame(FrameType.SECURE, ByteArray(32))
            assertTrue(c.isClosedByPeer())
        }
    }

    @Test
    fun unknownHandshakeTypeAborts() {
        server.connect().use { c ->
            c.sendHandshake(peer.id, "media.snapshot", buildJsonObject { })
            val err = c.readHandshake()
            assertEquals("error", err.type)
            assertEquals("bad_message", err.str("code"))
            assertTrue(c.isClosedByPeer())
        }
    }
}
