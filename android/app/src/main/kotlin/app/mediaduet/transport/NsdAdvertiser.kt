package app.mediaduet.transport

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import app.mediaduet.util.MdLog
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.net.URLEncoder

/** DNS-SD advertisement `_mediaduet._tcp` with TXT v, id, n (§1.2). */
class NsdAdvertiser(context: Context) {
    enum class State { OFF, REGISTERING, REGISTERED, FAILED }

    private val nsd = context.getSystemService(NsdManager::class.java)
    private var listener: NsdManager.RegistrationListener? = null
    private val _state = MutableStateFlow(State.OFF)
    val state: StateFlow<State> = _state

    @Volatile var registeredName: String? = null
        private set

    @Synchronized
    fun register(port: Int, deviceId: String, deviceName: String) {
        unregister()
        val info = NsdServiceInfo().apply {
            serviceName = instanceName(deviceName)
            serviceType = SERVICE_TYPE
            setPort(port)
            setAttribute("v", "1")
            setAttribute("id", deviceId)
            setAttribute("p", "android")
            setAttribute("n", encodeTxtName(deviceName))
        }
        val l = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(serviceInfo: NsdServiceInfo) {
                registeredName = serviceInfo.serviceName
                _state.value = State.REGISTERED
                MdLog.i(TAG, "registered ${serviceInfo.serviceName} on port $port")
            }

            override fun onRegistrationFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
                _state.value = State.FAILED
                MdLog.w(TAG, "NSD registration failed: $errorCode")
            }

            override fun onServiceUnregistered(serviceInfo: NsdServiceInfo) {
                registeredName = null
            }

            override fun onUnregistrationFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
                MdLog.w(TAG, "NSD unregistration failed: $errorCode")
            }
        }
        listener = l
        _state.value = State.REGISTERING
        try {
            nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, l)
        } catch (e: Exception) {
            listener = null
            _state.value = State.FAILED
            MdLog.w(TAG, "NSD registerService threw", e)
        }
    }

    @Synchronized
    fun unregister() {
        val l = listener ?: return
        listener = null
        runCatching { nsd.unregisterService(l) }
        registeredName = null
        _state.value = State.OFF
    }

    companion object {
        private const val TAG = "MD/Nsd"
        const val SERVICE_TYPE = "_mediaduet._tcp"

        private fun instanceName(deviceName: String): String {
            // DNS-SD instance names are limited to 63 bytes of UTF-8.
            val sb = StringBuilder()
            var bytes = 0
            for (cp in deviceName.codePoints().toArray()) {
                val s = String(Character.toChars(cp))
                val n = s.toByteArray(Charsets.UTF_8).size
                if (bytes + n > 63) break
                sb.append(s)
                bytes += n
            }
            return sb.toString().ifBlank { "MediaDuet" }
        }

        /** URL-encoded name truncated to at most 40 bytes without splitting an escape sequence. */
        fun encodeTxtName(name: String): String {
            val sb = StringBuilder()
            for (cp in name.codePoints().toArray()) {
                val encoded = URLEncoder.encode(String(Character.toChars(cp)), "UTF-8").replace("+", "%20")
                if (sb.length + encoded.length > 40) break
                sb.append(encoded)
            }
            return sb.toString()
        }
    }
}
