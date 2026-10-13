package app.mediaduet.audio

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import app.mediaduet.crypto.KeySchedule
import app.mediaduet.crypto.RecordCipher
import app.mediaduet.crypto.constantTimeEquals
import app.mediaduet.protocol.AudioOffer
import app.mediaduet.protocol.ErrorCode
import app.mediaduet.protocol.ErrorInfo
import app.mediaduet.protocol.FrameDecoder
import app.mediaduet.protocol.FrameException
import app.mediaduet.protocol.FrameType
import app.mediaduet.protocol.Hex
import app.mediaduet.util.MdLog
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.io.DataInputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.SocketTimeoutException
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * Experimental Windows→Android PCM receiver (§9). Threads, sockets and buffers exist only while a
 * stream is active. Audio arrives on its own TCP connection, never over RFCOMM.
 */
class AudioReceiver(
    private val scope: CoroutineScope,
    private val enabled: StateFlow<Boolean>,
    private val sendStop: (peerId: String, streamId: String, reason: String) -> Unit,
) {
    enum class Phase { IDLE, WAITING, STREAMING, ERROR }

    data class Status(
        val phase: Phase = Phase.IDLE,
        val streamId: String? = null,
        val sampleRate: Int = 0,
        val channels: Int = 0,
        val packets: Long = 0,
        val underruns: Int = 0,
        val dropped: Int = 0,
        val error: String? = null,
    )

    private val _status = MutableStateFlow(Status())
    val status: StateFlow<Status> = _status

    private class Stream(val peerId: String, val offer: AudioOffer, val server: ServerSocket) {
        @Volatile var stopped = false
        var job: Job? = null
        var player: Thread? = null
        // ~300 ms of 10 ms packets: bounds latency; older packets are dropped when full.
        val queue = ArrayBlockingQueue<ByteArray>(30)
        @Volatile var socket: java.net.Socket? = null
    }

    private var current: Stream? = null

    @Synchronized
    fun handleOffer(peerId: String, offer: AudioOffer, baseKey: ByteArray): JsonObject {
        fun refuse(code: String, message: String) = buildJsonObject {
            put("ok", false)
            put("error", ErrorInfo(code, message).toJson())
        }
        if (!enabled.value) return refuse(ErrorCode.UNSUPPORTED, "audio receive disabled")
        if (offer.direction != "windows_to_android" || offer.codec != "pcm_s16le" || offer.transport != "tcp") {
            return refuse(ErrorCode.UNSUPPORTED, "unsupported stream format")
        }
        if (offer.channels !in 1..2 || offer.sampleRate !in setOf(44_100, 48_000)) {
            return refuse(ErrorCode.UNSUPPORTED, "unsupported sample format")
        }
        current?.let { stopLocked(it, "replaced", notifyPeer = true) }
        val server = try {
            ServerSocket().apply {
                reuseAddress = true
                bind(InetSocketAddress(0), 1)
                soTimeout = ACCEPT_TIMEOUT_MS
            }
        } catch (e: Exception) {
            return refuse(ErrorCode.INTERNAL, "cannot open listener")
        }
        val stream = Stream(peerId, offer, server)
        current = stream
        val streamIdBytes = Hex.decode(offer.streamId)
        val key = KeySchedule.audioKey(baseKey, streamIdBytes)
        val iv = KeySchedule.audioIv(baseKey, streamIdBytes)
        _status.value = Status(Phase.WAITING, offer.streamId, offer.sampleRate, offer.channels)
        stream.job = scope.launch(Dispatchers.IO) { receive(stream, streamIdBytes, key, iv) }
        MdLog.i(TAG, "audio offer ${offer.streamId.take(8)} accepted on port ${server.localPort}")
        return buildJsonObject {
            put("ok", true)
            put("port", server.localPort)
        }
    }

    private fun receive(stream: Stream, streamId: ByteArray, key: ByteArray, iv: ByteArray) {
        try {
            val socket = stream.server.accept()
            stream.socket = socket
            runCatching { stream.server.close() }
            socket.tcpNoDelay = true
            socket.soTimeout = READ_TIMEOUT_MS
            val input = DataInputStream(socket.getInputStream())
            val presented = ByteArray(16)
            input.readFully(presented)
            if (!constantTimeEquals(presented, streamId)) throw FrameException("wrong streamId")
            val opener = RecordCipher(key, iv, FrameType.AUDIO, encrypt = false)
            val decoder = FrameDecoder(setOf(FrameType.AUDIO))
            startPlayer(stream)
            _status.update { it.copy(phase = Phase.STREAMING) }
            val buf = ByteArray(16 * 1024)
            while (!stream.stopped) {
                val n = input.read(buf)
                if (n < 0) break
                for (frame in decoder.feed(buf, 0, n)) {
                    val plain = opener.open(frame.body)
                    if (plain.size < 8) throw FrameException("short audio record")
                    val pcm = plain.copyOfRange(8, plain.size)
                    if (!stream.queue.offer(pcm)) {
                        // Bound latency: drop the oldest packet when the jitter buffer overflows.
                        stream.queue.poll()
                        stream.queue.offer(pcm)
                        _status.update { it.copy(dropped = it.dropped + 1) }
                    }
                    _status.update { it.copy(packets = it.packets + 1) }
                }
            }
            if (!stream.stopped) finish(stream, "eof", null)
        } catch (e: SocketTimeoutException) {
            if (!stream.stopped) finish(stream, "timeout", ERROR_TIMEOUT)
        } catch (e: FrameException) {
            if (!stream.stopped) finish(stream, "protocol_error", ERROR_PROTOCOL)
        } catch (e: java.security.GeneralSecurityException) {
            if (!stream.stopped) finish(stream, "protocol_error", ERROR_PROTOCOL)
        } catch (e: Exception) {
            MdLog.w(TAG, "audio stream failed", e)
            if (!stream.stopped) finish(stream, "error", ERROR_IO)
        }
    }

    private fun startPlayer(stream: Stream) {
        val offer = stream.offer
        val channelMask = if (offer.channels == 2) AudioFormat.CHANNEL_OUT_STEREO else AudioFormat.CHANNEL_OUT_MONO
        val bytesPerMs = offer.sampleRate * offer.channels * 2 / 1000
        val minBuffer = AudioTrack.getMinBufferSize(offer.sampleRate, channelMask, AudioFormat.ENCODING_PCM_16BIT)
        val track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build(),
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(offer.sampleRate)
                    .setChannelMask(channelMask)
                    .build(),
            )
            .setBufferSizeInBytes(maxOf(minBuffer * 2, bytesPerMs * 100))
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        val prebufferBytes = bytesPerMs * JITTER_MS
        stream.player = Thread({
            try {
                var playing = false
                val pending = ArrayDeque<ByteArray>()
                var pendingBytes = 0
                while (!stream.stopped) {
                    val packet = stream.queue.poll(UNDERRUN_MS, TimeUnit.MILLISECONDS)
                    if (packet == null) {
                        if (playing) {
                            track.pause()
                            playing = false
                            _status.update { it.copy(underruns = it.underruns + 1) }
                        }
                        continue
                    }
                    if (!playing) {
                        // Pre-fill the ~80 ms jitter buffer before (re)starting playback.
                        pending.addLast(packet)
                        pendingBytes += packet.size
                        if (pendingBytes < prebufferBytes) continue
                        track.play()
                        playing = true
                        while (pending.isNotEmpty()) {
                            val p = pending.removeFirst()
                            track.write(p, 0, p.size, AudioTrack.WRITE_BLOCKING)
                        }
                        pendingBytes = 0
                        continue
                    }
                    track.write(packet, 0, packet.size, AudioTrack.WRITE_BLOCKING)
                }
            } catch (e: Exception) {
                MdLog.w(TAG, "audio playback stopped", e)
            } finally {
                runCatching { track.stop() }
                track.release()
            }
        }, "MD-AudioPlayback").apply { start() }
    }

    private fun finish(stream: Stream, reason: String, error: String?) {
        synchronized(this) {
            if (current !== stream) return
            stopLocked(stream, reason, notifyPeer = true)
            if (error != null) _status.value = Status(Phase.ERROR, error = error)
        }
    }

    @Synchronized
    fun stop(streamId: String?, reason: String, notifyPeer: Boolean) {
        val s = current ?: return
        if (streamId != null && s.offer.streamId != streamId) return
        stopLocked(s, reason, notifyPeer)
    }

    @Synchronized
    fun stopForPeer(peerId: String) {
        val s = current ?: return
        if (s.peerId == peerId) stopLocked(s, "link_lost", notifyPeer = false)
    }

    private fun stopLocked(s: Stream, reason: String, notifyPeer: Boolean) {
        s.stopped = true
        runCatching { s.server.close() }
        runCatching { s.socket?.close() }
        s.job?.cancel()
        s.player?.interrupt()
        s.queue.clear()
        if (current === s) current = null
        val last = _status.value
        MdLog.i(TAG, "audio stream ${s.offer.streamId.take(8)}: ${last.packets} packets decrypted, ${last.underruns} underruns, ${last.dropped} dropped")
        _status.value = Status()
        if (notifyPeer) sendStop(s.peerId, s.offer.streamId, reason)
        MdLog.i(TAG, "audio stream ${s.offer.streamId.take(8)} stopped: $reason")
    }

    companion object {
        private const val TAG = "MD/Audio"
        const val JITTER_MS = 80
        const val UNDERRUN_MS = 200L
        const val ACCEPT_TIMEOUT_MS = 10_000
        const val READ_TIMEOUT_MS = 5_000
        const val ERROR_TIMEOUT = "timeout"
        const val ERROR_PROTOCOL = "protocol"
        const val ERROR_IO = "io"
    }
}
