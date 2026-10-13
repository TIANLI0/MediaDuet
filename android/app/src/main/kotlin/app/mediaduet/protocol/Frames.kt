package app.mediaduet.protocol

import java.io.IOException

/** Frame types (§2, §9). */
object FrameType {
    const val HANDSHAKE = 0x01
    const val SECURE = 0x02
    const val AUDIO = 0x03
}

/** Secure record channels (§2.1). */
object Channel {
    const val CONTROL = 0x01
    const val BLOB = 0x02
}

object Limits {
    /** Max value of the length field (type byte + body). */
    const val MAX_FRAME_LENGTH = 262_144
    const val MAX_HANDSHAKE_BODY = 16 * 1024
    const val MAX_CONTROL_DATA = 64 * 1024
    const val MAX_BLOB_PAYLOAD = 32 * 1024
    const val BLOB_HEADER = 40
    const val MAX_ARTWORK_SIZE = 1024 * 1024
    const val GCM_TAG = 16
    const val MAX_LYRICS_TEXT = 256 * 1024
}

class Frame(val type: Int, val body: ByteArray) {
    override fun toString(): String = "Frame(type=$type, ${body.size} B)"
}

class FrameException(message: String) : IOException(message)

object FrameCodec {
    fun encode(type: Int, body: ByteArray): ByteArray {
        val length = body.size + 1
        if (length > Limits.MAX_FRAME_LENGTH) throw FrameException("frame too large: $length")
        val out = ByteArray(4 + length)
        out[0] = (length ushr 24).toByte()
        out[1] = (length ushr 16).toByte()
        out[2] = (length ushr 8).toByte()
        out[3] = length.toByte()
        out[4] = type.toByte()
        System.arraycopy(body, 0, out, 5, body.size)
        return out
    }
}

/**
 * Incremental frame decoder. Accepts arbitrary partial reads, validates the length field and the
 * frame type as soon as the 5-byte header is complete (before buffering the body), and enforces
 * per-type body limits.
 */
class FrameDecoder(
    private val allowedTypes: Set<Int> = setOf(FrameType.HANDSHAKE, FrameType.SECURE),
    private val maxLength: Int = Limits.MAX_FRAME_LENGTH,
) {
    private val header = ByteArray(5)
    private var headerFill = 0
    private var body: ByteArray? = null
    private var bodyFill = 0
    private var currentType = 0

    /** Feeds bytes; returns every frame completed by them. Throws [FrameException] on violation. */
    fun feed(data: ByteArray, offset: Int = 0, length: Int = data.size): List<Frame> {
        val frames = ArrayList<Frame>(1)
        var pos = offset
        val end = offset + length
        while (pos < end) {
            val b = body
            if (b == null) {
                val take = minOf(5 - headerFill, end - pos)
                System.arraycopy(data, pos, header, headerFill, take)
                headerFill += take
                pos += take
                if (headerFill == 5) {
                    val len = ((header[0].toInt() and 0xff) shl 24) or
                        ((header[1].toInt() and 0xff) shl 16) or
                        ((header[2].toInt() and 0xff) shl 8) or
                        (header[3].toInt() and 0xff)
                    // A negative int here means the u32 exceeded 2^31 - also too large.
                    if (len < 1 || len > maxLength) throw FrameException("invalid frame length $len")
                    currentType = header[4].toInt() and 0xff
                    if (currentType !in allowedTypes) throw FrameException("unexpected frame type $currentType")
                    val bodyLen = len - 1
                    if (currentType == FrameType.HANDSHAKE && bodyLen > Limits.MAX_HANDSHAKE_BODY) {
                        throw FrameException("handshake frame too large: $bodyLen")
                    }
                    headerFill = 0
                    if (bodyLen == 0) {
                        frames.add(Frame(currentType, ByteArray(0)))
                    } else {
                        body = ByteArray(bodyLen)
                        bodyFill = 0
                    }
                }
            } else {
                val take = minOf(b.size - bodyFill, end - pos)
                System.arraycopy(data, pos, b, bodyFill, take)
                bodyFill += take
                pos += take
                if (bodyFill == b.size) {
                    frames.add(Frame(currentType, b))
                    body = null
                }
            }
        }
        return frames
    }

    /** True when no partial frame is buffered. */
    val isIdle: Boolean get() = headerFill == 0 && body == null
}

/** BLOB chunk payload (§8.6) - the bytes after the channel byte. */
class BlobChunk(val sha256: ByteArray, val total: Int, val offset: Int, val bytes: ByteArray) {
    fun encode(): ByteArray {
        val out = ByteArray(Limits.BLOB_HEADER + bytes.size)
        System.arraycopy(sha256, 0, out, 0, 32)
        putU32(out, 32, total)
        putU32(out, 36, offset)
        System.arraycopy(bytes, 0, out, Limits.BLOB_HEADER, bytes.size)
        return out
    }

    companion object {
        fun decode(data: ByteArray): BlobChunk {
            if (data.size < Limits.BLOB_HEADER) throw FrameException("short blob chunk")
            val payload = data.size - Limits.BLOB_HEADER
            if (payload > Limits.MAX_BLOB_PAYLOAD) throw FrameException("blob chunk too large")
            val total = getU32(data, 32)
            val offset = getU32(data, 36)
            if (total < 0 || total > Limits.MAX_ARTWORK_SIZE) throw FrameException("blob total out of range")
            if (offset < 0 || offset.toLong() + payload > total) throw FrameException("blob chunk out of range")
            return BlobChunk(data.copyOfRange(0, 32), total, offset, data.copyOfRange(Limits.BLOB_HEADER, data.size))
        }

        /** Splits [content] into chunks of at most 32 KiB. */
        fun split(sha256: ByteArray, content: ByteArray): List<BlobChunk> {
            if (content.isEmpty()) return listOf(BlobChunk(sha256, 0, 0, ByteArray(0)))
            val chunks = ArrayList<BlobChunk>()
            var offset = 0
            while (offset < content.size) {
                val end = minOf(content.size, offset + Limits.MAX_BLOB_PAYLOAD)
                chunks.add(BlobChunk(sha256, content.size, offset, content.copyOfRange(offset, end)))
                offset = end
            }
            return chunks
        }

        private fun putU32(out: ByteArray, at: Int, v: Int) {
            out[at] = (v ushr 24).toByte()
            out[at + 1] = (v ushr 16).toByte()
            out[at + 2] = (v ushr 8).toByte()
            out[at + 3] = v.toByte()
        }

        private fun getU32(b: ByteArray, at: Int): Int =
            ((b[at].toInt() and 0xff) shl 24) or ((b[at + 1].toInt() and 0xff) shl 16) or
                ((b[at + 2].toInt() and 0xff) shl 8) or (b[at + 3].toInt() and 0xff)
    }
}
