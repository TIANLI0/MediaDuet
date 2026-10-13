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

/** Resolves the link session id for a `hello` (§7). Nothing is mutated until auth succeeds. */
fun interface SessionResolver {
    /** Returns (sid, resumed). */
    fun resolve(resumeSid: String?, peerId: String): Pair<String, Boolean>
}

sealed interface HandshakeResult {
    class Session(
        val peer: DeviceInfo,
        val version: Int,
        val sid: String,
        val resumed: Boolean,
        val peerCaps: Set<String>,
        val claimedTransport: String?,
    ) : HandshakeResult

    class Paired(val peer: DeviceInfo) : HandshakeResult
    class Failed(val reason: String) : HandshakeResult
}

/**
 * Server (S) side of the pairing (§3.3) and session (§4) handshakes for one connection.
 * Any violation sends the matching error / pair.result and closes the connection.
 */
class ServerHandshake(
    private val localDevice: () -> DeviceInfo,
    private val trust: TrustStore,
    private val pairing: PairingController,
    private val sessions: SessionResolver,
    private val capabilities: () -> List<String>,
    private val clock: MonoClock,
    private val wallClock: () -> Long = System::currentTimeMillis,
    private val handshakeTimeoutMs: Long = HANDSHAKE_TIMEOUT_MS,
    private val pairingTimeoutMs: Long = PairingController.ATTEMPT_TIMEOUT_MS,
) {
    private var nextId = 1L

    /** Abort with an optional `error` (session) or `pair.result` (pairing) message. */
    private class Abort(
        val reason: String,
        val errorCode: String? = null,
        val re: Long? = null,
        val pairReason: String? = null,
    ) : Exception(reason)

    suspend fun run(conn: Connection): HandshakeResult {
        try {
            val first = withTimeoutOrNull(handshakeTimeoutMs) { receive(conn, null) }
                ?: throw Abort("timeout waiting for first message")
            return when (first.type) {
                MsgType.PAIR_REQUEST -> pair(conn, first)
                MsgType.HELLO -> session(conn, first)
                MsgType.ERROR -> throw Abort("peer error before handshake")
                else -> throw Abort("unexpected ${first.type}", ErrorCode.BAD_MESSAGE, first.id)
            }
        } catch (a: Abort) {
            MdLog.w(TAG, "$conn handshake aborted: ${a.reason}")
            runCatching {
                when {
                    a.pairReason != null -> conn.sendHandshake(
                        envelope(conn, MsgType.PAIR_RESULT, buildJsonObject {
                            put("ok", false)
                            put("reason", a.pairReason)
                        }, null),
                    )
                    a.errorCode != null -> conn.sendHandshake(
                        envelope(conn, MsgType.ERROR, ErrorInfo(a.errorCode).toJson(), a.re),
                    )
                }
            }
            conn.closeAfterFlush("handshake: ${a.reason}")
            return HandshakeResult.Failed(a.reason)
        } catch (e: ClosedReceiveChannelException) {
            conn.close("closed during handshake")
            return HandshakeResult.Failed("connection closed")
        }
    }

    // --------------------------------------------------------------------------------------
    // Session handshake (§4)
    // --------------------------------------------------------------------------------------

    private suspend fun session(conn: Connection, hello: Envelope): HandshakeResult {
        val started = clock.nowMs()
        val body = hello.body
        val parsed = try {
            val device = DeviceInfo.parse(body.requireObject("device"))
            if (device.id != hello.src) throw BadMessageException("device.id != src")
            val protocol = body.requireObject("protocol")
            val min = protocol.requireLong("min")
            val max = protocol.requireLong("max")
            val caps = body.optStringList("caps", 64, 64) ?: emptyList()
            val epkC = body.requireBase64("epk", 65)
            val nC = body.requireBase64("nonce", 32)
            val transport = body.optString("transport", 16)
            val resume = body.optString("resume", 64)?.takeIf { Hex.isLowerHex(it, 32) }
            HelloFields(device, min, max, caps.toSet(), epkC, nC, transport, resume)
        } catch (e: BadMessageException) {
            throw Abort("bad hello: ${e.message}", ErrorCode.BAD_MESSAGE, hello.id)
        }
        val k = trust.key(hello.src) ?: throw Abort("not paired: ${hello.src}", ErrorCode.NOT_PAIRED, hello.id)
        val version = SUPPORTED_VERSIONS.filter { it >= parsed.min && it <= parsed.max }.maxOrNull()
            ?: throw Abort("no common version", ErrorCode.VERSION_UNSUPPORTED, hello.id)
        val peerKey = try {
            P256.decodePublic(parsed.epkC)
        } catch (e: GeneralSecurityException) {
            throw Abort("bad epk", ErrorCode.BAD_MESSAGE, hello.id)
        }
        val me = localDevice()
        val kp = P256.generate()
        val epkS = P256.encodePublic(kp.public)
        val nS = Rand.bytes(32)
        val z = P256.ecdh(kp.private, peerKey)
        val th = KeySchedule.sessionTranscript(parsed.nC, nS, parsed.epkC, epkS, hello.src, me.id)
        val (sid, resumed) = sessions.resolve(parsed.resume, hello.src)
        conn.sendHandshake(envelope(conn, MsgType.HELLO_ACK, buildJsonObject {
            put("device", me.toJson())
            put("protocol", version)
            put("caps", JsonArray(capabilities().map { JsonPrimitive(it) }))
            put("epk", B64.encode(epkS))
            put("nonce", B64.encode(nS))
            put("sid", sid)
            put("resumed", resumed)
            put("mac", B64.encode(KeySchedule.serverMac(k, th)))
        }, hello.id))

        val remaining = handshakeTimeoutMs - (clock.nowMs() - started)
        val auth = withTimeoutOrNull(remaining.coerceAtLeast(1)) { receive(conn, hello.src) }
            ?: throw Abort("auth timeout", ErrorCode.TIMEOUT)
        when (auth.type) {
            MsgType.AUTH -> Unit
            MsgType.ERROR -> throw Abort("peer error: ${auth.body}")
            else -> throw Abort("expected auth, got ${auth.type}", ErrorCode.BAD_MESSAGE, auth.id)
        }
        val mac = try {
            auth.body.requireBase64("mac", 32)
        } catch (e: BadMessageException) {
            throw Abort("bad auth", ErrorCode.AUTH_FAILED, auth.id)
        }
        if (!constantTimeEquals(mac, KeySchedule.clientMac(k, th))) {
            throw Abort("auth mac mismatch", ErrorCode.AUTH_FAILED, auth.id)
        }
        val keys = KeySchedule.session(k, z, th)
        conn.installKeys(
            keys,
            sealer = RecordCipher(keys.keyS2c, keys.ivS2c, FrameType.SECURE, encrypt = true),
            opener = RecordCipher(keys.keyC2s, keys.ivC2s, FrameType.SECURE, encrypt = false),
        )
        return HandshakeResult.Session(parsed.device, version, sid, resumed, parsed.caps, parsed.transport)
    }

    private class HelloFields(
        val device: DeviceInfo,
        val min: Long,
        val max: Long,
        val caps: Set<String>,
        val epkC: ByteArray,
        val nC: ByteArray,
        val transport: String?,
        val resume: String?,
    )

    // --------------------------------------------------------------------------------------
    // Pairing (§3.3)
    // --------------------------------------------------------------------------------------

    private suspend fun pair(conn: Connection, request: Envelope): HandshakeResult {
        val started = clock.nowMs()
        val peer: DeviceInfo
        val epkC: ByteArray
        val nC: ByteArray
        try {
            peer = DeviceInfo.parse(request.body.requireObject("device"))
            if (peer.id != request.src) throw BadMessageException("device.id != src")
            epkC = request.body.requireBase64("epk", 65)
            nC = request.body.requireBase64("nonce", 32)
        } catch (e: BadMessageException) {
            throw Abort("bad pair.request: ${e.message}", ErrorCode.BAD_MESSAGE, request.id)
        }
        if (!pairing.isOpen()) throw Abort("pairing disabled", pairReason = "pairing_disabled")
        if (pairing.isBusy()) throw Abort("pairing busy", pairReason = "busy")
        val peerKey = try {
            P256.decodePublic(epkC)
        } catch (e: GeneralSecurityException) {
            throw Abort("bad epk", ErrorCode.BAD_MESSAGE, request.id)
        }
        val me = localDevice()
        val kp = P256.generate()
        val epkS = P256.encodePublic(kp.public)
        val nS = Rand.bytes(32)
        val z = P256.ecdh(kp.private, peerKey)
        val th = KeySchedule.pairingTranscript(nC, nS, epkC, epkS, peer.id, me.id)
        val secrets = KeySchedule.pairing(z, th)

        val attempt = pairing.begin(peer, secrets.sas, conn.kind)
            ?: throw Abort("pairing unavailable", pairReason = if (pairing.isOpen()) "busy" else "pairing_disabled")
        var success = false
        var failReason: String? = "aborted"
        try {
            conn.sendHandshake(envelope(conn, MsgType.PAIR_RESPONSE, buildJsonObject {
                put("device", me.toJson())
                put("epk", B64.encode(epkS))
                put("nonce", B64.encode(nS))
            }, request.id))

            var localAccepted = false
            var peerConfirmed = false
            val deadline = started + pairingTimeoutMs
            while (!(localAccepted && peerConfirmed)) {
                val remaining = deadline - clock.nowMs()
                if (remaining <= 0) throw Abort("pairing timeout", pairReason = "timeout").also { failReason = "timeout" }
                val event = withTimeoutOrNull(remaining) {
                    select<PairEvent> {
                        if (!localAccepted) attempt.decision.onAwait { PairEvent.Decision(it) }
                        conn.frames.onReceiveCatching { r ->
                            val f = r.getOrNull()
                            if (f == null) PairEvent.Closed else PairEvent.Message(f)
                        }
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
                    }
                    PairEvent.Closed -> {
                        failReason = "connection_lost"
                        conn.close("closed during pairing")
                        return HandshakeResult.Failed("connection closed during pairing")
                    }
                    is PairEvent.Message -> {
                        val env = parseHandshake(event.frame, peer.id)
                        when (env.type) {
                            MsgType.PAIR_CONFIRM -> {
                                if (peerConfirmed) throw Abort("duplicate confirm", ErrorCode.BAD_MESSAGE, env.id)
                                val mac = try {
                                    env.body.requireBase64("mac", 32)
                                } catch (e: BadMessageException) {
                                    failReason = "mac_mismatch"
                                    throw Abort("bad confirm", pairReason = "mac_mismatch")
                                }
                                if (!constantTimeEquals(mac, KeySchedule.confirmMac(secrets.pairingKey, "C", th))) {
                                    failReason = "mac_mismatch"
                                    throw Abort("confirm mac mismatch", pairReason = "mac_mismatch")
                                }
                                peerConfirmed = true
                            }
                            MsgType.PAIR_RESULT -> {
                                val ok = runCatching { env.body.optBool("ok") }.getOrNull() ?: false
                                failReason = if (ok) "unexpected_result" else
                                    runCatching { env.body.optString("reason", 64) }.getOrNull() ?: "rejected"
                                conn.closeAfterFlush("peer ended pairing")
                                return HandshakeResult.Failed("peer ended pairing: $failReason")
                            }
                            MsgType.ERROR -> {
                                failReason = "peer_error"
                                conn.closeAfterFlush("peer error during pairing")
                                return HandshakeResult.Failed("peer error during pairing")
                            }
                            else -> throw Abort("unexpected ${env.type} during pairing", ErrorCode.BAD_MESSAGE, env.id)
                        }
                    }
                }
            }

            conn.sendHandshake(envelope(conn, MsgType.PAIR_CONFIRM, buildJsonObject {
                put("mac", B64.encode(KeySchedule.confirmMac(secrets.pairingKey, "S", th)))
            }, null))
            val now = wallClock()
            trust.add(TrustedPeer(peer.id, peer.name, peer.platform, now, now), secrets.pairingKey)
            conn.sendHandshake(envelope(conn, MsgType.PAIR_RESULT, buildJsonObject { put("ok", true) }, null))
            conn.closeAfterFlush("paired")
            success = true
            failReason = null
            MdLog.i(TAG, "paired with ${peer.name} (${peer.id})")
            return HandshakeResult.Paired(peer)
        } catch (e: CancellationException) {
            failReason = "cancelled"
            throw e
        } finally {
            attempt.finish(success, failReason)
        }
    }

    private sealed interface PairEvent {
        class Decision(val accepted: Boolean) : PairEvent
        class Message(val frame: app.mediaduet.protocol.Frame) : PairEvent
        object Closed : PairEvent
    }

    // --------------------------------------------------------------------------------------

    private suspend fun receive(conn: Connection, expectedSrc: String?): Envelope =
        parseHandshake(conn.frames.receive(), expectedSrc)

    private fun parseHandshake(frame: app.mediaduet.protocol.Frame, expectedSrc: String?): Envelope {
        if (frame.type != FrameType.HANDSHAKE) throw Abort("non-handshake frame during handshake")
        val env = try {
            EnvelopeCodec.decode(frame.body, Limits.MAX_HANDSHAKE_BODY)
        } catch (e: BadMessageException) {
            throw Abort("bad handshake message: ${e.message}", ErrorCode.BAD_MESSAGE, e.re)
        }
        if (env.v != 1) throw Abort("handshake v must be 1", ErrorCode.VERSION_UNSUPPORTED, env.id)
        if (env.sid.isNotEmpty()) throw Abort("handshake sid must be empty", ErrorCode.BAD_MESSAGE, env.id)
        if (expectedSrc != null && env.src != expectedSrc) throw Abort("src changed", ErrorCode.BAD_MESSAGE, env.id)
        return env
    }

    private fun envelope(conn: Connection, type: String, body: JsonObject, re: Long?): Envelope =
        Envelope(1, type, nextId++, wallClock(), localDevice().id, "", re, body)

    companion object {
        private const val TAG = "MD/Handshake"
        const val HANDSHAKE_TIMEOUT_MS = 10_000L
        val SUPPORTED_VERSIONS = listOf(1)
    }
}
