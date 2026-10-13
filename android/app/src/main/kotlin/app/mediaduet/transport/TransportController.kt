package app.mediaduet.transport

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Build
import androidx.core.content.ContextCompat
import app.mediaduet.link.TransportStream
import app.mediaduet.protocol.LanEndpoint
import app.mediaduet.util.MdLog
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.net.Inet4Address
import java.net.NetworkInterface

enum class BluetoothListenState { UNSUPPORTED, NO_PERMISSION, OFF, LISTENING, ERROR }

data class TransportState(
    val bluetooth: BluetoothListenState = BluetoothListenState.OFF,
    val lanPort: Int? = null,
    val lanAddresses: List<String> = emptyList(),
    /** Android 17+ ACCESS_LOCAL_NETWORK (always true below API 37). */
    val localNetworkAllowed: Boolean = true,
    val nsd: NsdAdvertiser.State = NsdAdvertiser.State.OFF,
    val lanError: String? = null,
)

/**
 * Owns the listeners. Restarts RFCOMM on Bluetooth on/off, re-advertises NSD and re-announces
 * LAN endpoints on network changes. Every failure degrades to a status, never a crash.
 */
class TransportController(
    private val context: Context,
    private val scope: CoroutineScope,
    private val deviceId: String,
    private val deviceName: () -> String,
    onAccept: (TransportStream) -> Unit,
    private val onLanChanged: () -> Unit,
) {
    private val tcp = TcpServer(onAccept)
    private val rfcomm = RfcommServer(onAccept)
    private val nsd = NsdAdvertiser(context)
    private val cm = context.getSystemService(ConnectivityManager::class.java)
    private val btManager = context.getSystemService(BluetoothManager::class.java)

    private val _state = MutableStateFlow(TransportState())
    val state: StateFlow<TransportState> = _state

    private var started = false
    private var lanRefreshJob: Job? = null

    private val btReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == BluetoothAdapter.ACTION_STATE_CHANGED) refreshBluetooth()
        }
    }

    private val networkCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) = scheduleLanRefresh()
        override fun onLost(network: Network) = scheduleLanRefresh()
        override fun onLinkPropertiesChanged(network: Network, linkProperties: LinkProperties) = scheduleLanRefresh()
    }

    init {
        scope.launch { nsd.state.collect { s -> _state.update { it.copy(nsd = s) } } }
    }

    @Synchronized
    fun start() {
        if (started) return
        started = true
        ContextCompat.registerReceiver(
            context, btReceiver, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED), ContextCompat.RECEIVER_NOT_EXPORTED,
        )
        try {
            val request = NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET)
                .build()
            cm.registerNetworkCallback(request, networkCallback)
        } catch (e: Exception) {
            MdLog.w(TAG, "network callback unavailable", e)
        }
        startTcp()
        refreshBluetooth()
        refreshLan()
    }

    @Synchronized
    fun stop() {
        if (!started) return
        started = false
        runCatching { context.unregisterReceiver(btReceiver) }
        runCatching { cm.unregisterNetworkCallback(networkCallback) }
        nsd.unregister()
        tcp.stop()
        rfcomm.stop()
        _state.value = TransportState()
    }

    /** Re-evaluates permissions (called when the UI resumes or a permission was granted). */
    fun refresh() {
        if (!started) return
        startTcp()
        refreshBluetooth()
        refreshLan()
    }

    fun onDeviceNameChanged() {
        if (started) refreshLan(forceNsd = true)
    }

    private fun startTcp() {
        try {
            val port = tcp.start()
            _state.update { it.copy(lanPort = port, lanError = null) }
        } catch (e: Exception) {
            MdLog.e(TAG, "TCP server failed", e)
            _state.update { it.copy(lanPort = null, lanError = e.javaClass.simpleName) }
        }
    }

    fun hasBluetoothPermission(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

    /** The Bluetooth adapter name (needs BLUETOOTH_CONNECT on API 31+), null when unavailable. */
    @android.annotation.SuppressLint("MissingPermission") // guarded by hasBluetoothPermission()
    fun bluetoothName(): String? {
        if (!hasBluetoothPermission()) return null
        return try {
            btManager?.adapter?.name
        } catch (e: SecurityException) {
            null
        }
    }

    fun hasLocalNetworkPermission(): Boolean =
        Build.VERSION.SDK_INT < 37 ||
            ContextCompat.checkSelfPermission(context, PERMISSION_LOCAL_NETWORK) == PackageManager.PERMISSION_GRANTED

    @Synchronized
    private fun refreshBluetooth() {
        if (!started) return
        val adapter = btManager?.adapter
        val newState = when {
            adapter == null -> BluetoothListenState.UNSUPPORTED
            !hasBluetoothPermission() -> BluetoothListenState.NO_PERMISSION
            else -> try {
                if (adapter.isEnabled) {
                    rfcomm.start(adapter)
                    BluetoothListenState.LISTENING
                } else {
                    BluetoothListenState.OFF
                }
            } catch (e: SecurityException) {
                BluetoothListenState.NO_PERMISSION
            } catch (e: Exception) {
                MdLog.w(TAG, "RFCOMM listen failed", e)
                BluetoothListenState.ERROR
            }
        }
        if (newState != BluetoothListenState.LISTENING) rfcomm.stop()
        val changed = _state.value.bluetooth != newState
        _state.update { it.copy(bluetooth = newState) }
        // btName in transport.info depends on Bluetooth state and permission.
        if (changed) onLanChanged()
    }

    private fun scheduleLanRefresh() {
        lanRefreshJob?.cancel()
        lanRefreshJob = scope.launch {
            delay(1_500)
            refreshLan()
        }
    }

    private var advertisedKey: String? = null

    @Synchronized
    private fun refreshLan(forceNsd: Boolean = false) {
        if (!started) return
        val addresses = lanAddresses(cm)
        val allowed = hasLocalNetworkPermission()
        val port = tcp.port
        val previous = _state.value
        _state.update { it.copy(lanAddresses = addresses, localNetworkAllowed = allowed, lanPort = port) }
        val key = "$port|$allowed|${addresses.joinToString()}|${deviceName()}"
        if (port != null && allowed && addresses.isNotEmpty()) {
            if (forceNsd || key != advertisedKey) {
                nsd.register(port, deviceId, deviceName())
                advertisedKey = key
            }
        } else {
            nsd.unregister()
            advertisedKey = null
        }
        if (previous.lanAddresses != addresses) onLanChanged()
    }

    fun lanEndpoints(): List<LanEndpoint> {
        val s = _state.value
        val port = s.lanPort ?: return emptyList()
        if (!s.localNetworkAllowed) return emptyList()
        return s.lanAddresses.map { LanEndpoint(it, port) }
    }

    companion object {
        private const val TAG = "MD/Transports"
        const val PERMISSION_LOCAL_NETWORK = "android.permission.ACCESS_LOCAL_NETWORK"

        /** Interface addresses of the phone's hotspot (soft-AP interfaces are not CM networks). */
        private fun hotspotAddresses(): List<String> = try {
            NetworkInterface.getNetworkInterfaces()?.toList().orEmpty()
                .filter { runCatching { it.isUp && LanAddressPolicy.isHotspotInterface(it.name) }.getOrDefault(false) }
                .flatMap { it.inetAddresses.toList() }
                .filterIsInstance<Inet4Address>()
                .mapNotNull { it.hostAddress }
        } catch (e: Exception) {
            emptyList()
        }

        /** IPv4 addresses of Wi-Fi / Ethernet networks (no VPN / cellular), default Wi-Fi first. */
        @Suppress("DEPRECATION") // allNetworks: still the simplest complete enumeration
        fun lanAddresses(cm: ConnectivityManager?): List<String> = try {
            val default = cm?.activeNetwork
            val nets = cm?.allNetworks.orEmpty().mapNotNull { n ->
                val caps = cm?.getNetworkCapabilities(n) ?: return@mapNotNull null
                val props = cm.getLinkProperties(n) ?: return@mapNotNull null
                NetCandidate(
                    wifi = caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI),
                    ethernet = caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET),
                    vpn = caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN) || !caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN),
                    cellular = caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR),
                    isDefault = n == default,
                    addresses = props.linkAddresses.map { it.address }.filterIsInstance<Inet4Address>().mapNotNull { it.hostAddress },
                )
            }
            LanAddressPolicy.pick(nets, hotspotAddresses())
        } catch (e: Exception) {
            MdLog.w(TAG, "cannot enumerate networks: $e")
            emptyList()
        }
    }
}
