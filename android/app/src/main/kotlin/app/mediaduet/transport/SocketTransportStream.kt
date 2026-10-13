package app.mediaduet.transport

import app.mediaduet.link.TransportKind
import app.mediaduet.link.TransportStream
import java.io.BufferedOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.Socket

/** TCP socket as a link transport. */
class SocketTransportStream(private val socket: Socket, override val kind: TransportKind = TransportKind.LAN) : TransportStream {
    init {
        socket.tcpNoDelay = true
        socket.keepAlive = true
    }

    override val remoteDescription: String =
        "${socket.inetAddress?.hostAddress ?: "?"}:${socket.port}"
    override val input: InputStream = socket.getInputStream()
    override val output: OutputStream = BufferedOutputStream(socket.getOutputStream(), 16 * 1024)

    override fun close() {
        runCatching { socket.close() }
    }
}
