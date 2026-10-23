package app.mediaduet.link

import app.mediaduet.protocol.Caps
import app.mediaduet.protocol.DeviceInfo
import app.mediaduet.protocol.MediaSnapshot
import app.mediaduet.protocol.PeerPolicy
import app.mediaduet.transport.SocketTransportStream
import app.mediaduet.util.MonoClock
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.net.InetAddress
import java.net.Socket

/** Two Android LinkManagers: one listens (server), the other pairs and dials (client, §13). */
class AndroidPeerLinkTest {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    private class Side(scope: CoroutineScope, id: String, name: String) {
        val device = DeviceInfo(id, name, "android")
        val trust = InMemoryTrustStore()
        val pairing = PairingController(scope, MonoClock.SYSTEM_NANO)
        val host = FakeHost(device).apply { caps = listOf(Caps.MEDIA_SOURCE, Caps.MEDIA_CONTROL, Caps.ARTWORK, Caps.POLICY_PEER) }
        val link = LinkManager(scope, host, trust, pairing, MonoClock.SYSTEM_NANO)
    }

    private val tablet = Side(scope, "b".repeat(32), "Tablet")
    private val phone = Side(scope, "a".repeat(32), "Phone")
    private val server = LoopbackServer(tablet.link::accept)

    @After
    fun tearDown() {
        server.close()
        scope.cancel()
    }

    private fun stream() = SocketTransportStream(Socket(InetAddress.getLoopbackAddress(), server.port))

    private fun pair() = runBlocking {
        tablet.pairing.open()
        val result = scope.async { phone.link.pairWith(stream()) }
        val tabletPrompt = waitFor { tablet.pairing.state.value.prompt }
        val phonePrompt = waitFor { phone.pairing.state.value.prompt }
        assertEquals(tabletPrompt.sas, phonePrompt.sas)
        assertTrue(phonePrompt.outgoing)
        phone.pairing.accept(phonePrompt.attemptId)
        tablet.pairing.accept(tabletPrompt.attemptId)
        assertTrue(result.await() is HandshakeResult.Paired)
    }

    @Test
    fun pairDialAndExchangeMessages() {
        pair()
        assertArrayEquals(tablet.trust.key(phone.device.id), phone.trust.key(tablet.device.id))
        assertEquals("android", phone.trust.peers.value.single().platform)
        assertTrue(waitFor { phone.pairing.state.value.lastOutcome }.success)

        val r = runBlocking { phone.link.dial(stream(), tablet.device.id) }
        assertTrue(r is HandshakeResult.Session)
        val onTablet = waitFor { tablet.link.status.value.current }
        assertEquals(phone.device.id, onTablet.peer.id)
        val onPhone = waitFor { phone.link.status.value.current }
        assertTrue(onPhone.outgoing)
        // Each side sent its snapshot after attaching; the client got the server's addresses.
        waitFor { tablet.host.peerSnapshots.firstOrNull() }
        waitFor { phone.host.peerSnapshots.firstOrNull() }
        waitFor { phone.host.peerEndpoints.firstOrNull() }

        // Commands work in both directions.
        val cmd = app.mediaduet.protocol.MediaCommand(null, "pause", null, "user", "test")
        assertTrue(runBlocking { phone.link.sendCommand(cmd) }.ok)
        assertTrue(runBlocking { tablet.link.sendCommand(cmd) }.ok)
        waitFor { tablet.host.executed.firstOrNull() }
        waitFor { phone.host.executed.firstOrNull() }

        val policy = PeerPolicy(PeerPolicy.MODE_LATEST, 1, call = false, playing = true, cause = PeerPolicy.CAUSE_USER, onsetAt = 5, pausedAt = 0)
        phone.link.broadcastPeerPolicy(policy)
        assertEquals(policy, waitFor { tablet.host.peerPolicies.firstOrNull() })
        tablet.link.broadcastSnapshot(MediaSnapshot(100, null, emptyList()))
        waitFor { phone.host.peerSnapshots.firstOrNull { it.gen == 100L } }
    }

    @Test
    fun pairingFailsWhenThePeerIsNotInPairingMode() {
        val r = runBlocking { phone.link.pairWith(stream()) }
        assertEquals("pairing_disabled", (r as HandshakeResult.Failed).reason)
        assertNull(phone.trust.key(tablet.device.id))
    }

    @Test
    fun dialAfterThePeerForgotUsRemovesThePairing() {
        pair()
        tablet.trust.remove(phone.device.id)
        val r = runBlocking { phone.link.dial(stream(), tablet.device.id) }
        assertEquals("not_paired", (r as HandshakeResult.Failed).reason)
        assertNull(phone.trust.key(tablet.device.id))
    }
}
