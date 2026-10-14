package app.mediaduet.protocol

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/** customActions / queue in the snapshot, media.queue and the custom / playQueueItem commands (§8.2–8.4). */
class PlayerExtrasTest {
    private fun obj(text: String) = Json.parseToJsonElement(text).jsonObject

    private val base = MediaSessionInfo(
        key = "com.example#1", appId = "com.example", appName = "Example", title = "t", artist = "a", album = "",
        albumArtist = "", trackId = "", durationMs = 1000, positionMs = 0, rate = 1.0, status = PlayStatus.PLAYING,
        actions = listOf(Action.PLAY), artwork = null, kind = MediaKind.MUSIC,
    )

    @Test
    fun sessionExtrasRoundTrip() {
        val icon = ArtworkRef("a".repeat(64), "image/png", 812)
        val s = base.copy(
            customActions = listOf(CustomAction("like", "Like", icon), CustomAction("mode", "Shuffle", null)),
            queue = QueueHint(37, "5f0c2a91", 12),
        )
        val back = MediaSessionInfo.parse(s.toJson())
        assertEquals(s.customActions, back.customActions)
        assertEquals(s.queue, back.queue)
        // Custom actions and queue entries are never listed in `actions`.
        assertEquals(listOf(Action.PLAY), back.actions)
    }

    @Test
    fun malformedExtrasAreSkipped() {
        val o = obj(
            """{"key":"k","customActions":[{"id":""},{"title":"no id"},{"id":"ok","icon":{"hash":"zz"}},5],
               "queue":{"count":0}}""",
        )
        val s = MediaSessionInfo.parse(o)
        assertEquals(listOf("ok"), s.customActions.map { it.id })
        assertNull(s.customActions.single().icon)
        assertNull(s.queue)
    }

    @Test
    fun newCommandsNeedTheirArgument() {
        val c = MediaCommand("k", Action.CUSTOM, null, MediaCommand.ORIGIN_USER, null, customAction = "like")
        assertEquals("like", MediaCommand.parse(c.toJson()).customAction)
        val q = MediaCommand.parse(obj("""{"action":"playQueueItem","queueId":7}"""))
        assertEquals(7L, q.queueId)
        assertThrows(BadMessageException::class.java) { MediaCommand.parse(obj("""{"action":"custom"}""")) }
        assertThrows(BadMessageException::class.java) { MediaCommand.parse(obj("""{"action":"playQueueItem"}""")) }
    }

    @Test
    fun queueWindowKeepsTheCurrentEntry() {
        val all = (0 until 1000).map { QueueEntry(it.toLong(), "t$it", "") }
        val (offset, items) = MediaQueue.window(all, 900)
        assertEquals(MediaQueue.MAX_ITEMS, items.size)
        assertTrue(items.any { it.id == 900L })
        assertEquals(offset.toLong(), items.first().id)
        val small = MediaQueue.window(all.take(10), 3)
        assertEquals(0, small.first)
        assertEquals(10, small.second.size)

        val q = MediaQueue("k", "r", "Liked", 900, all.size, offset, items)
        val back = MediaQueue.parse(q.toJson())
        assertEquals(q, back)
    }
}
