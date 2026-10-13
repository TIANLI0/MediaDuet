package app.mediaduet.media

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class InteractionTest {
    @Test
    fun toggleIsOptimisticAndReconciles() {
        var o = OptimisticPlayback().onToggle(currentlyPlaying = true, nowMs = 1_000)
        assertFalse(o.displayPlaying(actualPlaying = true)) // icon flips immediately
        // Snapshot still says playing (command in flight): keep the optimistic value.
        o = o.reconcile(actualPlaying = true, actualPositionMs = 10_000, nowMs = 1_200)
        assertFalse(o.displayPlaying(true))
        // Device confirms: optimistic state cleared, actual state shown.
        o = o.reconcile(actualPlaying = false, actualPositionMs = 10_000, nowMs = 1_400)
        assertNull(o.pendingPlaying)
        assertFalse(o.displayPlaying(false))
    }

    @Test
    fun toggleRollsBackOnFailureAndAfterTimeout() {
        val o = OptimisticPlayback().onToggle(false, 0)
        assertTrue(o.displayPlaying(false))
        assertFalse(o.onFailed(OptimisticPlayback.Kind.PLAY_PAUSE).displayPlaying(false))
        // Never confirmed: the real state wins after CONFIRM_MS.
        assertFalse(o.reconcile(false, null, OptimisticPlayback.CONFIRM_MS + 1).displayPlaying(false))
    }

    @Test
    fun seekHoldsPositionUntilConfirmed() {
        var o = OptimisticPlayback().onSeek(60_000, nowMs = 0, playing = true, rate = 1.0)
        // The old snapshot (position 5 s) must not pull the thumb back.
        o = o.reconcile(actualPlaying = true, actualPositionMs = 5_000, nowMs = 500)
        assertEquals(60_500L, o.displayPosition(5_500, 500))
        // New snapshot near the target confirms the seek.
        o = o.reconcile(true, 60_700, 800)
        assertNull(o.seekPositionMs)
        assertEquals(60_700L, o.displayPosition(60_700, 800))
    }

    @Test
    fun seekIsReleasedAfterHoldOrFailure() {
        val o = OptimisticPlayback().onSeek(30_000, 0, playing = false, rate = 1.0)
        assertEquals(30_000L, o.displayPosition(1_000, 1_900))
        assertNull(o.reconcile(false, 1_000, OptimisticPlayback.SEEK_HOLD_MS + 1).seekPositionMs)
        assertNull(o.onFailed(OptimisticPlayback.Kind.SEEK).seekPositionMs)
        val idle = OptimisticPlayback()
        assertSame(idle, idle.reconcile(true, 1, 1))
    }

    @Test
    fun sliderThrottlesHoldsAndSendsFinalValue() {
        var s = SliderSync()
        val (s1, send1) = s.onDrag(0.1f, 0)
        assertEquals(0.1f, send1)
        val (s2, send2) = s1.onDrag(0.2f, 50) // within 100 ms: throttled
        assertNull(send2)
        val (s3, send3) = s2.onDrag(0.3f, 120)
        assertEquals(0.3f, send3)
        // Incoming remote values are ignored while dragging.
        assertEquals(0.3f, s3.displayed(remote = 0.9f, nowMs = 130))
        val (s4, final) = s3.onDrag(0.35f, 150).first.onRelease(0.35f, 160)
        assertEquals(0.35f, final) // final value always sent on release
        assertEquals(0.35f, s4.displayed(0.1f, 160 + SliderSync.HOLD_MS - 1)) // still held
        assertEquals(0.1f, s4.displayed(0.1f, 160 + SliderSync.HOLD_MS)) // remote wins afterwards
        // Releasing on the value just sent does not send twice.
        s = SliderSync().onDrag(0.5f, 0).first
        assertNull(s.onRelease(0.5f, 10).second)
    }
}
