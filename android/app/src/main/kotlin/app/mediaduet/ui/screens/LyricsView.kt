package app.mediaduet.ui.screens

import android.os.Build
import android.os.SystemClock
import android.provider.Settings
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.snap
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.animateScrollBy
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.BlurEffect
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.CompositingStrategy
import androidx.compose.ui.graphics.TileMode
import androidx.compose.ui.graphics.TransformOrigin
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.layout.boundsInWindow
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.repeatOnLifecycle
import app.mediaduet.R
import app.mediaduet.remote.Caret
import app.mediaduet.remote.GapRow
import app.mediaduet.remote.LineRow
import app.mediaduet.remote.LyricRow
import app.mediaduet.remote.LyricRows
import app.mediaduet.remote.LyricTiming
import app.mediaduet.remote.Lyrics
import app.mediaduet.remote.LyricsDelay
import app.mediaduet.remote.SmoothedClock
import app.mediaduet.ui.components.GlassButton
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collectLatest
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.min
import kotlin.math.sin

/** True when the user disabled animations (animator duration scale 0). */
@Composable
fun rememberReducedMotion(): Boolean {
    val context = LocalContext.current
    return remember {
        Settings.Global.getFloat(context.contentResolver, Settings.Global.ANIMATOR_DURATION_SCALE, 1f) == 0f
    }
}

/**
 * Playback position interpolated every frame from the last snapshot (monotonic clock × rate).
 * The frame loop runs only while [playing], [visible] and the lifecycle is at least STARTED;
 * otherwise the value is refreshed once whenever [key] (the snapshot) changes.
 */
@Composable
fun rememberFramePosition(key: Any?, playing: Boolean, visible: Boolean, compute: (nowMs: Long) -> Long): State<Long> {
    val position = remember { mutableLongStateOf(compute(SystemClock.elapsedRealtime())) }
    val latest by rememberUpdatedState(compute)
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    LaunchedEffect(key, playing, visible, lifecycle) {
        position.longValue = latest(SystemClock.elapsedRealtime())
        if (!playing || !visible) return@LaunchedEffect
        lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
            while (true) {
                withFrameNanos { }
                position.longValue = latest(SystemClock.elapsedRealtime())
            }
        }
    }
    return position
}

/**
 * The lyrics clock (see `docs/ipc.md` "Lyrics timing"): the player's own extrapolated position
 * ([truth], re-anchored by every report) smoothed by [SmoothedClock] on the frame clock, minus
 * [delayMs] (output latency + per-app offset). Reports are re-read whenever [key] changes; the
 * frame loop runs only while [playing], [active] and the screen is started.
 */
@Composable
fun rememberLyricsClock(
    key: Any?,
    epoch: Any?,
    playing: Boolean,
    rate: Double,
    delayMs: Int,
    active: Boolean,
    truth: (nowMs: Long) -> Long,
): State<Long> {
    val clock = remember { SmoothedClock() }
    val position = remember { mutableLongStateOf(LyricsDelay.displayPosition(truth(SystemClock.elapsedRealtime()), delayMs)) }
    val latest by rememberUpdatedState(truth)
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    LaunchedEffect(key, epoch, playing, rate, delayMs, active, lifecycle) {
        fun sample(nowMs: Double) {
            val shown = clock.update(latest(nowMs.toLong()), nowMs, playing, rate, epoch)
            position.longValue = LyricsDelay.displayPosition(shown, delayMs)
        }
        sample(SystemClock.elapsedRealtimeNanos() / 1e6)
        if (!playing || !active) return@LaunchedEffect
        lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
            // Frame times are on System.nanoTime(); map them onto elapsedRealtime once.
            val base = SystemClock.elapsedRealtimeNanos() - System.nanoTime()
            while (true) {
                val frame = withFrameNanos { it }
                sample((frame + base) / 1e6)
            }
        }
    }
    return position
}

private val LyricStyle = TextStyle(fontSize = 27.sp, lineHeight = 35.sp, fontWeight = FontWeight.Bold, letterSpacing = (-0.2).sp)
private val TranslationStyle = TextStyle(fontSize = 17.sp, lineHeight = 23.sp, fontWeight = FontWeight.Medium)

private fun TextStyle.scaled(k: Float) = if (k == 1f) this else copy(fontSize = fontSize * k, lineHeight = lineHeight * k)

/** Opacity of the not-yet-sung part of the current line (inside a fully opaque line). */
private const val UNSUNG_ALPHA = 0.42f

/** Where the current line sits, as a fraction of the lyrics area height. */
private const val ANCHOR = 0.32f

/** After a manual scroll, auto-follow resumes this long after the finger is lifted. */
private const val RESUME_FOLLOW_MS = 4_000L

/**
 * Apple-Music-style synced lyrics (AMLL-like): the current line at ~32 % of the area with a
 * continuous feathered fill (sung part bright, the rest clearly dimmer but brighter than other
 * lines), a slight spring scale-up, other lines dimmed (and lightly blurred on API 31+) by
 * distance, spring auto-scroll, breathing "•••" for instrumental gaps ≥ 4 s, translations under
 * each line. A manual scroll pauses auto-follow (no blur while browsing) and shows a "back to the
 * current line" pill; tapping a line seeks ([onSeek] null = not seekable).
 *
 * [position] is the delayed, smoothed lyrics clock; it is read in the draw phase only, so the
 * per-frame update is a redraw of the current line, not a recomposition.
 */
@Composable
fun SyncedLyrics(
    lyrics: Lyrics,
    position: State<Long>,
    onSeek: ((Long) -> Unit)?,
    backdrop: com.kyant.backdrop.Backdrop,
    modifier: Modifier = Modifier,
    showTranslations: Boolean = true,
    /** Height of the bottom fade (lines dissolve under floating controls). */
    bottomFade: androidx.compose.ui.unit.Dp = 96.dp,
    /** Space at the bottom covered by floating controls (the "back to current" pill sits above it). */
    bottomObscured: androidx.compose.ui.unit.Dp = 0.dp,
    /** Text size (LyricsSize.scale). */
    textScale: Float = 1f,
) {
    val reduced = rememberReducedMotion()
    val rows = remember(lyrics) { LyricRows.build(lyrics) }
    val endMs = remember(rows) { LyricRows.endMs(rows) }
    // -1 = before the first row, -2 = the lyrics are over (no current line).
    val active by remember(rows) {
        derivedStateOf {
            val p = position.value
            if (p >= endMs) -2 else LyricRows.activeIndex(rows, p)
        }
    }
    val listState = rememberLazyListState()
    var browsing by remember { mutableStateOf(false) }
    var programmatic by remember { mutableStateOf(false) }
    var flashIndex by remember { mutableIntStateOf(-1) }
    val haptics = LocalHapticFeedback.current

    // A manual scroll pauses auto-follow until RESUME_FOLLOW_MS after the scroll ends.
    LaunchedEffect(listState) {
        snapshotFlow { listState.isScrollInProgress && !programmatic }.collectLatest { userScrolling ->
            if (userScrolling) {
                browsing = true
            } else if (browsing) {
                delay(RESUME_FOLLOW_MS)
                browsing = false
            }
        }
    }
    LaunchedEffect(flashIndex) {
        if (flashIndex >= 0) {
            delay(700)
            flashIndex = -1
        }
    }

    BoxWithConstraints(modifier.fillMaxWidth()) {
        val anchor = maxHeight * ANCHOR
        val target = when (active) {
            -1 -> 0
            -2 -> rows.lastIndex
            else -> active
        }.coerceIn(0, (rows.size - 1).coerceAtLeast(0))
        LaunchedEffect(target, browsing, rows) {
            if (browsing || rows.isEmpty()) return@LaunchedEffect
            programmatic = true
            try {
                val info = listState.layoutInfo.visibleItemsInfo.firstOrNull { it.index == target }
                if (info != null && !reduced) {
                    // Item offset 0 == its top at the anchor (the top content padding).
                    listState.animateScrollBy(info.offset.toFloat(), spring(dampingRatio = 0.86f, stiffness = 110f))
                } else {
                    listState.scrollToItem(target)
                }
            } finally {
                programmatic = false
            }
        }
        LazyColumn(
            Modifier
                .fillMaxSize()
                // Alpha mask (not decoration): lines dissolve at the top (under the header) and the
                // bottom (under the floating controls) instead of being cut off.
                .graphicsLayer(compositingStrategy = CompositingStrategy.Offscreen)
                .drawWithContent {
                    drawContent()
                    val top = (size.height * 0.16f).coerceIn(56.dp.toPx(), 120.dp.toPx())
                    val bottom = bottomFade.toPx().coerceAtMost(size.height * 0.6f)
                    drawRect(
                        Brush.verticalGradient(0f to Color.Transparent, 0.35f to Color.Black.copy(alpha = 0.35f), 1f to Color.Black, startY = 0f, endY = top),
                        size = Size(size.width, top),
                        blendMode = BlendMode.DstIn,
                    )
                    drawRect(
                        Brush.verticalGradient(0f to Color.Black, 1f to Color.Transparent, startY = size.height - bottom, endY = size.height),
                        topLeft = Offset(0f, size.height - bottom),
                        size = Size(size.width, bottom),
                        blendMode = BlendMode.DstIn,
                    )
                },
            state = listState,
            contentPadding = PaddingValues(top = anchor, bottom = maxHeight - anchor),
        ) {
            items(rows.size, key = { it }) { i ->
                val distance = when (active) {
                    -1 -> i + 1
                    -2 -> rows.size - i
                    else -> abs(i - active)
                }
                when (val row = rows[i]) {
                    is LineRow -> LyricLineItem(
                        row = row,
                        isActive = i == active,
                        distance = distance,
                        browsing = browsing,
                        reduced = reduced,
                        flash = i == flashIndex,
                        position = position,
                        showTranslation = showTranslations,
                        textScale = textScale,
                        onClick = onSeek?.let { seek ->
                            {
                                haptics.performHapticFeedback(HapticFeedbackType.Confirm)
                                flashIndex = i
                                seek(row.startMs)
                                browsing = false
                            }
                        },
                    )
                    is GapRow -> GapItem(row, isActive = i == active, reduced = reduced, position = position)
                }
            }
        }
        AnimatedVisibility(
            visible = browsing && active != -2,
            enter = fadeIn() + scaleIn(initialScale = 0.9f),
            exit = fadeOut() + scaleOut(targetScale = 0.9f),
            modifier = Modifier.align(Alignment.BottomCenter).padding(bottom = 16.dp + bottomObscured),
        ) {
            GlassButton({ browsing = false }, backdrop, height = 44.dp) {
                Text(stringResource(R.string.lyrics_back_to_current), style = MaterialTheme.typography.labelLarge)
            }
        }
    }
}

@Composable
private fun LyricLineItem(
    row: LineRow,
    isActive: Boolean,
    distance: Int,
    browsing: Boolean,
    reduced: Boolean,
    flash: Boolean,
    position: State<Long>,
    showTranslation: Boolean,
    textScale: Float,
    onClick: (() -> Unit)?,
) {
    val color = MaterialTheme.colorScheme.onSurface
    val lyricStyle = remember(textScale) { LyricStyle.scaled(textScale) }
    val translationStyle = remember(textScale) { TranslationStyle.scaled(textScale) }
    val targetAlpha = when {
        isActive -> 1f
        browsing -> 0.5f
        else -> (0.34f - 0.06f * (distance - 1)).coerceAtLeast(0.14f)
    }
    val alpha by animateFloatAsState(targetAlpha, if (reduced) snap() else tween(320), label = "lyricAlpha")
    val scale by animateFloatAsState(
        if (isActive) 1f else 0.95f,
        if (reduced) snap() else spring(dampingRatio = 0.8f, stiffness = Spring.StiffnessMediumLow),
        label = "lyricScale",
    )
    // Depth of field (AMLL / Apple Music): the blur grows with the distance from the current line
    // (API 31+; off while browsing so every line can be read).
    val blurTarget = if (Build.VERSION.SDK_INT < 31 || reduced || browsing || isActive) 0f else min(distance, 4) * 1.3f
    val blurDp by animateFloatAsState(blurTarget, tween(360), label = "lyricBlur")
    var layout by remember(row.line.text) { mutableStateOf<TextLayoutResult?>(null) }
    val feather = with(LocalDensity.current) { lyricStyle.fontSize.toPx() * 0.5f }
    Column(
        Modifier
            .fillMaxWidth()
            .background(if (flash) color.copy(alpha = 0.08f) else Color.Transparent, RoundedCornerShape(14.dp))
            .then(
                if (onClick != null) Modifier.clickable(remember { MutableInteractionSource() }, indication = null, onClick = onClick) else Modifier,
            )
            .padding(vertical = 13.dp)
            .graphicsLayer {
                this.alpha = alpha
                scaleX = scale
                scaleY = scale
                transformOrigin = TransformOrigin(0f, 0.5f)
                val r = blurDp.dp.toPx()
                renderEffect = if (r > 0.5f) BlurEffect(r, r, TileMode.Decal) else null
            },
    ) {
        Text(
            row.line.text,
            style = lyricStyle,
            color = color,
            onTextLayout = { layout = it },
            modifier = if (!isActive) Modifier else Modifier
                .graphicsLayer(compositingStrategy = CompositingStrategy.Offscreen)
                .drawWithContent {
                    drawContent()
                    val l = layout ?: return@drawWithContent
                    val unsung = Color.Black.copy(alpha = UNSUNG_ALPHA)
                    val c = LyricTiming.caret(row.line, position.value)
                    if (c == null) {
                        drawRect(unsung, blendMode = BlendMode.DstIn)
                        return@drawWithContent
                    }
                    val (edgeLine, x) = fillEdge(l, row.line.text, c)
                    for (vl in edgeLine until l.lineCount) {
                        val top = l.getLineTop(vl)
                        val height = l.getLineBottom(vl) - top
                        if (vl > edgeLine) {
                            drawRect(unsung, Offset(0f, top), Size(size.width, height), blendMode = BlendMode.DstIn)
                        } else {
                            // Feathered edge (~0.5 × font size): sung part opaque, the rest dimmed.
                            drawRect(
                                Brush.horizontalGradient(0f to Color.Black, 1f to unsung, startX = x - feather / 2, endX = x + feather / 2),
                                Offset(0f, top),
                                Size(size.width, height),
                                blendMode = BlendMode.DstIn,
                            )
                        }
                    }
                },
        )
        val tr = row.translation
        if (tr != null && showTranslation) {
            Spacer(Modifier.height(4.dp))
            Text(tr, style = translationStyle, color = color.copy(alpha = if (isActive) 0.72f else 0.9f))
        }
    }
}

/**
 * (visual line, x) of the fill edge. A span on one visual line interpolates linearly in x between
 * its carets (like Windows); a span that wraps moves code point by code point across the lines.
 */
private fun fillEdge(l: TextLayoutResult, text: String, c: Caret): Pair<Int, Float> {
    val len = text.length
    if (c.from >= len) {
        val last = l.lineCount - 1
        return last to l.getLineRight(last)
    }
    fun xAt(offset: Int, line: Int): Float =
        if (offset >= len || l.getLineForOffset(offset) != line) l.getLineRight(line) else l.getHorizontalPosition(offset, true)
    val fromLine = l.getLineForOffset(c.from)
    val lastCharLine = l.getLineForOffset((c.to - 1).coerceIn(c.from, len - 1))
    if (c.to <= c.from || fromLine == lastCharLine) {
        val x0 = l.getHorizontalPosition(c.from, true)
        val x1 = if (c.to <= c.from) x0 else xAt(c.to, fromLine)
        return fromLine to (x0 + (x1 - x0) * c.fraction)
    }
    val step = LyricTiming.codePointStep(text, c)
    if (step.from >= len) {
        val last = l.lineCount - 1
        return last to l.getLineRight(last)
    }
    val line = l.getLineForOffset(step.from)
    val x0 = l.getHorizontalPosition(step.from, true)
    return line to (x0 + (xAt(step.to, line) - x0) * step.fraction)
}

/** Instrumental gap: three dots light up one after another, breathe, and shrink away at the end. */
@Composable
private fun GapItem(row: GapRow, isActive: Boolean, reduced: Boolean, position: State<Long>) {
    val color = MaterialTheme.colorScheme.onSurface
    val shown by animateFloatAsState(if (isActive) 1f else 0f, if (reduced) snap() else tween(400), label = "gap")
    Box(
        Modifier
            .fillMaxWidth()
            .height(52.dp)
            .graphicsLayer { alpha = shown }
            .drawBehind {
                if (shown <= 0f) return@drawBehind
                val p = position.value
                val span = (row.endMs - row.startMs).coerceAtLeast(1)
                val progress = ((p - row.startMs).toFloat() / span).coerceIn(0f, 1f)
                val breathe = if (reduced) 1f else 1f + 0.12f * sin(2 * PI * (p - row.startMs) / 2_400.0).toFloat()
                val exit = ((row.endMs - p) / 450f).coerceIn(0f, 1f)
                val r = 5.dp.toPx() * breathe * (0.4f + 0.6f * exit)
                val gap = 18.dp.toPx()
                for (d in 0 until 3) {
                    val on = (progress * 3 - d).coerceIn(0f, 1f)
                    drawCircle(color.copy(alpha = (0.25f + 0.75f * on) * exit.coerceAtLeast(0.2f)), r, Offset(r + 4.dp.toPx() + d * gap, size.height / 2))
                }
            },
    )
}

/** Static list for plain (unsynced) lyrics. */
@Composable
fun PlainLyrics(lines: List<String>, modifier: Modifier = Modifier) {
    Column(modifier.fillMaxWidth()) {
        for (l in lines) {
            Text(l, style = LyricStyle.copy(fontSize = 22.sp, lineHeight = 30.sp, fontWeight = FontWeight.SemiBold), modifier = Modifier.padding(vertical = 8.dp))
        }
    }
}

/** Compact current line that cross-fades / slides on change. */
@Composable
fun CurrentLyricLine(text: String?, modifier: Modifier = Modifier, style: TextStyle? = null) {
    val reduced = rememberReducedMotion()
    AnimatedContent(
        targetState = text,
        transitionSpec = {
            if (reduced) {
                fadeIn(snap()) togetherWith fadeOut(snap())
            } else {
                (slideInVertically(tween(320)) { it / 2 } + fadeIn(tween(320))) togetherWith
                    (slideOutVertically(tween(240)) { -it / 2 } + fadeOut(tween(200)))
            }
        },
        modifier = modifier,
        label = "lyricLine",
    ) { t ->
        Text(
            t.orEmpty(),
            style = style ?: MaterialTheme.typography.bodyLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

/** Reports whether any part of the element is inside the window. */
fun Modifier.trackVisibility(onChange: (Boolean) -> Unit): Modifier =
    onGloballyPositioned { coords ->
        val b = coords.boundsInWindow()
        onChange(b.width > 0f && b.height > 0f)
    }
