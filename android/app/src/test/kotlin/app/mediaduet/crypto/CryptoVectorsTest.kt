package app.mediaduet.crypto

import app.mediaduet.protocol.BlobChunk
import app.mediaduet.protocol.EnvelopeCodec
import app.mediaduet.protocol.FrameCodec
import app.mediaduet.protocol.FrameDecoder
import app.mediaduet.protocol.FrameType
import app.mediaduet.protocol.Hex
import app.mediaduet.protocol.TrackId
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.File
import java.math.BigInteger
import java.security.GeneralSecurityException
import java.security.spec.ECFieldFp

/** Reproduces every value of protocol/test-vectors/crypto-v1.json. */
class CryptoVectorsTest {

    private val v: JsonObject = Json.parseToJsonElement(vectorsFile().readText(Charsets.UTF_8)).jsonObject

    private fun JsonObject.hex(name: String) = Hex.decode(this[name]!!.jsonPrimitive.content)
    private fun JsonObject.str(name: String) = this[name]!!.jsonPrimitive.content
    private fun obj(name: String) = v[name]!!.jsonObject

    private val ids = obj("ids")
    private val idC = ids.str("client")
    private val idS = ids.str("server")
    private val eph = obj("ephemeral")
    private val dC = eph.hex("client_private")
    private val dS = eph.hex("server_private")
    private val epkC = eph.hex("client_public")
    private val epkS = eph.hex("server_public")
    private val nC = eph.hex("client_nonce")
    private val nS = eph.hex("server_nonce")

    @Test
    fun publicKeysFromPrivateScalars() {
        assertArrayEquals(epkC, EcMath.publicFromScalar(dC))
        assertArrayEquals(epkS, EcMath.publicFromScalar(dS))
        // Round trip through JCA public key objects.
        assertArrayEquals(epkC, P256.encodePublic(P256.decodePublic(epkC)))
        assertArrayEquals(epkS, P256.encodePublic(P256.decodePublic(epkS)))
    }

    @Test
    fun ecdhSharedSecret() {
        val z = eph.hex("shared_secret")
        assertArrayEquals(z, P256.ecdh(P256.privateKeyFromScalar(dC), P256.decodePublic(epkS)))
        assertArrayEquals(z, P256.ecdh(P256.privateKeyFromScalar(dS), P256.decodePublic(epkC)))
    }

    @Test
    fun invalidPointsAreRejected() {
        val offCurve = epkC.copyOf().also { it[64] = (it[64] + 1).toByte() }
        assertThrows(GeneralSecurityException::class.java) { P256.decodePublic(offCurve) }
        val compressed = epkC.copyOf().also { it[0] = 0x02 }
        assertThrows(GeneralSecurityException::class.java) { P256.decodePublic(compressed) }
        assertThrows(GeneralSecurityException::class.java) { P256.decodePublic(epkC.copyOf(64)) }
    }

    @Test
    fun pairingSchedule() {
        val p = obj("pairing")
        val z = eph.hex("shared_secret")
        val th = KeySchedule.pairingTranscript(nC, nS, epkC, epkS, idC, idS)
        assertArrayEquals(p.hex("transcript_hash"), th)
        val secrets = KeySchedule.pairing(z, th)
        assertArrayEquals(p.hex("prk"), secrets.prk)
        assertEquals("410860", secrets.sas)
        assertEquals(p.str("sas"), secrets.sas)
        assertArrayEquals(p.hex("pairing_key"), secrets.pairingKey)
        assertArrayEquals(p.hex("confirm_mac_client"), KeySchedule.confirmMac(secrets.pairingKey, "C", th))
        assertArrayEquals(p.hex("confirm_mac_server"), KeySchedule.confirmMac(secrets.pairingKey, "S", th))
    }

    @Test
    fun sessionSchedule() {
        val s = obj("session")
        val k = s.hex("pairing_key")
        val z = eph.hex("shared_secret")
        val th = KeySchedule.sessionTranscript(nC, nS, epkC, epkS, idC, idS)
        assertArrayEquals(s.hex("transcript_hash"), th)
        assertArrayEquals(s.hex("mac_server"), KeySchedule.serverMac(k, th))
        assertArrayEquals(s.hex("mac_client"), KeySchedule.clientMac(k, th))
        val keys = KeySchedule.session(k, z, th)
        assertArrayEquals(s.hex("prk"), keys.prk)
        assertArrayEquals(s.hex("key_c2s"), keys.keyC2s)
        assertArrayEquals(s.hex("key_s2c"), keys.keyS2c)
        assertArrayEquals(s.hex("iv_c2s"), keys.ivC2s)
        assertArrayEquals(s.hex("iv_s2c"), keys.ivS2c)
    }

    @Test
    fun recordProtection() {
        val s = obj("session")
        val sealers = mapOf(
            "c2s" to RecordCipher(s.hex("key_c2s"), s.hex("iv_c2s"), FrameType.SECURE, encrypt = true),
            "s2c" to RecordCipher(s.hex("key_s2c"), s.hex("iv_s2c"), FrameType.SECURE, encrypt = true),
        )
        val openers = mapOf(
            "c2s" to RecordCipher(s.hex("key_c2s"), s.hex("iv_c2s"), FrameType.SECURE, encrypt = false),
            "s2c" to RecordCipher(s.hex("key_s2c"), s.hex("iv_s2c"), FrameType.SECURE, encrypt = false),
        )
        for (r in v["records"]!!.jsonArray.map { it.jsonObject }) {
            val dir = r.str("dir")
            val seq = r["seq"]!!.jsonPrimitive.longOrNull!!
            val sealer = sealers.getValue(dir)
            assertEquals(seq, sealer.seq)
            val ct = sealer.seal(r.hex("plaintext"))
            assertArrayEquals(r.hex("ciphertext"), ct)
            val frame = FrameCodec.encode(r["frame_type"]!!.jsonPrimitive.content.toInt(), ct)
            assertArrayEquals(r.hex("frame"), frame)
            val decoded = FrameDecoder().feed(frame).single()
            assertArrayEquals(r.hex("plaintext"), openers.getValue(dir).open(decoded.body))
        }
    }

    @Test
    fun tamperedOrReplayedRecordFails() {
        val s = obj("session")
        val rec = v["records"]!!.jsonArray[0].jsonObject
        val opener = RecordCipher(s.hex("key_c2s"), s.hex("iv_c2s"), FrameType.SECURE, encrypt = false)
        val tampered = rec.hex("ciphertext").also { it[3] = (it[3].toInt() xor 1).toByte() }
        assertThrows(GeneralSecurityException::class.java) { opener.open(tampered) }
        // Correct record but at the wrong sequence number (replay / reorder).
        val opener2 = RecordCipher(s.hex("key_c2s"), s.hex("iv_c2s"), FrameType.SECURE, encrypt = false)
        opener2.open(rec.hex("ciphertext"))
        assertThrows(GeneralSecurityException::class.java) { opener2.open(rec.hex("ciphertext")) }
    }

    @Test
    fun audioKeys() {
        val a = obj("audio")
        val base = obj("session").hex(a.str("base"))
        val streamId = a.hex("stream_id")
        val key = KeySchedule.audioKey(base, streamId)
        val iv = KeySchedule.audioIv(base, streamId)
        assertArrayEquals(a.hex("audio_key"), key)
        assertArrayEquals(a.hex("audio_iv"), iv)
        val sealer = RecordCipher(key, iv, FrameType.AUDIO, encrypt = true)
        assertArrayEquals(a.hex("ciphertext"), sealer.seal(a.hex("plaintext")))
        val opener = RecordCipher(key, iv, FrameType.AUDIO, encrypt = false)
        assertArrayEquals(a.hex("plaintext"), opener.open(a.hex("ciphertext")))
    }

    @Test
    fun blobChunk() {
        val b = obj("blob")
        val blob = ByteArray(1024) { (it % 256).toByte() }
        val hash = sha256(blob)
        assertArrayEquals(b.hex("content_sha256"), hash)
        val chunk = BlobChunk(hash, blob.size, 0, blob.copyOf(512))
        val plaintext = byteArrayOf(0x02) + chunk.encode()
        assertArrayEquals(b.hex("first_chunk_plaintext"), plaintext)
        val parsed = BlobChunk.decode(plaintext.copyOfRange(1, plaintext.size))
        assertEquals(1024, parsed.total)
        assertEquals(0, parsed.offset)
        assertArrayEquals(blob.copyOf(512), parsed.bytes)
    }

    @Test
    fun handshakeFrameAndCanonicalEncoding() {
        val h = obj("handshake_frame")
        val json = h.str("json")
        assertArrayEquals(h.hex("frame"), FrameCodec.encode(FrameType.HANDSHAKE, json.toByteArray()))
        val env = EnvelopeCodec.decode(json.toByteArray(), 16 * 1024)
        assertEquals("auth", env.type)
        assertEquals(json, String(env.encode(), Charsets.UTF_8))
    }

    @Test
    fun trackIds() {
        for (t in v["track_ids"]!!.jsonArray.map { it.jsonObject }) {
            val duration = t["durationMs"]!!.jsonPrimitive.longOrNull
            assertEquals(t.str("trackId"), TrackId.of(t.str("title"), t.str("artist"), t.str("album"), duration))
        }
    }

    companion object {
        fun vectorsFile(): File {
            val prop = System.getProperty("mediaduet.vectors")
            val candidates = listOfNotNull(
                prop?.let { File(it) },
                File("../../protocol/test-vectors/crypto-v1.json"),
                File("../protocol/test-vectors/crypto-v1.json"),
            )
            return candidates.firstOrNull { it.isFile }
                ?: error("crypto-v1.json not found (set -Dmediaduet.vectors)")
        }
    }
}

/** Independent affine P-256 scalar multiplication used to check public keys from scalars. */
object EcMath {
    private val params = P256.params
    private val p = (params.curve.field as ECFieldFp).p
    private val a = params.curve.a

    private data class Pt(val x: BigInteger, val y: BigInteger)

    private fun add(P: Pt?, Q: Pt?): Pt? {
        if (P == null) return Q
        if (Q == null) return P
        if (P.x == Q.x) {
            if ((P.y + Q.y).mod(p) == BigInteger.ZERO) return null
            val l = (P.x.pow(2).multiply(BigInteger.valueOf(3)).add(a))
                .multiply(P.y.shiftLeft(1).modInverse(p)).mod(p)
            val x = l.pow(2).subtract(P.x.shiftLeft(1)).mod(p)
            return Pt(x, l.multiply(P.x.subtract(x)).subtract(P.y).mod(p))
        }
        val l = Q.y.subtract(P.y).multiply(Q.x.subtract(P.x).modInverse(p)).mod(p)
        val x = l.pow(2).subtract(P.x).subtract(Q.x).mod(p)
        return Pt(x, l.multiply(P.x.subtract(x)).subtract(P.y).mod(p))
    }

    fun publicFromScalar(d: ByteArray): ByteArray {
        var k = BigInteger(1, d)
        var r: Pt? = null
        var q: Pt? = Pt(params.generator.affineX, params.generator.affineY)
        while (k.signum() > 0) {
            if (k.testBit(0)) r = add(r, q)
            q = add(q, q)
            k = k.shiftRight(1)
        }
        val out = ByteArray(65)
        out[0] = 4
        fun put(v: BigInteger, at: Int) {
            val raw = v.toByteArray().let { if (it.size > 32) it.copyOfRange(it.size - 32, it.size) else it }
            System.arraycopy(raw, 0, out, at + 32 - raw.size, raw.size)
        }
        put(r!!.x, 1)
        put(r.y, 33)
        return out
    }
}
