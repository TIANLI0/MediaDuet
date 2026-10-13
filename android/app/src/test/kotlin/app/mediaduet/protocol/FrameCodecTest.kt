package app.mediaduet.protocol

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class FrameCodecTest {

    @Test
    fun roundTripWithByteByByteFeeding() {
        val bodies = listOf(byteArrayOf(1, 2, 3), ByteArray(0), ByteArray(70_000) { it.toByte() })
        val stream = bodies.flatMap { FrameCodec.encode(FrameType.SECURE, it).toList() }.toByteArray()
        val decoder = FrameDecoder()
        val frames = mutableListOf<Frame>()
        for (b in stream) frames += decoder.feed(byteArrayOf(b))
        assertEquals(3, frames.size)
        bodies.forEachIndexed { i, body -> assertArrayEquals(body, frames[i].body) }
        assertTrue(decoder.isIdle)
    }

    @Test
    fun multipleFramesInOneReadAndSplitAcrossReads() {
        val f1 = FrameCodec.encode(FrameType.HANDSHAKE, "{}".toByteArray())
        val f2 = FrameCodec.encode(FrameType.SECURE, ByteArray(100) { 7 })
        val all = f1 + f2
        val decoder = FrameDecoder()
        // First read: whole f1 plus 3 bytes of f2's header.
        val first = decoder.feed(all, 0, f1.size + 3)
        assertEquals(1, first.size)
        assertEquals(FrameType.HANDSHAKE, first[0].type)
        val second = decoder.feed(all, f1.size + 3, all.size - f1.size - 3)
        assertEquals(1, second.size)
        assertEquals(100, second[0].body.size)
    }

    @Test
    fun maximumLengthIsAccepted() {
        val body = ByteArray(Limits.MAX_FRAME_LENGTH - 1)
        val frames = FrameDecoder().feed(FrameCodec.encode(FrameType.SECURE, body))
        assertEquals(body.size, frames.single().body.size)
    }

    @Test
    fun oversizedFrameIsRejectedFromHeaderAlone() {
        val header = byteArrayOf(0x00, 0x04, 0x00, 0x01, FrameType.SECURE.toByte()) // 262145
        assertThrows(FrameException::class.java) { FrameDecoder().feed(header) }
        assertThrows(FrameException::class.java) { FrameCodec.encode(FrameType.SECURE, ByteArray(Limits.MAX_FRAME_LENGTH)) }
        val huge = byteArrayOf(0xff.toByte(), 0xff.toByte(), 0xff.toByte(), 0xff.toByte(), 2)
        assertThrows(FrameException::class.java) { FrameDecoder().feed(huge) }
    }

    @Test
    fun zeroLengthIsRejected() {
        assertThrows(FrameException::class.java) { FrameDecoder().feed(byteArrayOf(0, 0, 0, 0, 1)) }
    }

    @Test
    fun unknownTypeIsRejected() {
        assertThrows(FrameException::class.java) { FrameDecoder().feed(FrameCodec.encode(0x05, byteArrayOf(1))) }
        // AUDIO frames are only allowed on audio connections.
        assertThrows(FrameException::class.java) { FrameDecoder().feed(FrameCodec.encode(FrameType.AUDIO, byteArrayOf(1))) }
        assertEquals(1, FrameDecoder(setOf(FrameType.AUDIO)).feed(FrameCodec.encode(FrameType.AUDIO, byteArrayOf(1))).size)
    }

    @Test
    fun handshakeLimit() {
        val ok = FrameCodec.encode(FrameType.HANDSHAKE, ByteArray(Limits.MAX_HANDSHAKE_BODY))
        assertEquals(1, FrameDecoder().feed(ok).size)
        val tooBig = FrameCodec.encode(FrameType.HANDSHAKE, ByteArray(Limits.MAX_HANDSHAKE_BODY + 1))
        assertThrows(FrameException::class.java) { FrameDecoder().feed(tooBig.copyOf(5)) }
    }

    @Test
    fun blobChunkValidation() {
        val hash = ByteArray(32) { 1 }
        val chunks = BlobChunk.split(hash, ByteArray(70_000) { it.toByte() })
        assertEquals(3, chunks.size)
        assertEquals(65_536, chunks[2].offset)
        val decoded = BlobChunk.decode(chunks[1].encode())
        assertEquals(32_768, decoded.offset)
        assertEquals(70_000, decoded.total)
        // offset + length beyond total
        val bad = BlobChunk(hash, 10, 5, ByteArray(10)).encode()
        assertThrows(FrameException::class.java) { BlobChunk.decode(bad) }
        // total beyond 1 MiB
        val tooBig = BlobChunk(hash, Limits.MAX_ARTWORK_SIZE + 1, 0, ByteArray(1)).encode()
        assertThrows(FrameException::class.java) { BlobChunk.decode(tooBig) }
        assertThrows(FrameException::class.java) { BlobChunk.decode(ByteArray(39)) }
    }
}
