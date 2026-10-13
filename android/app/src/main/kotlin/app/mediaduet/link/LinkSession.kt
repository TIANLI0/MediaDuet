package app.mediaduet.link

import app.mediaduet.protocol.CommandResult
import app.mediaduet.protocol.DeviceInfo
import app.mediaduet.protocol.Envelope
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.serialization.json.JsonObject

/**
 * A logical link session (§7). Survives transport switches; several connections may be attached
 * at once. All mutation happens on the LinkManager's single-threaded dispatcher.
 */
class LinkSession(
    val sid: String,
    var peer: DeviceInfo,
    val version: Int,
    var peerCaps: Set<String>,
    createdAtMs: Long,
) {
    val connections = mutableListOf<Connection>()
    var primary: Connection? = null
        private set

    /** Bumped whenever the primary changes; heartbeat loops observe it to adapt their interval. */
    val primaryEpoch = MutableStateFlow(0)

    var lastActiveMs: Long = createdAtMs
    var attachedAtMs: Long = createdAtMs
    var rttMs: Long? = null
    var lastPeerGen: Long = 0
    var resumeCount: Int = 0

    private var lastSentId = 0L

    /** Peer ids already processed (§7 receiver de-duplication). */
    val dedup = DedupWindow(1024)

    /** Cached `media.command.result` bodies by request id, re-sent for duplicates. */
    private val results = object : LinkedHashMap<Long, JsonObject>(64, 0.75f, false) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<Long, JsonObject>?): Boolean = size > 256
    }

    /** Commands from the peer still executing: request id -> connections waiting for the result. */
    val inFlight = HashMap<Long, MutableList<Connection>>()

    class PendingCommand(
        val envelope: Envelope,
        var sentAtMs: Long,
        var connection: Connection,
        val result: CompletableDeferred<CommandResult>,
    )

    /** Commands we sent to the peer, awaiting `media.command.result`. */
    val pending = HashMap<Long, PendingCommand>()

    /** Our `artwork.get` requests: id -> hash. */
    val artworkRequests = HashMap<Long, String>()

    fun nextId(): Long = ++lastSentId

    fun cacheResult(id: Long, body: JsonObject) {
        results[id] = body
    }

    fun cachedResult(id: Long): JsonObject? = results[id]

    fun setPrimary(conn: Connection?) {
        if (primary !== conn) {
            primary = conn
            primaryEpoch.value++
        }
    }

    /** The connection for unsolicited messages: designated primary, else the newest live one. */
    fun primaryOrAny(): Connection? =
        primary?.takeIf { !it.isClosed } ?: connections.lastOrNull { !it.isClosed }

    val isConnected: Boolean get() = connections.any { !it.isClosed }
}
