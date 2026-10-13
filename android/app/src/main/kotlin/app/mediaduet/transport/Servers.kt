package app.mediaduet.transport

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothServerSocket
import android.bluetooth.BluetoothSocket
import app.mediaduet.link.TransportKind
import app.mediaduet.link.TransportStream
import app.mediaduet.util.MdLog
import java.io.InputStream
import java.io.OutputStream
import java.net.BindException
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.util.UUID

/** TCP listener on 47321 (falls back to an ephemeral port when busy, §1.2). */
class TcpServer(private val onAccept: (TransportStream) -> Unit) {
    @Volatile private var server: ServerSocket? = null
    @Volatile var port: Int? = null
        private set

    val isRunning: Boolean get() = server?.isClosed == false

    @Synchronized
    fun start(): Int {
        server?.takeIf { !it.isClosed }?.let { return it.localPort }
        val s = ServerSocket()
        s.reuseAddress = true
        try {
            s.bind(InetSocketAddress(DEFAULT_PORT), 16)
        } catch (e: BindException) {
            MdLog.w(TAG, "port $DEFAULT_PORT busy, using an ephemeral port")
            s.bind(InetSocketAddress(0), 16)
        }
        server = s
        port = s.localPort
        Thread({ acceptLoop(s) }, "MD-TcpAccept").apply { isDaemon = true }.start()
        MdLog.i(TAG, "TCP server listening on port ${s.localPort}")
        return s.localPort
    }

    private fun acceptLoop(s: ServerSocket) {
        while (!s.isClosed) {
            try {
                val client = s.accept()
                onAccept(SocketTransportStream(client, TransportKind.LAN))
            } catch (e: Exception) {
                if (!s.isClosed) MdLog.w(TAG, "accept failed", e)
            }
        }
    }

    @Synchronized
    fun stop() {
        runCatching { server?.close() }
        server = null
        port = null
    }

    companion object {
        private const val TAG = "MD/Tcp"
        const val DEFAULT_PORT = 47321
    }
}

/** Bluetooth RFCOMM stream (§1.1). */
class RfcommTransportStream(private val socket: BluetoothSocket) : TransportStream {
    override val kind = TransportKind.BT

    @SuppressLint("MissingPermission") // only constructed after BLUETOOTH_CONNECT was verified
    override val remoteDescription: String = runCatching { socket.remoteDevice?.name }.getOrNull()
        ?: runCatching { socket.remoteDevice?.address }.getOrNull() ?: "bluetooth"
    override val input: InputStream = socket.inputStream
    override val output: OutputStream = socket.outputStream

    override fun close() {
        runCatching { socket.close() }
    }
}

/** RFCOMM listener registered with the SDP record "MediaDuet" (secure, requires a bond). */
class RfcommServer(private val onAccept: (TransportStream) -> Unit) {
    @Volatile private var server: BluetoothServerSocket? = null

    val isRunning: Boolean get() = server != null

    /** Caller must have verified BLUETOOTH_CONNECT and that the adapter is enabled. */
    @SuppressLint("MissingPermission")
    @Synchronized
    fun start(adapter: BluetoothAdapter) {
        if (server != null) return
        val s = adapter.listenUsingRfcommWithServiceRecord(SERVICE_NAME, SERVICE_UUID)
        server = s
        Thread({ acceptLoop(s) }, "MD-RfcommAccept").apply { isDaemon = true }.start()
        MdLog.i(TAG, "RFCOMM server listening ($SERVICE_UUID)")
    }

    private fun acceptLoop(s: BluetoothServerSocket) {
        while (true) {
            try {
                val socket = s.accept()
                onAccept(RfcommTransportStream(socket))
            } catch (e: Exception) {
                // accept() throws when the adapter turns off or the server socket is closed.
                MdLog.i(TAG, "RFCOMM accept loop ended: ${e.message}")
                synchronized(this) { if (server === s) server = null }
                runCatching { s.close() }
                return
            }
        }
    }

    @Synchronized
    fun stop() {
        runCatching { server?.close() }
        server = null
    }

    companion object {
        private const val TAG = "MD/Rfcomm"
        const val SERVICE_NAME = "MediaDuet"
        val SERVICE_UUID: UUID = UUID.fromString("5f3a8c2e-7d41-4b9a-9c61-2e8d0f6b4a17")
    }
}
