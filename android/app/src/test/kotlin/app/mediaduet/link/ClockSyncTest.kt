package app.mediaduet.link

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** Same cases as windows/tests/test_clock_sync.cpp. */
class ClockSyncTest {
    /** The peer's clock runs 50 000 ms ahead of ours. */
    private val off = 50_000L

    /** A ping sent at [t0] that takes [up] ms to the peer and [down] ms back. */
    private fun ClockSync.ping(t0: Long, up: Long, down: Long) = addSample(t0, t0 + up + off, t0 + up + down)

    @Test
    fun noSamplesFallsBack() {
        val c = ClockSync()
        assertNull(c.offsetMs(0))
        assertNull(c.uncertaintyMs(0))
        assertEquals(960L, c.anchor(12_345, 1_000, 40))
        assertEquals(960L, c.anchor(null, 1_000, 40))
    }

    @Test
    fun symmetricRoundTripIsExact() {
        val c = ClockSync()
        c.ping(1_000, 20, 20)
        assertEquals(off, c.offsetMs(1_040))
        assertEquals(20L, c.uncertaintyMs(1_040))
        assertEquals(2_000L, c.anchor(2_000 + off, 2_600, 0))
    }

    @Test
    fun lowestRttWinsOverSlowAsymmetricSamples() {
        val c = ClockSync()
        c.ping(0, 300, 10)
        c.ping(1_000, 10, 250)
        c.ping(2_000, 4, 4)
        c.ping(3_000, 120, 120)
        assertEquals(off, c.offsetMs(4_000))
        assertTrue(c.uncertaintyMs(4_000)!! <= 5)
    }

    @Test
    fun oldSamplesAgeOutThenExpire() {
        val c = ClockSync()
        c.ping(0, 2, 2)
        c.ping(400_000, 30, 30)
        assertEquals(30L, c.uncertaintyMs(400_060))
        val d = ClockSync()
        d.ping(0, 5, 5)
        assertEquals(off, d.offsetMs(ClockSync.MAX_AGE_MS))
        assertNull(d.offsetMs(ClockSync.MAX_AGE_MS + 20))
    }

    @Test
    fun restartedClockDiscardsOldSamples() {
        val c = ClockSync()
        c.ping(0, 5, 5)
        c.addSample(10_000, 10_005 - 3_000, 10_010)
        assertEquals(-3_000L, c.offsetMs(10_010))
    }

    @Test
    fun implausibleAnchorsFallBack() {
        val c = ClockSync()
        c.ping(0, 5, 5)
        val now = 1_000 + ClockSync.MAX_TRANSIT_MS + 1
        assertEquals(now - 30, c.anchor(1_000 + off, now, 30))
        assertEquals(4_990L, c.anchor(5_000 + off, 4_990, 30))
    }

    @Test
    fun badSamplesIgnoredAndWindowBounded() {
        val c = ClockSync()
        c.addSample(100, 5, 50)
        assertNull(c.offsetMs(100))
        for (i in 0 until 40) c.ping(i * 1_000L, 10L + i, 10L + i)
        assertTrue(c.uncertaintyMs(40_000)!! >= 10 + 40 - ClockSync.MAX_SAMPLES)
    }
}
