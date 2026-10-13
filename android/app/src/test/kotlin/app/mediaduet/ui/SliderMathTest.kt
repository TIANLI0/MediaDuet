package app.mediaduet.ui

import app.mediaduet.ui.components.SliderMath
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** The new slider: relative drags, tap-to-set, clamping, RTL and the drag slop. */
class SliderMathTest {
    @Test
    fun dragIsRelativeToTheStartValueAndClamped() {
        assertEquals(0.6f, SliderMath.dragged(0.5f, 100f, 1_000f), 1e-6f) // no jump to the finger
        assertEquals(0.4f, SliderMath.dragged(0.5f, -100f, 1_000f), 1e-6f)
        assertEquals(1f, SliderMath.dragged(0.9f, 500f, 1_000f), 0f)
        assertEquals(0f, SliderMath.dragged(0.1f, -500f, 1_000f), 0f)
        assertEquals(0.4f, SliderMath.dragged(0.5f, 100f, 1_000f, rtl = true), 1e-6f)
        assertEquals(0.5f, SliderMath.dragged(0.5f, 100f, 0f), 0f)
    }

    @Test
    fun tapSetsTheValueAtThePosition() {
        assertEquals(0.25f, SliderMath.tapped(250f, 1_000f), 1e-6f)
        assertEquals(0.75f, SliderMath.tapped(250f, 1_000f, rtl = true), 1e-6f)
        assertEquals(1f, SliderMath.tapped(2_000f, 1_000f), 0f)
    }

    @Test
    fun onlyHorizontalMovementCounts() {
        // A diagonal drag (40 px vertical, 5 px horizontal) is not a value change yet; the
        // vertical component never moves the value or (since every change is consumed) the page.
        assertFalse(SliderMath.isDrag(5f, 8f))
        assertTrue(SliderMath.isDrag(-9f, 8f))
    }
}
