package app.mediaduet.link

import app.mediaduet.crypto.KeySchedule
import app.mediaduet.crypto.RecordCipher
import app.mediaduet.protocol.Envelope
import app.mediaduet.protocol.Frame
import app.mediaduet.protocol.FrameCodec
import app.mediaduet.protocol.FrameDecoder
import app.mediaduet.protocol.FrameException
import app.mediaduet.protocol.FrameType
import app.mediaduet.protocol.Limits
import app.mediaduet.util.MdLog
import app.mediaduet.util.MonoClock
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.channels.ClosedSendChannelException
import kotlinx.coroutines.channels.ReceiveChannel
import kotlinx.coroutines.launch
import kotlinx.coroutines.selects.select
import java.io.InputStream
import java.io.OutputStream
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

enum class TransportKind(val wire: String) { BT("bt"), LAN("lan") }

/** A connected byte stream (RFCOMM socket, TCP socket, or an in-memory pipe in tests). */
interface TransportStream {
    val kind: TransportKind
    val remoteDescription: String
    val input: InputStream
    val output: OutputStream
    fun close()
}

class TrafficCounters {
    val bytesIn = AtomicLong()
    val bytesOut = AtomicLong()
    val messagesIn = AtomicLong()
    val messagesOut = AtomicLong()
    val blobChunksOut = AtomicLong()
    val blobChunksIn = AtomicLong()
    val protocolErrors = AtomicLong()
    val handshakesOk = AtomicLong()
    val handshakesFailed = AtomicLong()
}

/**
 * One transport connection. A reader coroutine decodes frames into [frames]; a writer coroutine
 * drains two queues and always prefers CONTROL records over BLOB records (§2.1), encrypting at
 * dequeue time so that sequence numbers follow wire order.
 */
class Connection(
    val stream: TransportStream,
    private val scope: CoroutineScope,
    private val counters: TrafficCounters,
    private val clock: MonoClock,
) {
    val id: Int = nextId.incrementAndGet()
    val kind: TransportKind get() = stream.kind
    val connectedAtMs: Long = clock.nowMs()

    private val incoming = Channel<Frame>(capacity = 32)
    val frames: ReceiveChannel<Frame> get() = incoming

    private sealed interface Out {
        class Plain(val frame: ByteArray) : Out
        class Secure(val channel: Int, val data: ByteArray) : Out
        class Close(val reason: String) : Out
    }

    private val control = Channel<Out>(Channel.UNLIMITED)
    private val blob = Channel<Out>(capacity = 4)

    @Volatile private var sealer: RecordCipher? = null
    private var opener: RecordCipher? = null

    /** Session keys of this connection (needed to derive audio stream keys, §9). */
    @Volatile var sessionKeys: KeySchedule.SessionKeys? = null
        private set

    @Volatile var lastInboundMs: Long = clock.nowMs()
        private set

    private val closedFlag = AtomicBoolean(false)
    val closed = CompletableDeferred<String>()
    val isClosed: Boolean get() = closedFlag.get()
    val hasKeys: Boolean get() = sealer != null

    fun start() {
        scope.launch(Dispatchers.IO) { readLoop() }
        scope.launch(Dispatchers.IO) { writeLoop() }
    }

    fun installKeys(keys: KeySchedule.SessionKeys, sealer: RecordCipher, opener: RecordCipher) {
        this.sessionKeys = keys
        this.opener = opener
        this.sealer = sealer
    }

    fun sendHandshake(envelope: Envelope) {
        val body = envelope.encode()
        if (body.size > Limits.MAX_HANDSHAKE_BODY) throw FrameException("handshake message too large")
        control.trySend(Out.Plain(FrameCodec.encode(FrameType.HANDSHAKE, body)))
    }

    /** Queues a CONTROL record. Returns false when the connection is closed. */
    fun sendControl(envelope: Envelope): Boolean {
        val data = envelope.encode()
        if (data.size > Limits.MAX_CONTROL_DATA) {
            MdLog.w(TAG, "#$id dropping oversized ${envelope.type} (${data.size} B)")
            return false
        }
        if (isClosed) return false
        counters.messagesOut.incrementAndGet()
        return control.trySend(Out.Secure(app.mediaduet.protocol.Channel.CONTROL, data)).isSuccess
    }

    /** Queues a BLOB record, suspending while the (small) blob queue is full. */
    suspend fun sendBlob(data: ByteArray): Boolean {
        if (isClosed) return false
        return try {
            blob.send(Out.Secure(app.mediaduet.protocol.Channel.BLOB, data))
            counters.blobChunksOut.incrementAndGet()
            true
        } catch (e: ClosedSendChannelException) {
            false
        }
    }

    /** Decrypts a SECURE frame body; returns (channel, data). Throws on any failure. */
    fun openRecord(frame: Frame): Pair<Int, ByteArray> {
        val o = opener ?: throw FrameException("SECURE frame before keys")
        val plain = o.open(frame.body)
        if (plain.isEmpty()) throw FrameException("empty record")
        return (plain[0].toInt() and 0xff) to plain.copyOfRange(1, plain.size)
    }

    /** Sends everything queued so far on the control queue, then closes. */
    fun closeAfterFlush(reason: String) {
        if (!control.trySend(Out.Close(reason)).isSuccess) close(reason)
    }

    fun close(reason: String) {
        if (!closedFlag.compareAndSet(false, true)) return
        MdLog.i(TAG, "#$id (${kind.wire} ${stream.remoteDescription}) closed: $reason")
        incoming.close()
        control.close()
        blob.close()
        runCatching { stream.close() }
        closed.complete(reason)
    }

    private suspend fun readLoop() {
        val decoder = FrameDecoder()
        val buffer = ByteArray(16 * 1024)
        try {
            val input = stream.input
            while (!isClosed) {
                val n = input.read(buffer)
                if (n < 0) {
                    close("eof")
                    return
                }
                counters.bytesIn.addAndGet(n.toLong())
                for (frame in decoder.feed(buffer, 0, n)) {
                    lastInboundMs = clock.nowMs()
                    // Suspends while the consumer is busy (back-pressure towards the peer).
                    incoming.send(frame)
                }
            }
        } catch (e: FrameException) {
            counters.protocolErrors.incrementAndGet()
            close("framing: ${e.message}")
        } catch (e: ClosedSendChannelException) {
            close("closed")
        } catch (e: java.io.IOException) {
            close("read: ${e.javaClass.simpleName}${e.message?.let { " $it" } ?: ""}")
        }
    }

    private suspend fun writeLoop() {
        val output = stream.output
        try {
            while (true) {
                val item: Out = control.tryReceive().getOrNull()
                    ?: blob.tryReceive().getOrNull()
                    ?: run {
                        output.flush()
                        select {
                            control.onReceiveCatching { it.getOrNull() ?: Out.Close("closed") }
                            blob.onReceiveCatching { it.getOrNull() ?: Out.Close("closed") }
                        }
                    }
                when (item) {
                    is Out.Plain -> write(output, item.frame)
                    is Out.Secure -> {
                        val s = sealer ?: throw FrameException("SECURE record before keys")
                        val plain = ByteArray(item.data.size + 1)
                        plain[0] = item.channel.toByte()
                        System.arraycopy(item.data, 0, plain, 1, item.data.size)
                        write(output, FrameCodec.encode(FrameType.SECURE, s.seal(plain)))
                    }
                    is Out.Close -> {
                        output.flush()
                        close(item.reason)
                        return
                    }
                }
            }
        } catch (e: Exception) {
            close("write: ${e.javaClass.simpleName}${e.message?.let { " $it" } ?: ""}")
        }
    }

    private fun write(output: OutputStream, bytes: ByteArray) {
        output.write(bytes)
        counters.bytesOut.addAndGet(bytes.size.toLong())
    }

    override fun toString(): String = "Connection#$id(${kind.wire} ${stream.remoteDescription})"

    companion object {
        private const val TAG = "MD/Conn"
        private val nextId = AtomicInteger()
    }
}
