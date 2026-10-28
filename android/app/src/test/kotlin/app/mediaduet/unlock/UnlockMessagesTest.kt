package app.mediaduet.unlock

import app.mediaduet.protocol.B64
import app.mediaduet.protocol.BadMessageException
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class UnlockMessagesTest {
    private fun obj(text: String) = Json.parseToJsonElement(text).jsonObject

    private val enrollment = "0123456789abcdef0123456789abcdef"
    private val key = ByteArray(32) { it.toByte() }
    private val keyB64 = B64.encode(key)

    @Test
    fun enrollParsesAndValidates() {
        val e = UnlockEnroll.parse(obj("""{"enrollment":"$enrollment","key":"$keyB64","pc":"DESKTOP-1","extra":1}"""))
        assertEquals(enrollment, e.enrollment)
        assertArrayEquals(key, e.key)
        assertEquals("DESKTOP-1", e.pc)
        // Key: base64 of exactly 32 bytes.
        assertThrows(BadMessageException::class.java) {
            UnlockEnroll.parse(obj("""{"enrollment":"$enrollment","key":"${B64.encode(ByteArray(31))}","pc":"x"}"""))
        }
        assertThrows(BadMessageException::class.java) { UnlockEnroll.parse(obj("""{"enrollment":"$enrollment","key":"%%%","pc":"x"}""")) }
        assertThrows(BadMessageException::class.java) { UnlockEnroll.parse(obj("""{"enrollment":"$enrollment","pc":"x"}""")) }
        // Enrollment: 32 lower-case hex characters.
        assertThrows(BadMessageException::class.java) {
            UnlockEnroll.parse(obj("""{"enrollment":"${enrollment.uppercase()}","key":"$keyB64","pc":"x"}"""))
        }
        assertThrows(BadMessageException::class.java) { UnlockEnroll.parse(obj("""{"enrollment":"abc","key":"$keyB64","pc":"x"}""")) }
        // PC name: at most 64 code points.
        assertEquals(64, UnlockEnroll.parse(obj("""{"enrollment":"$enrollment","key":"$keyB64","pc":"${"电".repeat(64)}"}""")).pc.length)
        assertThrows(BadMessageException::class.java) {
            UnlockEnroll.parse(obj("""{"enrollment":"$enrollment","key":"$keyB64","pc":"${"a".repeat(65)}"}"""))
        }
    }

    @Test
    fun requestAndRevoke() {
        assertEquals(UnlockRequest(enrollment, "PC"), UnlockRequest.parse(obj("""{"enrollment":"$enrollment","pc":"PC"}""")))
        assertThrows(BadMessageException::class.java) { UnlockRequest.parse(obj("""{"enrollment":"$enrollment"}""")) }
        assertThrows(BadMessageException::class.java) { UnlockRequest.parse(obj("""{"enrollment":5,"pc":"PC"}""")) }
        assertEquals(enrollment, UnlockRevoke.parse(UnlockRevoke.toJson(enrollment)))
        assertThrows(BadMessageException::class.java) { UnlockRevoke.parse(obj("""{}""")) }
    }

    @Test
    fun resultBodies() {
        val ok = UnlockResult.ok(key)
        assertEquals(JsonPrimitive(true), ok["ok"])
        assertArrayEquals(key, B64.decode(ok["key"]!!.jsonPrimitive.content))
        assertNull(UnlockResult.ok()["key"])
        val failed = UnlockResult.failed(UnlockErrors.DENIED)
        assertEquals(JsonPrimitive(false), failed["ok"])
        assertEquals("denied", failed["error"]!!.jsonObject["code"]!!.jsonPrimitive.content)
        assertFalse(failed.containsKey("key"))
        assertTrue(UnlockController.CANCELLED.isEmpty())
    }
}
