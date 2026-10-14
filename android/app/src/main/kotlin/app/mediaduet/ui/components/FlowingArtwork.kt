package app.mediaduet.ui.components

import android.graphics.Bitmap
import android.os.Build
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.wrapContentSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.BlurEffect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.ColorMatrix
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.TileMode
import androidx.compose.ui.graphics.TransformOrigin
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.core.graphics.scale
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.currentStateAsState
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.sin

/**
 * Apple-Music-style living background: four copies of the artwork, each turning slowly around its
 * own drifting centre at its own pace, saturated and blurred into soft colour fields that flow
 * while music plays (and hold still while paused, in Settings or with reduced motion).
 *
 * Cheap by construction: the scene is drawn into a canvas at 1/[DOWNSCALE] of the screen size,
 * blurred there and scaled up by its layer (bilinear filtering softens it further), and the clock
 * advances at ~30 fps — the motion is slow, so more frames would only cost the glass controls that
 * refract this layer extra work. Below Android 12 (no RenderEffect) the tiny canvas and a 16-px
 * copy of the artwork alone give the blur.
 */
@Composable
fun FlowingArtwork(artwork: Bitmap, animate: Boolean, modifier: Modifier = Modifier) {
    val blurSupported = Build.VERSION.SDK_INT >= 31
    val source = remember(artwork) {
        val side = if (blurSupported) 48 else 16
        artwork.scale(side, side).asImageBitmap()
    }
    // Average colour: the ground under the turning copies (never a black gap at the edges).
    val ground = remember(artwork) { Color(artwork.scale(1, 1).getPixel(0, 0)) }
    val saturate = remember { ColorFilter.colorMatrix(ColorMatrix().apply { setToSaturation(1.45f) }) }

    var seconds by remember { mutableFloatStateOf(START_SECONDS) }
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    val visible by lifecycle.currentStateAsState()
    val running = animate && visible.isAtLeast(Lifecycle.State.STARTED)
    LaunchedEffect(running) {
        if (!running) return@LaunchedEffect
        var last = withFrameNanos { it }
        var pending = 0f
        while (true) {
            withFrameNanos { now ->
                // Capped step: coming back from a stall never jumps the scene.
                pending += ((now - last) / 1e9f).coerceAtMost(0.1f)
                last = now
                if (pending >= FRAME_SECONDS) {
                    seconds += pending
                    pending = 0f
                }
            }
        }
    }

    BoxWithConstraints(modifier, contentAlignment = Alignment.TopStart) {
        val w = maxWidth / DOWNSCALE
        val h = maxHeight / DOWNSCALE
        Canvas(
            Modifier
                .wrapContentSize(Alignment.TopStart)
                .size(w, h)
                .graphicsLayer {
                    transformOrigin = TransformOrigin(0f, 0f)
                    scaleX = DOWNSCALE
                    scaleY = DOWNSCALE
                    if (blurSupported) {
                        val r = size.minDimension * 0.16f
                        renderEffect = BlurEffect(r, r, TileMode.Clamp)
                    }
                },
        ) {
            drawRect(ground)
            val t = seconds
            val extent = max(size.width, size.height)
            for (b in BLOBS) {
                val orbit = 2f * PI.toFloat() * t / b.orbitSeconds + b.phase
                val center = Offset(
                    size.width * b.x + extent * b.drift * cos(orbit),
                    size.height * b.y + extent * b.drift * sin(orbit),
                )
                val side = (extent * b.scale).toInt().coerceAtLeast(1)
                rotate(degrees = b.phase * 57.3f + 360f * t / b.turnSeconds, pivot = center) {
                    drawImage(
                        source,
                        dstOffset = IntOffset((center.x - side / 2f).toInt(), (center.y - side / 2f).toInt()),
                        dstSize = IntSize(side, side),
                        alpha = b.alpha,
                        colorFilter = saturate,
                        filterQuality = FilterQuality.Low,
                    )
                }
            }
        }
    }
}

/** One turning copy: position (fraction of the screen), size (of the longer side), pace, phase. */
private class Blob(
    val x: Float,
    val y: Float,
    val scale: Float,
    val turnSeconds: Float,
    val orbitSeconds: Float,
    val drift: Float,
    val phase: Float,
    val alpha: Float,
)

private val BLOBS = listOf(
    Blob(x = 0.22f, y = 0.18f, scale = 1.25f, turnSeconds = 70f, orbitSeconds = 41f, drift = 0.08f, phase = 0.0f, alpha = 1f),
    Blob(x = 0.82f, y = 0.32f, scale = 1.05f, turnSeconds = -55f, orbitSeconds = 37f, drift = 0.10f, phase = 1.7f, alpha = 0.9f),
    Blob(x = 0.25f, y = 0.78f, scale = 1.15f, turnSeconds = 85f, orbitSeconds = 47f, drift = 0.09f, phase = 3.1f, alpha = 0.9f),
    Blob(x = 0.78f, y = 0.84f, scale = 0.95f, turnSeconds = -64f, orbitSeconds = 33f, drift = 0.07f, phase = 4.6f, alpha = 0.85f),
)

private const val DOWNSCALE = 8f
private const val FRAME_SECONDS = 1f / 30f
/** Not 0: the first (and the paused) frame is already an interesting arrangement. */
private const val START_SECONDS = 12f
