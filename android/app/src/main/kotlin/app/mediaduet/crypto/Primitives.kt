package app.mediaduet.crypto

import java.math.BigInteger
import java.security.AlgorithmParameters
import java.security.GeneralSecurityException
import java.security.KeyFactory
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.PrivateKey
import java.security.PublicKey
import java.security.SecureRandom
import java.security.interfaces.ECPublicKey
import java.security.spec.ECFieldFp
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPrivateKeySpec
import java.security.spec.ECPublicKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

object Rand {
    private val random = SecureRandom()
    fun bytes(n: Int): ByteArray = ByteArray(n).also { random.nextBytes(it) }
}

/** NIST P-256 via JCA (SunEC on the JVM, Conscrypt on Android). */
object P256 {
    val params: ECParameterSpec by lazy {
        val ap = AlgorithmParameters.getInstance("EC")
        ap.init(ECGenParameterSpec("secp256r1"))
        ap.getParameterSpec(ECParameterSpec::class.java)
    }

    fun generate(): KeyPair {
        val g = KeyPairGenerator.getInstance("EC")
        g.initialize(ECGenParameterSpec("secp256r1"))
        return g.generateKeyPair()
    }

    fun privateKeyFromScalar(d: ByteArray): PrivateKey =
        KeyFactory.getInstance("EC").generatePrivate(ECPrivateKeySpec(BigInteger(1, d), params))

    /** SEC1 uncompressed encoding: 0x04 || X(32) || Y(32). */
    fun encodePublic(key: PublicKey): ByteArray {
        val w = (key as ECPublicKey).w
        val out = ByteArray(65)
        out[0] = 0x04
        toFixed(w.affineX, out, 1)
        toFixed(w.affineY, out, 33)
        return out
    }

    /** Decodes and validates (on-curve, in-range) an uncompressed point. */
    fun decodePublic(encoded: ByteArray): PublicKey {
        if (encoded.size != 65 || encoded[0] != 0x04.toByte()) throw GeneralSecurityException("not an uncompressed P-256 point")
        val x = BigInteger(1, encoded.copyOfRange(1, 33))
        val y = BigInteger(1, encoded.copyOfRange(33, 65))
        val curve = params.curve
        val p = (curve.field as ECFieldFp).p
        if (x >= p || y >= p) throw GeneralSecurityException("coordinate out of range")
        val lhs = y.multiply(y).mod(p)
        val rhs = x.pow(3).add(curve.a.multiply(x)).add(curve.b).mod(p)
        if (lhs != rhs) throw GeneralSecurityException("point not on curve")
        return KeyFactory.getInstance("EC").generatePublic(ECPublicKeySpec(ECPoint(x, y), params))
    }

    /** ECDH shared secret: the 32-byte big-endian X coordinate. */
    fun ecdh(privateKey: PrivateKey, peer: PublicKey): ByteArray {
        val ka = KeyAgreement.getInstance("ECDH")
        ka.init(privateKey)
        ka.doPhase(peer, true)
        val secret = ka.generateSecret()
        return when {
            secret.size == 32 -> secret
            secret.size < 32 -> ByteArray(32 - secret.size) + secret
            else -> throw GeneralSecurityException("unexpected ECDH output length ${secret.size}")
        }
    }

    private fun toFixed(v: BigInteger, out: ByteArray, at: Int) {
        val raw = v.toByteArray()
        val start = if (raw.size > 32) raw.size - 32 else 0
        val len = raw.size - start
        System.arraycopy(raw, start, out, at + 32 - len, len)
    }
}

fun sha256(vararg parts: ByteArray): ByteArray {
    val md = MessageDigest.getInstance("SHA-256")
    parts.forEach { md.update(it) }
    return md.digest()
}

fun hmacSha256(key: ByteArray, vararg parts: ByteArray): ByteArray {
    val mac = Mac.getInstance("HmacSHA256")
    mac.init(SecretKeySpec(key, "HmacSHA256"))
    parts.forEach { mac.update(it) }
    return mac.doFinal()
}

/** RFC 5869 HKDF-SHA256. */
object Hkdf {
    fun extract(salt: ByteArray, ikm: ByteArray): ByteArray = hmacSha256(salt, ikm)

    fun expand(prk: ByteArray, info: ByteArray, length: Int): ByteArray {
        require(length in 1..255 * 32)
        val out = ByteArray(length)
        var t = ByteArray(0)
        var filled = 0
        var counter = 1
        while (filled < length) {
            t = hmacSha256(prk, t, info, byteArrayOf(counter.toByte()))
            val n = minOf(t.size, length - filled)
            System.arraycopy(t, 0, out, filled, n)
            filled += n
            counter++
        }
        return out
    }
}

/** Constant-time comparison (MessageDigest.isEqual is time-constant for equal lengths). */
fun constantTimeEquals(a: ByteArray, b: ByteArray): Boolean = MessageDigest.isEqual(a, b)

/**
 * One direction of AES-256-GCM record protection (§5): nonce = iv4 || u64_be(seq),
 * AAD = "MDv1" || frameType. The sequence starts at 0 and must never wrap.
 */
class RecordCipher(key: ByteArray, iv4: ByteArray, frameType: Int, private val encrypt: Boolean) {
    private val keySpec = SecretKeySpec(key.copyOf(), "AES")
    private val iv = iv4.copyOf()
    private val aad = byteArrayOf('M'.code.toByte(), 'D'.code.toByte(), 'v'.code.toByte(), '1'.code.toByte(), frameType.toByte())
    private val cipher = Cipher.getInstance("AES/GCM/NoPadding")

    var seq: Long = 0
        private set

    init {
        require(key.size == 32) { "AES-256 key required" }
        require(iv4.size == 4) { "4-byte IV prefix required" }
    }

    fun nonce(sequence: Long): ByteArray {
        val n = ByteArray(12)
        System.arraycopy(iv, 0, n, 0, 4)
        for (i in 0 until 8) n[4 + i] = (sequence ushr (56 - 8 * i)).toByte()
        return n
    }

    /** Encrypts with the next sequence number; returns ciphertext || tag. */
    fun seal(plaintext: ByteArray): ByteArray {
        check(encrypt)
        if (seq == Long.MAX_VALUE) throw GeneralSecurityException("sequence exhausted")
        cipher.init(Cipher.ENCRYPT_MODE, keySpec, GCMParameterSpec(128, nonce(seq)))
        cipher.updateAAD(aad)
        val out = cipher.doFinal(plaintext)
        seq++
        return out
    }

    /** Decrypts the next record; any failure (tamper, replay, reorder, loss) throws. */
    fun open(record: ByteArray): ByteArray {
        check(!encrypt)
        if (seq == Long.MAX_VALUE) throw GeneralSecurityException("sequence exhausted")
        if (record.size < 16) throw GeneralSecurityException("record too short")
        cipher.init(Cipher.DECRYPT_MODE, keySpec, GCMParameterSpec(128, nonce(seq)))
        cipher.updateAAD(aad)
        val out = cipher.doFinal(record)
        seq++
        return out
    }
}
