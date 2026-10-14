package app.mediaduet.link

import kotlinx.coroutines.flow.StateFlow

data class TrustedPeer(
    val id: String,
    val name: String,
    val platform: String,
    val pairedAtMs: Long,
    val lastSeenMs: Long,
    /** The peer's MediaDuet version (DeviceInfo.app) when last seen; "" = unknown. */
    val appVersion: String = "",
)

/** Long-term pairing keys K per peer (§3.1). Implementations must keep keys in secure storage. */
interface TrustStore {
    val peers: StateFlow<List<TrustedPeer>>
    fun key(peerId: String): ByteArray?
    fun add(peer: TrustedPeer, key: ByteArray)
    fun remove(peerId: String)
    /** [appVersion] "" keeps the stored version. */
    fun updateSeen(peerId: String, name: String, seenAtMs: Long, appVersion: String = "")
}
