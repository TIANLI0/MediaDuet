package app.mediaduet.protocol

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Test

class VolumeMessagesTest {
    private fun obj(text: String) = Json.parseToJsonElement(text).jsonObject

    @Test
    fun volumeStateRoundTripAndValidation() {
        val s = VolumeState(0.4, true, 15)
        assertEquals(s, VolumeState.parse(s.toJson()))
        assertEquals(VolumeState(1.0, false, null), VolumeState.parse(obj("""{"level":1,"extra":"x"}""")))
        assertThrows(BadMessageException::class.java) { VolumeState.parse(obj("""{"level":1.5,"muted":false}""")) }
        assertThrows(BadMessageException::class.java) { VolumeState.parse(obj("""{"muted":false}""")) }
        assertThrows(BadMessageException::class.java) { VolumeState.parse(obj("""{"level":"0.5"}""")) }
        assertThrows(BadMessageException::class.java) { VolumeState.parse(obj("""{"level":0.5,"steps":0}""")) }
    }

    @Test
    fun volumeSetVariants() {
        assertEquals(VolumeSet(level = 0.25), VolumeSet.parse(obj("""{"level":0.25}""")))
        assertEquals(VolumeSet(muted = true), VolumeSet.parse(obj("""{"muted":true}""")))
        val d = VolumeSet.parse(obj("""{"delta":-2}"""))
        assertEquals(-2, d.delta)
        assertNull(d.level)
        assertEquals(VolumeSet(0.5, false, 1), VolumeSet.parse(VolumeSet(0.5, false, 1).toJson()))
        assertThrows(BadMessageException::class.java) { VolumeSet.parse(obj("""{}""")) }
        assertThrows(BadMessageException::class.java) { VolumeSet.parse(obj("""{"level":-0.1}""")) }
        assertThrows(BadMessageException::class.java) { VolumeSet.parse(obj("""{"delta":500}""")) }
        assertThrows(BadMessageException::class.java) { VolumeSet.parse(obj("""{"delta":1.5}""")) }
        assertEquals("spotify.exe", VolumeSet.parse(obj("""{"level":0.3,"key":"spotify.exe"}""")).key)
        assertEquals(VolumeSet(muted = true, key = "a"), VolumeSet.parse(VolumeSet(muted = true, key = "a").toJson()))
        // A key alone changes nothing.
        assertThrows(BadMessageException::class.java) { VolumeSet.parse(obj("""{"key":"spotify.exe"}""")) }
    }

    @Test
    fun sessionAppVolumeIsOptionalAndLenient() {
        fun session(volume: String?) = obj(
            """{"key":"spotify.exe","status":"playing"${if (volume != null) ",\"volume\":$volume" else ""}}""",
        )
        assertNull(MediaSessionInfo.parse(session(null)).volume)
        assertNull(MediaSessionInfo.parse(session("null")).volume)
        assertEquals(AppVolume(0.8, false), MediaSessionInfo.parse(session("""{"level":0.8,"muted":false}""")).volume)
        assertEquals(AppVolume(0.2, true), MediaSessionInfo.parse(session("""{"level":0.2,"muted":true,"future":1}""")).volume)
        // Malformed values are ignored rather than rejecting the whole snapshot.
        assertNull(MediaSessionInfo.parse(session("""{"level":3}""")).volume)
        assertNull(MediaSessionInfo.parse(session("""{"level":"loud"}""")).volume)
        assertNull(MediaSessionInfo.parse(session("42")).volume)
        // Round trip keeps the field.
        val withVolume = MediaSessionInfo.parse(session("""{"level":0.5,"muted":false}"""))
        assertEquals(withVolume, MediaSessionInfo.parse(withVolume.toJson()))
    }
}
