package app.mediaduet.protocol

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Test

/** Optional session field `outputLatencyMs` (integer 0–2000; invalid values are ignored). */
class OutputLatencyTest {
    private fun session(extra: String): JsonObject = Json.parseToJsonElement(
        """{"key":"k","app":{"id":"a","name":"A"},"title":"t","trackId":"x","durationMs":1000,"positionMs":0,
           "rate":1.0,"status":"playing","actions":[],"artwork":null,"kind":"music"$extra}""",
    ).jsonObject

    @Test
    fun parsesValidValues() {
        assertEquals(180, MediaSessionInfo.parse(session(""","outputLatencyMs":180""")).outputLatencyMs)
        assertEquals(0, MediaSessionInfo.parse(session(""","outputLatencyMs":0""")).outputLatencyMs)
        assertEquals(2_000, MediaSessionInfo.parse(session(""","outputLatencyMs":2000""")).outputLatencyMs)
    }

    @Test
    fun absentOrInvalidIsIgnoredNotRejected() {
        for (bad in listOf("", ""","outputLatencyMs":null""", ""","outputLatencyMs":-1""", ""","outputLatencyMs":2001""",
            ""","outputLatencyMs":12.5""", ""","outputLatencyMs":"180"""", ""","outputLatencyMs":true""", ""","outputLatencyMs":{}""")) {
            val s = MediaSessionInfo.parse(session(bad)) // must not throw
            assertNull(bad, s.outputLatencyMs)
        }
    }

    @Test
    fun serialisesOnlyWhenKnown() {
        val s = MediaSessionInfo.parse(session(""))
        assertFalse("outputLatencyMs" in s.toJson())
        assertEquals(180, s.copy(outputLatencyMs = 180).toJson()["outputLatencyMs"]!!.jsonPrimitive.int)
        assertEquals(2_000, s.copy(outputLatencyMs = 5_000).toJson()["outputLatencyMs"]!!.jsonPrimitive.int)
        // Round trip through a snapshot.
        val snap = MediaSnapshot(1, "k", listOf(s.copy(outputLatencyMs = 250)))
        assertEquals(250, MediaSnapshot.parse(snap.toJson()).sessions.single().outputLatencyMs)
    }
}
