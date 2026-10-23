package app.mediaduet.service

import app.mediaduet.link.HandshakeResult
import app.mediaduet.link.LinkManager
import app.mediaduet.link.LinkStatus
import app.mediaduet.link.TrustStore
import app.mediaduet.link.TrustedPeer
import app.mediaduet.protocol.ErrorCode
import app.mediaduet.protocol.LanEndpoint
import app.mediaduet.transport.NearbyDevice
import app.mediaduet.transport.PeerDiscovery
import app.mediaduet.transport.SocketTransportStream
import app.mediaduet.util.MdLog
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.merge
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import java.net.InetSocketAddress
import java.net.Socket
import kotlin.random.Random

/**
 * Keeps paired Android devices connected (§13): of two Android peers the one with the smaller
 * device id dials (so they never open two sessions at once), the other only listens. Addresses
 * come from DNS-SD and from the peer's `transport.info`; reconnects back off 2 s → 60 s (±20 %)
 * and start over at once when the peer is found again or the network changes.
 */
class PeerDialer(
    private val scope: CoroutineScope,
    private val selfId: String,
    private val link: LinkManager,
    private val trust: TrustStore,
    private val discovery: PeerDiscovery,
    private val settings: AppSettings,
) {
    private val jobs = HashMap<String, Job>()
    private val wake = MutableSharedFlow<Unit>(extraBufferCapacity = 1)

    fun start() {
        scope.launch {
            combine(trust.peers, settings.mode) { peers, mode -> if (mode.linked) peers.filter(::dials) else emptyList() }
                .collect { sync(it) }
        }
    }

    /** True for paired Android devices this device connects to (rather than waiting for them). */
    fun dials(peer: TrustedPeer) = peer.platform == LinkStatus.PLATFORM_ANDROID && selfId < peer.id

    /** Something changed (network, app in the foreground): retry now instead of after the back-off. */
    fun nudge() {
        wake.tryEmit(Unit)
    }

    @Synchronized
    private fun sync(peers: List<TrustedPeer>) {
        val ids = peers.map { it.id }.toSet()
        jobs.keys.filter { it !in ids }.forEach { id ->
            jobs.remove(id)?.cancel()
            discovery.release(holder(id))
        }
        for (p in peers) if (p.id !in jobs) jobs[p.id] = scope.launch { keepConnected(p.id, p.name) }
    }

    private suspend fun keepConnected(id: String, name: String) {
        var backoff = MIN_BACKOFF_MS
        try {
            while (scope.isActive) {
                if (link.isConnected(id)) {
                    discovery.release(holder(id))
                    link.status.first { s -> s.sessions.none { it.peer.id == id && it.connected } }
                    backoff = MIN_BACKOFF_MS
                    continue
                }
                discovery.acquire(holder(id))
                when (tryOnce(id)) {
                    Attempt.CONNECTED -> {
                        // The session attaches right after the handshake: wait for it to show up.
                        withTimeoutOrNull(5_000) { link.status.first { s -> s.sessions.any { it.peer.id == id && it.connected } } }
                        continue
                    }
                    Attempt.REVOKED -> return
                    Attempt.FAILED -> Unit
                }
                val wait = (backoff * Random.nextDouble(0.8, 1.2)).toLong()
                withTimeoutOrNull(wait) {
                    merge(wake, discovery.devices.map { list -> list.any { it.id == id } }.filter { it }.map { }).first()
                }
                backoff = (backoff * 2).coerceAtMost(MAX_BACKOFF_MS)
            }
        } finally {
            discovery.release(holder(id))
            MdLog.d(TAG, "stopped dialling $name")
        }
    }

    private enum class Attempt { CONNECTED, FAILED, REVOKED }

    private suspend fun tryOnce(id: String): Attempt {
        val found = discovery.devices.value.filter { it.id == id }.map { LanEndpoint(it.host, it.port) }
        val candidates = (found + settings.peerEndpoints(id)).distinct()
        for (e in candidates) {
            val stream = connect(e) ?: continue
            when (val r = link.dial(stream, id)) {
                is HandshakeResult.Session -> {
                    settings.rememberPeerEndpoints(id, listOf(e))
                    return Attempt.CONNECTED
                }
                is HandshakeResult.Failed -> if (r.reason == ErrorCode.NOT_PAIRED) return Attempt.REVOKED
                is HandshakeResult.Paired -> Unit
            }
        }
        return Attempt.FAILED
    }

    /**
     * Pairs with [device] (it shows its pairing window): the SAS appears on both screens. Returns
     * a failure reason, or null when the devices are paired.
     */
    suspend fun pair(device: NearbyDevice): String? {
        val stream = connect(LanEndpoint(device.host, device.port)) ?: return "unreachable"
        return when (val r = link.pairWith(stream)) {
            is HandshakeResult.Paired -> {
                settings.rememberPeerEndpoints(r.peer.id, listOf(LanEndpoint(device.host, device.port)))
                null
            }
            is HandshakeResult.Failed -> r.reason
            is HandshakeResult.Session -> null
        }
    }

    private suspend fun connect(e: LanEndpoint): SocketTransportStream? = withContext(Dispatchers.IO) {
        val socket = Socket()
        try {
            socket.connect(InetSocketAddress(e.host, e.port), CONNECT_TIMEOUT_MS)
            SocketTransportStream(socket)
        } catch (ex: Exception) {
            MdLog.d(TAG, "cannot reach ${e.host}:${e.port}: ${ex.javaClass.simpleName}")
            runCatching { socket.close() }
            null
        }
    }

    private fun holder(id: String) = "dial:$id"

    companion object {
        private const val TAG = "MD/Dialer"
        const val MIN_BACKOFF_MS = 2_000L
        const val MAX_BACKOFF_MS = 60_000L
        const val CONNECT_TIMEOUT_MS = 3_000
    }
}
