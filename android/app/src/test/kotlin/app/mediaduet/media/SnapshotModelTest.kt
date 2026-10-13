package app.mediaduet.media

import app.mediaduet.protocol.Action
import app.mediaduet.protocol.MediaKind
import app.mediaduet.protocol.PlayStatus
import app.mediaduet.protocol.TrackId
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SnapshotModelTest {
    private fun state(
        key: String = "com.netease.cloudmusic#1",
        state: Int = PlaybackConst.STATE_PLAYING,
        position: Long = 10_000,
        updated: Long = 1_000,
        speed: Float = 1f,
        actions: Long = PlaybackConst.ACTION_PLAY or PlaybackConst.ACTION_PAUSE or PlaybackConst.ACTION_SKIP_TO_NEXT or PlaybackConst.ACTION_SEEK_TO,
        duration: Long? = 245_000,
        changed: Long = 0,
        hasState: Boolean = true,
        title: String = "晴天",
    ) = LocalSessionState(
        key, key.substringBefore('#'), "网易云音乐", title, "周杰伦", "叶惠美", "", duration, hasState, state, position,
        updated, speed, actions, 0, null, changed,
    )

    @Test
    fun mapsStatusActionsAndPosition() {
        val s = state()
        assertEquals(PlayStatus.PLAYING, s.status)
        assertEquals(listOf(Action.PLAY, Action.PAUSE, Action.TOGGLE, Action.NEXT, Action.SEEK), s.protocolActions())
        assertEquals(12_000L, s.estimatedPosition(3_000))
        val info = s.toSessionInfo(3_000)
        assertEquals(12_000L, info.positionMs)
        assertEquals(TrackId.of("晴天", "周杰伦", "叶惠美", 245_000), info.trackId)
        assertEquals(MediaKind.MUSIC, info.kind)
        // Paused: no extrapolation; position clamps to duration.
        assertEquals(10_000L, state(state = PlaybackConst.STATE_PAUSED).estimatedPosition(9_999_999))
        assertEquals(245_000L, state(position = 244_000).estimatedPosition(10_000))
        // Players without a PlaybackState or with unknown position.
        assertNull(state(hasState = false).estimatedPosition(5_000))
        assertEquals(PlayStatus.UNKNOWN, state(hasState = false).status)
        assertNull(state(position = -1).estimatedPosition(5_000))
        // No seek without a duration.
        assertFalse(Action.SEEK in state(duration = null).protocolActions())
        assertEquals(PlayStatus.BUFFERING, state(state = PlaybackConst.STATE_SKIPPING_TO_NEXT).status)
        assertEquals(PlayStatus.STOPPED, state(state = PlaybackConst.STATE_ERROR).status)
    }

    @Test
    fun picksPlayingThenMostRecent() {
        val a = state(key = "a#1", state = PlaybackConst.STATE_PAUSED, changed = 50)
        val b = state(key = "b#1", state = PlaybackConst.STATE_PLAYING, changed = 10)
        val c = state(key = "c#1", state = PlaybackConst.STATE_PAUSED, changed = 90)
        assertEquals("b#1", SnapshotBuilder.pickActive(listOf(a, b, c))?.key)
        assertEquals("c#1", SnapshotBuilder.pickActive(listOf(a, c))?.key)
        // Tie: first in priority order wins.
        val d = state(key = "d#1", state = PlaybackConst.STATE_PAUSED, changed = 90)
        assertEquals("d#1", SnapshotBuilder.pickActive(listOf(d, c))?.key)
        assertNull(SnapshotBuilder.pickActive(emptyList()))
    }

    @Test
    fun buildCapsSessions() {
        val many = (1..20).map { state(key = "p$it#1") }
        val snap = SnapshotBuilder.build(1, many, 1_000)
        assertEquals(16, snap.sessions.size)
    }

    @Test
    fun suppressesNaturallyAdvancingPosition() {
        val sup = SnapshotSuppressor()
        val s0 = SnapshotBuilder.build(1, listOf(state(position = 10_000, updated = 1)), 1)
        assertTrue(sup.shouldSend(s0, 1))
        // 5 s later, same state; position advanced naturally -> suppressed (gen differs, ignored).
        val s1 = SnapshotBuilder.build(2, listOf(state(position = 10_000, updated = 1)), 5_001)
        assertEquals(15_000L, s1.sessions[0].positionMs)
        assertFalse(sup.shouldSend(s1, 5_001))
        // A seek -> sent.
        val s2 = SnapshotBuilder.build(3, listOf(state(position = 100_000, updated = 6_000)), 6_000)
        assertTrue(sup.shouldSend(s2, 6_000))
        // Status change -> sent.
        val s3 = SnapshotBuilder.build(4, listOf(state(state = PlaybackConst.STATE_PAUSED, position = 100_500, updated = 6_500)), 6_500)
        assertTrue(sup.shouldSend(s3, 6_500))
        // Paused, nothing changes -> suppressed even after a long time.
        val s4 = SnapshotBuilder.build(5, listOf(state(state = PlaybackConst.STATE_PAUSED, position = 100_500, updated = 6_500)), 60_000)
        assertFalse(sup.shouldSend(s4, 60_000))
        // Track change -> sent.
        val s5 = SnapshotBuilder.build(6, listOf(state(state = PlaybackConst.STATE_PAUSED, position = 100_500, updated = 6_500, title = "七里香")), 61_000)
        assertTrue(sup.shouldSend(s5, 61_000))
    }
}
