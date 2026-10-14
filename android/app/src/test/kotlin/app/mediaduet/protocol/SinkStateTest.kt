package app.mediaduet.protocol

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SinkStateTest {
    private fun obj(text: String) = Json.parseToJsonElement(text).jsonObject

    @Test
    fun parsesTheHostsAudioSinkState() {
        val on = SinkState.parse(obj("""{"available":true,"state":"on","device":"Find X8"}"""))
        assertEquals(SinkState(true, SinkState.ON, "Find X8", null), on)
        assertTrue(on.active)
        assertTrue(SinkState.parse(obj("""{"available":true,"state":"connecting"}""")).active)

        val err = SinkState.parse(obj("""{"available":false,"state":"error","error":"no_device","future":1}"""))
        assertFalse(err.active)
        assertFalse(err.available)
        assertEquals("no_device", err.error)
    }

    @Test
    fun missingFieldsMeanOffAndUnavailable() {
        val s = SinkState.parse(obj("""{}"""))
        assertEquals(SinkState.OFF, s.state)
        assertFalse(s.available)
        assertFalse(s.active)
        assertNull(SinkState.parse(obj("""{"state":"off","device":""}""")).device)
    }
}
