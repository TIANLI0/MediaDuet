package app.mediaduet.crypto

import java.nio.ByteBuffer

/** Key schedule of protocol v1 (§3.3, §4, §9). */
object KeySchedule {
    private fun ascii(s: String) = s.toByteArray(Charsets.US_ASCII)

    fun pairingTranscript(nC: ByteArray, nS: ByteArray, epkC: ByteArray, epkS: ByteArray, idC: String, idS: String): ByteArray =
        sha256(ascii("MediaDuet-pair-v1"), nC, nS, epkC, epkS, ascii(idC), ascii(idS))

    fun sessionTranscript(nC: ByteArray, nS: ByteArray, epkC: ByteArray, epkS: ByteArray, idC: String, idS: String): ByteArray =
        sha256(ascii("MediaDuet-session-v1"), nC, nS, epkC, epkS, ascii(idC), ascii(idS))

    class PairingSecrets(val prk: ByteArray, val sas: String, val pairingKey: ByteArray)

    fun pairing(sharedSecret: ByteArray, transcript: ByteArray): PairingSecrets {
        val prk = Hkdf.extract(transcript, sharedSecret)
        val sasRaw = ByteBuffer.wrap(Hkdf.expand(prk, ascii("sas"), 4)).int.toLong() and 0xffffffffL
        val sas = (sasRaw % 1_000_000L).toString().padStart(6, '0')
        return PairingSecrets(prk, sas, Hkdf.expand(prk, ascii("pairing key"), 32))
    }

    /** role is "C" or "S". */
    fun confirmMac(pairingKey: ByteArray, role: String, transcript: ByteArray): ByteArray =
        hmacSha256(pairingKey, ascii("confirm"), ascii(role), transcript)

    fun serverMac(pairingKey: ByteArray, transcript: ByteArray) = hmacSha256(pairingKey, ascii("server"), transcript)
    fun clientMac(pairingKey: ByteArray, transcript: ByteArray) = hmacSha256(pairingKey, ascii("client"), transcript)

    class SessionKeys(
        val prk: ByteArray,
        val keyC2s: ByteArray,
        val keyS2c: ByteArray,
        val ivC2s: ByteArray,
        val ivS2c: ByteArray,
    )

    fun session(pairingKey: ByteArray, sharedSecret: ByteArray, transcript: ByteArray): SessionKeys {
        val prk = Hkdf.extract(pairingKey, sharedSecret)
        return SessionKeys(
            prk = prk,
            keyC2s = Hkdf.expand(prk, ascii("c2s key") + transcript, 32),
            keyS2c = Hkdf.expand(prk, ascii("s2c key") + transcript, 32),
            ivC2s = Hkdf.expand(prk, ascii("c2s iv") + transcript, 4),
            ivS2c = Hkdf.expand(prk, ascii("s2c iv") + transcript, 4),
        )
    }

    fun audioKey(base: ByteArray, streamId: ByteArray): ByteArray = hmacSha256(base, ascii("audio key"), streamId)
    fun audioIv(base: ByteArray, streamId: ByteArray): ByteArray = hmacSha256(base, ascii("audio iv"), streamId).copyOf(4)
}
