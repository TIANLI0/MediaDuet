package app.mediaduet.protocol

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class EnvelopeTest {
    private val src = "0123456789abcdef0123456789abcdef"
    private val sid = "00112233445566778899aabbccddeeff"

    private fun json(fields: String) = """{"v":1,"type":"ping","id":5,"ts":1760000000000,"src":"$src","sid":"$sid",$fields}"""

    private fun decode(text: String, max: Int = 64 * 1024) = EnvelopeCodec.decode(text.toByteArray(), max)

    @Test
    fun validEnvelopeWithUnknownFields() {
        val e = decode(json(""""re":4,"body":{"t":1,"future":[1,2]},"extra":"x""""))
        assertEquals("ping", e.type)
        assertEquals(5L, e.id)
        assertEquals(4L, e.re)
        assertEquals(sid, e.sid)
    }

    @Test
    fun handshakeEnvelopeWithEmptySid() {
        val e = decode("""{"v":1,"type":"hello","id":1,"ts":0,"src":"$src","sid":"","body":{}}""")
        assertEquals("", e.sid)
        assertNull(e.re)
    }

    private fun assertBad(text: String, expectedRe: Long? = null) {
        val ex = assertThrows(BadMessageException::class.java) { decode(text) }
        assertEquals(expectedRe, ex.re)
    }

    @Test
    fun rejectsTypeViolations() {
        assertBad("""{"v":1,"type":"ping","id":"5","ts":0,"src":"$src","sid":"","body":{}}""")
        assertBad("""{"v":1,"type":"ping","id":5.5,"ts":0,"src":"$src","sid":"","body":{}}""")
        assertBad("""{"v":1,"type":"ping","id":0,"ts":0,"src":"$src","sid":"","body":{}}""")
        assertBad("""{"v":1,"type":"ping","id":9007199254740992,"ts":0,"src":"$src","sid":"","body":{}}""")
        assertBad("""{"v":"1","type":"ping","id":5,"ts":0,"src":"$src","sid":"","body":{}}""", 5)
        assertBad("""{"v":1,"type":"Ping","id":5,"ts":0,"src":"$src","sid":"","body":{}}""", 5)
        assertBad("""{"v":1,"type":"${"a".repeat(49)}","id":5,"ts":0,"src":"$src","sid":"","body":{}}""", 5)
        assertBad("""{"v":1,"type":"ping","id":5,"ts":0,"src":"${src.uppercase()}","sid":"","body":{}}""", 5)
        assertBad("""{"v":1,"type":"ping","id":5,"ts":0,"src":"$src","sid":"abc","body":{}}""", 5)
        assertBad("""{"v":1,"type":"ping","id":5,"ts":0,"src":"$src","sid":"","body":[]}""", 5)
        assertBad("""{"v":1,"type":"ping","id":5,"ts":0,"src":"$src","sid":""}""", 5)
        assertBad("""{"v":1,"type":"ping","id":5,"ts":0,"src":"$src","sid":"","re":"x","body":{}}""", 5)
        assertBad("""[1,2]""")
        assertBad("""{"v":1,""")
    }

    @Test
    fun maxIdIsAccepted() {
        val e = decode("""{"v":1,"type":"ping","id":9007199254740991,"ts":0,"src":"$src","sid":"","body":{}}""")
        assertEquals(EnvelopeCodec.MAX_ID, e.id)
    }

    @Test
    fun rejectsInvalidUtf8AndOversize() {
        val bytes = json(""""body":{"x":"AB"}""").toByteArray()
        val idx = bytes.indexOf('A'.code.toByte())
        bytes[idx] = 0xC3.toByte()
        bytes[idx + 1] = 0x28
        assertThrows(BadMessageException::class.java) { EnvelopeCodec.decode(bytes, 64 * 1024) }
        val ex = assertThrows(BadMessageException::class.java) { decode(json(""""body":{}""").padEnd(100), 50) }
        assertEquals(ErrorCode.TOO_LARGE, ex.code)
    }

    @Test
    fun stringLimitsUseCodePoints() {
        val emoji = "🎵" // one code point, two UTF-16 units
        val device = Json.parseToJsonElement(
            """{"id":"$src","name":"${emoji.repeat(64)}","platform":"windows"}""",
        ).jsonObject
        assertEquals(128, DeviceInfo.parse(device).name.length)
        val tooLong = Json.parseToJsonElement(
            """{"id":"$src","name":"${"x".repeat(65)}","platform":"windows"}""",
        ).jsonObject
        assertThrows(BadMessageException::class.java) { DeviceInfo.parse(tooLong) }
        assertEquals(3, "abcdef".truncateCodePoints(3).length)
        assertEquals(emoji, (emoji + emoji).truncateCodePoints(1))
    }

    @Test
    fun snapshotRoundTripAndLenientEnums() {
        val s = MediaSnapshot(
            gen = 3, active = "a#1",
            sessions = listOf(
                MediaSessionInfo(
                    "a#1", "a", "App", "T", "Ar", "Al", "", TrackId.of("T", "Ar", "Al", 1000), 1000, 10, 1.0,
                    PlayStatus.PLAYING, listOf(Action.PLAY, Action.PAUSE), ArtworkRef("ab".repeat(32), "image/jpeg", 10),
                    MediaKind.MUSIC,
                ),
            ),
        )
        assertEquals(s, MediaSnapshot.parse(s.toJson()))
        val lenient = Json.parseToJsonElement(
            """{"gen":1,"active":null,"sessions":[{"key":"k","status":"weird","actions":["play","fly"],"kind":"podcast","durationMs":null}]}""",
        ).jsonObject
        val parsed = MediaSnapshot.parse(lenient)
        assertEquals(PlayStatus.UNKNOWN, parsed.sessions[0].status)
        assertEquals(listOf(Action.PLAY), parsed.sessions[0].actions)
        assertEquals(MediaKind.OTHER, parsed.sessions[0].kind)
        assertNull(parsed.sessions[0].durationMs)
        val tooMany = Json.parseToJsonElement(
            """{"gen":1,"sessions":[${(1..17).joinToString(",") { """{"key":"k$it"}""" }}]}""",
        ).jsonObject
        assertThrows(BadMessageException::class.java) { MediaSnapshot.parse(tooMany) }
    }

    @Test
    fun commandValidation() {
        val seek = Json.parseToJsonElement("""{"key":null,"action":"seek","positionMs":60000,"origin":"policy"}""").jsonObject
        val c = MediaCommand.parse(seek)
        assertNull(c.key)
        assertEquals(MediaCommand.ORIGIN_POLICY, c.origin)
        val badSeek = Json.parseToJsonElement("""{"action":"seek"}""").jsonObject
        assertThrows(BadMessageException::class.java) { MediaCommand.parse(badSeek) }
        assertTrue(CommandResult.parse(CommandResult.failure("x").toJson()).error!!.code == "x")
    }
}
