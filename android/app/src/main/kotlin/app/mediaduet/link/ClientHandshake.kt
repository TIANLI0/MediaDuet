package app.mediaduet.link

import app.mediaduet.crypto.KeySchedule
import app.mediaduet.crypto.P256
import app.mediaduet.crypto.Rand
import app.mediaduet.crypto.RecordCipher
import app.mediaduet.crypto.constantTimeEquals
import app.mediaduet.protocol.B64
import app.mediaduet.protocol.BadMessageException
import app.mediaduet.protocol.DeviceInfo
import app.mediaduet.protocol.Envelope
import app.mediaduet.protocol.EnvelopeCodec
import app.mediaduet.protocol.ErrorCode
import app.mediaduet.protocol.ErrorInfo
import app.mediaduet.protocol.Frame
import app.mediaduet.protocol.FrameType
import app.mediaduet.protocol.Hex
import app.mediaduet.protocol.Limits
import app.mediaduet.protocol.MsgType
import app.mediaduet.protocol.optBool
import app.mediaduet.protocol.optString
import app.mediaduet.protocol.optStringList
import app.mediaduet.protocol.requireBase64
import app.mediaduet.protocol.requireLong
import app.mediaduet.protocol.requireObject
import app.mediaduet.protocol.requireString
import app.mediaduet.util.MdLog
import app.mediaduet.util.MonoClock
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.channels.ClosedReceiveChannelException
import kotlinx.coroutines.selects.select
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.security.GeneralSecurityException

/**
 * Client (C) side of the pairing (§3.3) and session (§4) handshakes for one connection. Android
 * takes this role towards another Android device (§13); Windows has its own implementation.
 */
class ClientHandshake(
    private val localDevice: () -> DeviceInfo,
    private val trust: TrustStore,
    private val pairing: PairingController,
    private val capabilities: () -> List<String>,
    private val clock: MonoClock,
    private val wallClock: () -> Long = System::currentTimeMillis,
    private val handshakeTimeoutMs: Long = ServerHandshake.HANDSHAKE_TIMEOUT_MS,
    private val pairingTimeoutMs: Long = PairingController.ATTEMPT_TIMEOUT_MS,
) {
    private var nextId = 1L

    private class Abort(val reason: String, val errorCode: String? = null, val re: Long? = null, val pairReason: String? = null) : Exception(reason)

    /** `hello` → `hello.ack` → `auth` with the paired [peerId]; [resume] a previous session id. */
    suspend fun session(conn: Connection, peerId: String, resume: String?): HandshakeResult = guarded(conn) {
        val k = trust.key(peerId) ?: throw Abort("not paired: $peerId")
        val me = localDevice()
        val kp = P256.generate()
        val epkC = P256.encodePublic(kp.public)
        val nC = Rand.bytes(32)
        val hello = envelope(MsgType.HELLO, buildJsonObject {
            put("device", me.toJson())
            put("protocol", buildJsonObject {
                put("min", ServerHandshake.SUPPORTED_VERSIONS.min())
                put("max", ServerHandshake.SUPPORTED_VERSIONS.max())
            })
            put("caps", JsonArray(capabilities().map { JsonPrimitive(it) }))
            put("epk", B64.encode(epkC))
            put("nonce", B64.encode(nC))
            put("transport", conn.kind.wire)
            if (resume != null) put("resume", resume)
        }, null)
        conn.sendHandshake(hello)
        val ack = withTimeoutOrNull(handshakeTimeoutMs) { receive(conn, peerId) } ?: throw Abort("hello.ack timeout")
        when (ack.type) {
            MsgType.HELLO_ACK -> Unit
            MsgType.ERROR -> {
                val code = runCatching { ErrorInfo.parse(ack.body).code }.getOrDefault(ErrorCode.INTERNAL)
                conn.closeAfterFlush("peer error $code")
                return@guarded HandshakeResult.Failed(code)
            }
            else -> throw Abort("expected hello.ack, got ${ack.type}", ErrorCode.BAD_MESSAGE, ack.id)
        }
        val body = ack.body
        val fields = try {
            val device = DeviceInfo.parse(body.requireObject("device"))
            if (device.id != peerId) throw BadMessageException("unexpected device")
            val version = body.requireLong("protocol").toInt()
            if (version !in ServerHandshake.SUPPORTED_VERSIONS) throw BadMessageException("version $version")
            val sid = body.requireString("sid", 64)
            if (!Hex.isLowerHex(sid, 32)) throw BadMessageException("bad sid")
            AckFields(
                device, version, sid,
                resumed = body.optBool("resumed") ?: false,
                caps = (body.optStringList("caps", 64, 64) ?: emptyList()).toSet(),
                epkS = body.requireBase64("epk", 65),
                nS = body.requireBase64("nonce", 32),
                mac = body.requireBase64("mac", 32),
            )
        } catch (e: BadMessageException) {
            throw Abort("bad hello.ack: ${e.message}", ErrorCode.BAD_MESSAGE, ack.id)
        }
        val peerKey = try {
            P256.decodePublic(fields.epkS)
        } catch (e: GeneralSecurityException) {
            throw Abort("bad epk", ErrorCode.BAD_MESSAGE, ack.id)
        }
        val th = KeySchedule.sessionTranscript(nC, fields.nS, epkC, fields.epkS, me.id, peerId)
        if (!constantTimeEquals(fields.mac, KeySchedule.serverMac(k, th))) throw Abort("server mac mismatch", ErrorCode.AUTH_FAILED, ack.id)
        val z = P256.ecdh(kp.private, peerKey)
        conn.sendHandshake(envelope(MsgType.AUTH, buildJsonObject { put("mac", B64.encode(KeySchedule.clientMac(k, th))) }, null))
        val keys = KeySchedule.session(k, z, th)
        conn.installKeys(
            keys,
            sealer = RecordCipher(keys.keyC2s, keys.ivC2s, FrameType.SECURE, encrypt = true),
            opener = RecordCipher(keys.keyS2c, keys.ivS2c, FrameType.SECURE, encrypt = false),
        )
        HandshakeResult.Session(fields.device, fields.version, fields.sid, fields.resumed, fields.caps, conn.kind.wire)
    }

    private class AckFields(
        val device: DeviceInfo,
        val version: Int,
        val sid: String,
        val resumed: Boolean,
        val caps: Set<String>,
        val epkS: ByteArray,
        val nS: ByteArray,
        val mac: ByteArray,
    )

    /**
     * Pairs with the server on [conn] (it must have pairing mode open). The SAS is shown through
     * [pairing] like an incoming request; the key is stored once both sides confirmed.
     */
    suspend fun pair(conn: Connection): HandshakeResult = guarded(conn) {
        val started = clock.nowMs()
        val me = localDevice()
        val kp = P256.generate()
        val epkC = P256.encodePublic(kp.public)
        val nC = Rand.bytes(32)
        conn.sendHandshake(envelope(MsgType.PAIR_REQUEST, buildJsonObject {
            put("device", me.toJson())
            put("epk", B64.encode(epkC))
            put("nonce", B64.encode(nC))
        }, null))
        val response = withTimeoutOrNull(handshakeTimeoutMs) { receive(conn, null) } ?: throw Abort("pair.response timeout")
        when (response.type) {
            MsgType.PAIR_RESPONSE -> Unit
            MsgType.PAIR_RESULT -> {
                val reason = runCatching { response.body.optString("reason", 64) }.getOrNull() ?: "rejected"
                conn.closeAfterFlush("pairing refused")
                return@guarded HandshakeResult.Failed(reason)
            }
            MsgType.ERROR -> {
                conn.closeAfterFlush("peer error")
                return@guarded HandshakeResult.Failed("peer_error")
            }
            else -> throw Abort("unexpected ${response.type}", ErrorCode.BAD_MESSAGE, response.id)
        }
        val peer: DeviceInfo
        val epkS: ByteArray
        val nS: ByteArray
        try {
            peer = DeviceInfo.parse(response.body.requireObject("device"))
            if (peer.id != response.src || peer.id == me.id) throw BadMessageException("bad device")
            epkS = response.body.requireBase64("epk", 65)
            nS = response.body.requireBase64("nonce", 32)
        } catch (e: BadMessageException) {
            throw Abort("bad pair.response: ${e.message}", ErrorCode.BAD_MESSAGE, response.id)
        }
        val peerKey = try {
            P256.decodePublic(epkS)
        } catch (e: GeneralSecurityException) {
            throw Abort("bad epk", ErrorCode.BAD_MESSAGE, response.id)
        }
        val th = KeySchedule.pairingTranscript(nC, nS, epkC, epkS, me.id, peer.id)
        val secrets = KeySchedule.pairing(P256.ecdh(kp.private, peerKey), th)
        val attempt = pairing.beginOutgoing(peer, secrets.sas, conn.kind) ?: throw Abort("pairing busy", pairReason = "busy")
        var success = false
        var stored = false
        var failReason: String? = "aborted"
        try {
            var localAccepted = false
            var peerConfirmed = false
            val deadline = started + pairingTimeoutMs
            var result: HandshakeResult? = null
            while (result == null) {
                val remaining = deadline - clock.nowMs()
                if (remaining <= 0) {
                    failReason = "timeout"
                    throw Abort("pairing timeout", pairReason = "timeout")
                }
                val event = withTimeoutOrNull(remaining) {
                    select<PairEvent> {
                        if (!localAccepted) attempt.decision.onAwait { PairEvent.Decision(it) }
                        conn.frames.onReceiveCatching { r -> r.getOrNull()?.let { PairEvent.Message(it) } ?: PairEvent.Closed }
                    }
                } ?: run {
                    failReason = "timeout"
                    throw Abort("pairing timeout", pairReason = "timeout")
                }
                when (event) {
                    is PairEvent.Decision -> {
                        if (!event.accepted) {
                            failReason = "rejected"
                            throw Abort("rejected locally", pairReason = "rejected")
                        }
                        localAccepted = true
                        conn.sendHandshake(envelope(MsgType.PAIR_CONFIRM, buildJsonObject {
                            put("mac", B64.encode(KeySchedule.confirmMac(secrets.pairingKey, "C", th)))
                        }, null))
                    }
                    PairEvent.Closed -> {
                        failReason = "connection_lost"
                        conn.close("closed during pairing")
                        result = HandshakeResult.Failed("connection_lost")
                    }
                    is PairEvent.Message -> {
                        val env = parseHandshake(event.frame, peer.id)
                        when (env.type) {
                            MsgType.PAIR_CONFIRM -> {
                                // The server confirms only after our confirm and its user's accept.
                                if (!localAccepted || peerConfirmed) throw Abort("unexpected confirm", ErrorCode.BAD_MESSAGE, env.id)
                                val mac = runCatching { env.body.requireBase64("mac", 32) }.getOrNull()
                                if (mac == null || !constantTimeEquals(mac, KeySchedule.confirmMac(secrets.pairingKey, "S", th))) {
                                    failReason = "mac_mismatch"
                                    throw Abort("confirm mac mismatch", pairReason = "mac_mismatch")
                                }
                                peerConfirmed = true
                                val now = wallClock()
                                trust.add(TrustedPeer(peer.id, peer.name, peer.platform, now, now, peer.app), secrets.pairingKey)
                                stored = true
                            }
                            MsgType.PAIR_RESULT -> {
                                val ok = runCatching { env.body.optBool("ok") }.getOrNull() ?: false
                                conn.closeAfterFlush("pairing finished")
                                if (ok && peerConfirmed) {
                                    success = true
                                    failReason = null
                                    MdLog.i(TAG, "paired with ${peer.name} (${peer.id})")
                                    result = HandshakeResult.Paired(peer)
                                } else {
                                    failReason = if (ok) "unexpected_result" else runCatching { env.body.optString("reason", 64) }.getOrNull() ?: "rejected"
                                    result = HandshakeResult.Failed(failReason ?: "rejected")
                                }
                            }
                            MsgType.ERROR -> {
                                failReason = "peer_error"
                                conn.closeAfterFlush("peer error during pairing")
                                result = HandshakeResult.Failed("peer_error")
                            }
                            else -> throw Abort("unexpected ${env.type} during pairing", ErrorCode.BAD_MESSAGE, env.id)
                        }
                    }
                }
            }
            result
        } catch (e: CancellationException) {
            failReason = "cancelled"
            throw e
        } finally {
            // A key stored before the server's final result is dropped again when pairing failed.
            if (stored && !success) trust.remove(peer.id)
            attempt.finish(success, failReason)
        }
    }

    private sealed interface PairEvent {
        class Decision(val accepted: Boolean) : PairEvent
        class Message(val frame: Frame) : PairEvent
        object Closed : PairEvent
    }

    private suspend fun guarded(conn: Connection, block: suspend () -> HandshakeResult): HandshakeResult = try {
        block()
    } catch (a: Abort) {
        MdLog.w(TAG, "$conn client handshake aborted: ${a.reason}")
        runCatching {
            when {
                a.pairReason != null -> conn.sendHandshake(envelope(MsgType.PAIR_RESULT, buildJsonObject {
                    put("ok", false)
                    put("reason", a.pairReason)
                }, null))
                a.errorCode != null -> conn.sendHandshake(envelope(MsgType.ERROR, ErrorInfo(a.errorCode).toJson(), a.re))
            }
        }
        conn.closeAfterFlush("handshake: ${a.reason}")
        HandshakeResult.Failed(a.pairReason ?: a.reason)
    } catch (e: ClosedReceiveChannelException) {
        conn.close("closed during handshake")
        HandshakeResult.Failed("connection_lost")
    }

    private suspend fun receive(conn: Connection, expectedSrc: String?): Envelope =
        parseHandshake(conn.frames.receive(), expectedSrc)

    private fun parseHandshake(frame: Frame, expectedSrc: String?): Envelope {
        if (frame.type != FrameType.HANDSHAKE) throw Abort("non-handshake frame during handshake")
        val env = try {
            EnvelopeCodec.decode(frame.body, Limits.MAX_HANDSHAKE_BODY)
        } catch (e: BadMessageException) {
            throw Abort("bad handshake message: ${e.message}", ErrorCode.BAD_MESSAGE, e.re)
        }
        if (env.v != 1) throw Abort("handshake v must be 1", ErrorCode.VERSION_UNSUPPORTED, env.id)
        if (env.sid.isNotEmpty()) throw Abort("handshake sid must be empty", ErrorCode.BAD_MESSAGE, env.id)
        if (expectedSrc != null && env.src != expectedSrc) throw Abort("unexpected src", ErrorCode.BAD_MESSAGE, env.id)
        return env
    }

    private fun envelope(type: String, body: JsonObject, re: Long?): Envelope =
        Envelope(1, type, nextId++, wallClock(), localDevice().id, "", re, body)

    companion object {
        private const val TAG = "MD/ClientHs"
    }
}
