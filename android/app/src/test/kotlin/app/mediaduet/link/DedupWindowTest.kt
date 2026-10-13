package app.mediaduet.link

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DedupWindowTest {
    @Test
    fun acceptsNewRejectsDuplicates() {
        val w = DedupWindow(1024)
        assertTrue(w.accept(1))
        assertTrue(w.accept(2))
        assertFalse(w.accept(2))
        assertFalse(w.accept(1))
    }

    @Test
    fun outOfOrderWithinWindowIsAcceptedOnce() {
        val w = DedupWindow(1024)
        assertTrue(w.accept(10))
        assertTrue(w.accept(7)) // late delivery over a slower transport
        assertFalse(w.accept(7))
        assertTrue(w.accept(8))
        assertTrue(w.accept(9))
        assertFalse(w.accept(10))
    }

    @Test
    fun idsOlderThanWindowAreRejected() {
        val w = DedupWindow(1024)
        assertTrue(w.accept(1))
        assertTrue(w.accept(2000))
        assertFalse(w.accept(976)) // 2000 - 976 = 1024 -> outside
        assertTrue(w.accept(977))
        assertFalse(w.accept(977))
    }

    @Test
    fun windowSlidesAndClearsOldSlots() {
        val w = DedupWindow(4)
        for (i in 1L..4L) assertTrue(w.accept(i))
        assertTrue(w.accept(6)) // slot of 2 reused for 6, 5 cleared
        assertTrue(w.accept(5))
        assertFalse(w.accept(2))
        assertTrue(w.accept(100))
        assertFalse(w.accept(6))
        assertTrue(w.accept(99))
    }
}
