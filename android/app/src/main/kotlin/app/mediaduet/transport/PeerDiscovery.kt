package app.mediaduet.transport

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import app.mediaduet.protocol.Hex
import app.mediaduet.util.MdLog
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import java.net.Inet4Address
import java.net.URLDecoder

/** Another MediaDuet Android device found with DNS-SD (`_mediaduet._tcp`, §1.2). */
data class NearbyDevice(val id: String, val name: String, val host: String, val port: Int, val serviceName: String)

/**
 * Finds other Android devices that run MediaDuet (they all advertise `_mediaduet._tcp`) for pairing
 * and for reconnecting to paired ones (§13). Discovery runs only while someone holds it
 * ([acquire]): the Connection page, or a paired device that is missing.
 */
class PeerDiscovery(context: Context, private val selfId: String) {
    private val nsd = context.getSystemService(NsdManager::class.java)
    private val _devices = MutableStateFlow<List<NearbyDevice>>(emptyList())
    val devices: StateFlow<List<NearbyDevice>> = _devices

    private val holders = HashSet<String>()
    private var listener: NsdManager.DiscoveryListener? = null
    /** Services waiting to be resolved; resolved one at a time (older NsdManagers allow only one). */
    private val queue = ArrayDeque<NsdServiceInfo>()
    private var resolving = false

    @Synchronized
    fun acquire(holder: String) {
        if (holders.add(holder) && holders.size == 1) start()
    }

    @Synchronized
    fun release(holder: String) {
        if (holders.remove(holder) && holders.isEmpty()) stop()
    }

    /** Starts over (network changed: addresses may be stale). */
    @Synchronized
    fun restart() {
        if (listener == null) return
        stop()
        start()
    }

    private fun start() {
        val l = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) = MdLog.i(TAG, "discovery started")
            override fun onDiscoveryStopped(serviceType: String) = MdLog.i(TAG, "discovery stopped")
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) = MdLog.w(TAG, "discovery failed: $errorCode")
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) = Unit
            override fun onServiceFound(info: NsdServiceInfo) = enqueue(info)
            override fun onServiceLost(info: NsdServiceInfo) {
                _devices.update { list -> list.filter { it.serviceName != info.serviceName } }
            }
        }
        listener = l
        try {
            nsd.discoverServices(NsdAdvertiser.SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, l)
        } catch (e: Exception) {
            listener = null
            MdLog.w(TAG, "discoverServices threw", e)
        }
    }

    private fun stop() {
        val l = listener ?: return
        listener = null
        runCatching { nsd.stopServiceDiscovery(l) }
        queue.clear()
        _devices.value = emptyList()
    }

    @Synchronized
    private fun enqueue(info: NsdServiceInfo) {
        if (listener == null || queue.any { it.serviceName == info.serviceName }) return
        queue.addLast(info)
        resolveNext()
    }

    @Synchronized
    private fun resolveNext() {
        if (resolving) return
        val info = queue.removeFirstOrNull() ?: return
        resolving = true
        try {
            @Suppress("DEPRECATION") // registerServiceInfoCallback (API 34+) is not worth a second code path here
            nsd.resolveService(info, object : NsdManager.ResolveListener {
                override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) = done()
                override fun onServiceResolved(serviceInfo: NsdServiceInfo) {
                    found(serviceInfo)
                    done()
                }
            })
        } catch (e: Exception) {
            MdLog.w(TAG, "resolveService threw", e)
            resolving = false
        }
    }

    private fun done() {
        synchronized(this) { resolving = false }
        resolveNext()
    }

    private fun found(info: NsdServiceInfo) {
        val attrs = info.attributes
        val id = attrs["id"]?.toString(Charsets.UTF_8)?.takeIf { Hex.isLowerHex(it, 32) } ?: return
        if (id == selfId) return
        val name = attrs["n"]?.toString(Charsets.UTF_8)?.let { runCatching { URLDecoder.decode(it, "UTF-8") }.getOrNull() }
            ?.takeIf { it.isNotBlank() } ?: info.serviceName
        val host = hostOf(info) ?: return
        val device = NearbyDevice(id, name, host, info.port, info.serviceName)
        MdLog.i(TAG, "found $name at $host:${info.port}")
        _devices.update { list -> list.filter { it.id != id && it.serviceName != info.serviceName } + device }
    }

    private fun hostOf(info: NsdServiceInfo): String? {
        val addresses = if (Build.VERSION.SDK_INT >= 34) info.hostAddresses else {
            @Suppress("DEPRECATION")
            listOfNotNull(info.host)
        }
        // Prefer IPv4 (the advertiser binds IPv4 / dual stack and reports IPv4 in transport.info).
        return (addresses.firstOrNull { it is Inet4Address } ?: addresses.firstOrNull())?.hostAddress
    }

    companion object {
        private const val TAG = "MD/Discovery"
    }
}
