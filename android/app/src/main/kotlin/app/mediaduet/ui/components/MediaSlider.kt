package app.mediaduet.ui.components

import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.PointerInputChange
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.disabled
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.setProgress
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import app.mediaduet.ui.theme.LocalGlass
import kotlin.math.abs

/**
 * Pure slider gesture maths (unit-tested): a drag moves the value relative to where it started
 * (no jump to the finger); a tap without movement sets the value at the tap position.
 */
object SliderMath {
    fun dragged(startValue: Float, dxPx: Float, widthPx: Float, rtl: Boolean = false): Float {
        if (widthPx <= 0f) return startValue
        val d = (if (rtl) -dxPx else dxPx) / widthPx
        return (startValue + d).coerceIn(0f, 1f)
    }

    fun tapped(xPx: Float, widthPx: Float, rtl: Boolean = false): Float {
        if (widthPx <= 0f) return 0f
        val f = (xPx / widthPx).coerceIn(0f, 1f)
        return if (rtl) 1f - f else f
    }

    /** Once [slopPx] is passed horizontally the slider owns the gesture. */
    fun isDrag(dxPx: Float, slopPx: Float): Boolean = abs(dxPx) > slopPx
}

/**
 * Apple-Music-style slider (progress, volume, settings): a thin rounded track (5 dp) in the
 * foreground colour - played part at ~85 % (dark) / ~80 % (light), the rest at ~22 % - that grows
 * with a spring to 11 dp and brightens to full opacity while touched. No white thumb.
 *
 * The slider owns every pointer that starts on it: all changes are consumed from the first down
 * until the pointer is lifted, so no parent (page scroll, list, swipe gestures) moves while you
 * drag it, whatever the vertical component of the drag. [value] is read in the draw phase only.
 */
@Composable
fun MediaSlider(
    value: () -> Float,
    onValueChange: (Float) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    contentDescription: String? = null,
    color: Color = MaterialTheme.colorScheme.onSurface,
    onActiveChange: (Boolean) -> Unit = {},
    onValueChangeFinished: (Float) -> Unit = {},
) {
    val glass = LocalGlass.current
    var active by remember { mutableStateOf(false) }
    val thickness by animateDpAsState(if (active) 11.dp else 5.dp, spring(dampingRatio = 0.7f, stiffness = Spring.StiffnessMedium), label = "sliderThickness")
    val playedAlpha by animateFloatAsState(if (active) 1f else if (glass.isDark) 0.85f else 0.8f, spring(stiffness = Spring.StiffnessMedium), label = "sliderAlpha")
    val rtl = LocalLayoutDirection.current == LayoutDirection.Rtl
    val currentValue by rememberUpdatedState(value)
    val change by rememberUpdatedState(onValueChange)
    val finished by rememberUpdatedState(onValueChangeFinished)
    val activeChange by rememberUpdatedState(onActiveChange)
    Box(
        modifier
            .fillMaxWidth()
            .height(40.dp)
            .alpha(if (enabled) 1f else 0.4f)
            .semantics {
                contentDescription?.let { this.contentDescription = it }
                progressBarRangeInfo = ProgressBarRangeInfo(currentValue().coerceIn(0f, 1f), 0f..1f)
                if (!enabled) {
                    disabled()
                } else {
                    setProgress { v ->
                        val f = v.coerceIn(0f, 1f)
                        change(f)
                        finished(f)
                        true
                    }
                }
            }
            .then(
                if (!enabled) Modifier else Modifier.pointerInput(rtl) {
                    awaitEachGesture {
                        val down = awaitFirstDown(requireUnconsumed = false)
                        down.consume()
                        active = true
                        activeChange(true)
                        val start = currentValue().coerceIn(0f, 1f)
                        var v = start
                        var dragging = false
                        try {
                            while (true) {
                                val event = awaitPointerEvent()
                                val c: PointerInputChange = event.changes.firstOrNull { it.id == down.id } ?: break
                                val dx = c.position.x - down.position.x
                                if (!c.pressed) {
                                    c.consume()
                                    if (!dragging) {
                                        v = SliderMath.tapped(down.position.x, size.width.toFloat(), rtl)
                                        change(v)
                                    }
                                    finished(v)
                                    break
                                }
                                if (dragging || SliderMath.isDrag(dx, viewConfiguration.touchSlop / 2)) {
                                    dragging = true
                                    v = SliderMath.dragged(start, dx, size.width.toFloat(), rtl)
                                    change(v)
                                }
                                // Own the whole gesture: no parent sees any movement.
                                c.consume()
                            }
                        } finally {
                            active = false
                            activeChange(false)
                        }
                    }
                },
            ),
    ) {
        Canvas(Modifier.fillMaxSize()) {
            val t = thickness.toPx()
            val top = (size.height - t) / 2f
            val r = CornerRadius(t / 2f, t / 2f)
            drawRoundRect(color.copy(alpha = 0.22f), Offset(0f, top), Size(size.width, t), r)
            val f = currentValue().coerceIn(0f, 1f)
            if (f > 0f) {
                val w = (size.width * f).coerceAtLeast(t)
                val x = if (rtl) size.width - w else 0f
                drawRoundRect(color.copy(alpha = playedAlpha), Offset(x, top), Size(w, t), r)
            }
        }
    }
}
