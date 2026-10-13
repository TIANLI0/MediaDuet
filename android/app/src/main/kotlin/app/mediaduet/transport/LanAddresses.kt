package app.mediaduet.transport

/** One network as seen by ConnectivityManager, reduced to what the LAN endpoint choice needs. */
data class NetCandidate(
    val wifi: Boolean,
    val ethernet: Boolean,
    val vpn: Boolean,
    val cellular: Boolean,
    val isDefault: Boolean,
    val addresses: List<String>,
)

/**
 * Which local addresses are advertised to the PC (`transport.info.lan`, NSD): only IPv4
 * addresses of Wi-Fi / Ethernet networks - never VPN, cellular or other virtual interfaces,
 * which may accept TCP without ever reaching this app. The default Wi-Fi network comes first.
 * Addresses of the phone's own hotspot interface (not a ConnectivityManager network) are
 * appended last so a PC on the phone's hotspot can still connect. Pure and unit-tested.
 */
object LanAddressPolicy {
    fun pick(networks: List<NetCandidate>, hotspot: List<String> = emptyList()): List<String> =
        networks
            .filter { (it.wifi || it.ethernet) && !it.vpn && !it.cellular }
            .sortedWith(
                compareByDescending<NetCandidate> { it.isDefault && it.wifi }
                    .thenByDescending { it.isDefault }
                    .thenByDescending { it.wifi },
            )
            .flatMap { it.addresses }
            .plus(hotspot)
            .filter(::isUsableIpv4)
            .distinct()

    /** Dotted IPv4 that a LAN peer could reach: not loopback, link-local, unspecified or multicast. */
    fun isUsableIpv4(a: String): Boolean {
        val parts = a.split('.')
        if (parts.size != 4) return false
        val b = parts.map { it.toIntOrNull() ?: return false }
        if (b.any { it !in 0..255 }) return false
        return when {
            b[0] == 0 || b[0] == 127 -> false
            b[0] == 169 && b[1] == 254 -> false
            b[0] >= 224 -> false
            else -> true
        }
    }

    /** Interface names used by Android soft-AP (hotspot) implementations. */
    fun isHotspotInterface(name: String): Boolean = Regex("""(ap|swlan)\d+""").matches(name)
}
