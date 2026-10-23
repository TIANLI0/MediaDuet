package app.mediaduet.ui.screens

import android.graphics.Bitmap
import android.os.SystemClock
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.ui.graphics.TransformOrigin
import androidx.compose.ui.layout.onSizeChanged
import app.mediaduet.handoff.HandoffApp
import app.mediaduet.handoff.HandoffErrors
import app.mediaduet.handoff.HandoffOutcome
import app.mediaduet.handoff.PlayResult
import app.mediaduet.protocol.Caps
import app.mediaduet.ui.components.InfoRow
import app.mediaduet.ui.components.liquid.LiquidToggle
import app.mediaduet.util.VersionCompat
import com.kyant.backdrop.backdrops.layerBackdrop
import com.kyant.backdrop.backdrops.rememberCombinedBackdrop
import com.kyant.backdrop.backdrops.rememberLayerBackdrop
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.AnimatedVisibilityScope
import androidx.compose.animation.Crossfade
import androidx.compose.animation.EnterExitState
import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.animation.ExperimentalSharedTransitionApi
import androidx.compose.animation.SharedTransitionLayout
import androidx.compose.animation.SharedTransitionScope
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.animate
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.snap
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.Orientation
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.draggable
import androidx.compose.foundation.gestures.rememberDraggableState
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Outline
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.mediaduet.R
import app.mediaduet.media.OptimisticPlayback
import app.mediaduet.media.Side
import app.mediaduet.media.SliderSync
import app.mediaduet.media.SnapshotBuilder
import app.mediaduet.media.TargetChoice
import app.mediaduet.protocol.Action
import app.mediaduet.protocol.CommandResult
import app.mediaduet.protocol.ErrorCode
import app.mediaduet.protocol.MediaSessionInfo
import app.mediaduet.protocol.SinkState
import app.mediaduet.service.LyricsSize
import app.mediaduet.protocol.VolumeSet
import app.mediaduet.remote.LyricsDelay
import app.mediaduet.remote.LyricsState
import app.mediaduet.ui.LocalBottomInset
import app.mediaduet.ui.SettingsRoute
import app.mediaduet.ui.UiContext
import app.mediaduet.ui.components.AppIcons
import app.mediaduet.ui.components.EmptyState
import app.mediaduet.ui.components.GlassCard
import app.mediaduet.ui.components.GlassIconButton
import app.mediaduet.ui.components.StatusChip
import app.mediaduet.ui.components.MediaSlider
import app.mediaduet.ui.theme.LocalGlass
import com.kyant.backdrop.Backdrop
import com.kyant.backdrop.drawBackdrop
import com.kyant.backdrop.effects.blur
import com.kyant.backdrop.effects.lens
import com.kyant.backdrop.effects.vibrancy
import com.kyant.shapes.Capsule
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import androidx.compose.ui.platform.LocalContext
import kotlin.math.abs
import kotlin.math.roundToInt

/** Player title / artist (Apple Music scale: large semibold title, regular artist). */
private val TitleLarge = TextStyle(fontSize = 24.sp, lineHeight = 30.sp, fontWeight = FontWeight.SemiBold, letterSpacing = (-0.2).sp)
private val ArtistLarge = TextStyle(fontSize = 19.sp, lineHeight = 25.sp)
private val TitleSmall = TextStyle(fontSize = 17.sp, lineHeight = 22.sp, fontWeight = FontWeight.SemiBold)
private val ArtistSmall = TextStyle(fontSize = 15.sp, lineHeight = 20.sp)

/**
 * After a track change in lyrics mode, lyrics mode stays on this long while nobody is looking for
 * the next track's lyrics any more (the phone's own lookup keeps it on while it runs).
 */
private const val LYRICS_WAIT_MS = 6_000L

/** Everything the player layouts need, resolved once per composition of [PlayerScreen]. */
private class PlayerParts(
    val ui: UiContext,
    val view: PlayerView,
    val playing: Boolean,
    val position: State<Long>,
    val lyricsPosition: State<Long>,
    val controls: PlayerControls,
    val timing: LyricsTiming,
    /** Lyrics of the current track (null while searching / none). */
    val lyrics: LyricsState?,
    /** The phone is still looking for lyrics for this track. */
    val searching: Boolean,
    val translations: Boolean,
    val lyricsSize: LyricsSize,
    val reduced: Boolean,
    val canSeek: Boolean,
    /** Opens the "Up next" sheet (the session exposes a queue). */
    val openQueue: () -> Unit,
) {
    val info: MediaSessionInfo get() = view.info
    val lyricsAvailable: Boolean get() = lyrics != null || searching
}

/** The shared-element scopes of one layout (artwork, title, progress and buttons morph between modes). */
@OptIn(ExperimentalSharedTransitionApi::class)
private class Morph(val shared: SharedTransitionScope, val visibility: AnimatedVisibilityScope, val reduced: Boolean)

/**
 * Apple-Music-style full-screen player for the chosen control target (this phone or the PC); the
 * app's home screen. Portrait: artwork, title / artist, progress, transport, volume and an action
 * row (Lyrics · playback source · Settings). Lyrics mode morphs the artwork into a slim header and
 * gives the lyrics the whole height: they scroll under a compact liquid-glass dock (progress +
 * Lyrics · ◀◀ · ▶ · ▶▶ · Source) that refracts them. Landscape puts the artwork (player) or the
 * artwork + controls (lyrics) on the left and the controls / lyrics on the right.
 */
@OptIn(ExperimentalSharedTransitionApi::class)
@Composable
fun PlayerScreen(ui: UiContext) {
    val engine = ui.engine
    val lyricsMap by engine.lyrics.entries.collectAsStateWithLifecycle()
    val pending by engine.lyrics.pending.collectAsStateWithLifecycle()
    val translations by engine.settings.showTranslations.collectAsStateWithLifecycle()
    val lyricsSize by engine.settings.lyricsSize.collectAsStateWithLifecycle()
    val autoLatency by engine.settings.lyricsAutoLatency.collectAsStateWithLifecycle()
    val offsets by engine.settings.lyricOffsets.collectAsStateWithLifecycle()
    val reduced = rememberReducedMotion()
    val haptics = LocalHapticFeedback.current
    val scope = rememberCoroutineScope()
    val view = rememberPlayerView(engine)
    val bottomInset = LocalBottomInset.current
    val currentUi by rememberUpdatedState(ui)
    var queueOpen by remember { mutableStateOf(false) }

    BoxWithConstraints(Modifier.fillMaxSize()) {
        val landscape = maxWidth > maxHeight * 1.15f
        val side = if (maxWidth >= 600.dp && !landscape) 40.dp else 24.dp
        Column(
            Modifier
                .fillMaxSize()
                .statusBarsPadding()
                .windowInsetsPadding(WindowInsets.displayCutout.only(WindowInsetsSides.Horizontal))
                .padding(start = side, end = side, bottom = bottomInset + 4.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Spacer(Modifier.height(if (landscape) 4.dp else 12.dp))
            StatusPill(ui, view)
            if (view == null) {
                Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
                    EmptyState(AppIcons.Music, stringResource(R.string.empty_nothing_playing_phone))
                }
                ActionRow(ui, null, lyricsAvailable = false, morph = null, onQueue = null)
                return@Column
            }
            val info = view.info
            AutoSwitchHaptic(view)
            val lyrics = engine.lyrics.resolve(info, lyricsMap)
            val searching = lyrics == null && info.trackId in pending
            // Lyrics mode follows a track change: it waits for the next lyrics and ends only when
            // the new track has none.
            var waitOver by remember(info.trackId) { mutableStateOf(false) }
            LaunchedEffect(info.trackId, lyrics == null, searching) {
                if (lyrics == null && !searching) {
                    delay(LYRICS_WAIT_MS)
                    waitOver = true
                    if (currentUi.nav.lyricsMode) currentUi.setNav(currentUi.nav.copy(lyricsMode = false))
                }
            }
            val lyricsMode = ui.nav.lyricsMode && (lyrics != null || searching || !waitOver)

            // ---- optimistic state (reconciled with every device update) -----------------
            var optimistic by remember(view.side, view.key) { mutableStateOf(OptimisticPlayback()) }
            LaunchedEffect(view.positionKey, view.playing) {
                optimistic = optimistic.reconcile(view.playing, view.position(), SystemClock.elapsedRealtime())
            }
            LaunchedEffect(optimistic) {
                if (!optimistic.isIdle) {
                    delay(OptimisticPlayback.SEEK_HOLD_MS + 50)
                    optimistic = optimistic.reconcile(view.playing, view.position(), SystemClock.elapsedRealtime())
                    delay(OptimisticPlayback.CONFIRM_MS)
                    optimistic = optimistic.reconcile(view.playing, view.position(), SystemClock.elapsedRealtime())
                }
            }
            val playing = optimistic.displayPlaying(view.playing)
            val positionKey = Triple(view.positionKey, optimistic.seekPositionMs, playing)
            // Exact position for the seek bar; it is read only inside the controls (no per-frame
            // recomposition of the whole player).
            val position = rememberFramePosition(positionKey, playing, visible = true) { now ->
                optimistic.displayPosition(view.position(now), now)
            }
            // Lyrics: smoothed clock minus output latency (if automatic) and the per-app offset.
            val offsetMs = offsets[info.appId] ?: 0
            val delayMs = LyricsDelay.totalMs(autoLatency, view.outputLatencyMs, offsetMs)
            val lyricsPosition = rememberLyricsClock(
                key = positionKey,
                epoch = view.key to info.trackId,
                playing = playing,
                rate = info.rate,
                delayMs = delayMs,
                active = lyricsMode,
            ) { now -> optimistic.displayPosition(view.position(now), now) }
            val failText = commandFailureText()
            fun handle(kind: OptimisticPlayback.Kind?, r: CommandResult) {
                if (!r.ok) {
                    if (kind != null) optimistic = optimistic.onFailed(kind)
                    haptics.performHapticFeedback(HapticFeedbackType.Reject)
                    ui.toast(failText(r.error?.code))
                }
            }
            val controls = PlayerControls(
                togglePlay = {
                    haptics.performHapticFeedback(HapticFeedbackType.VirtualKey)
                    optimistic = optimistic.onToggle(playing, SystemClock.elapsedRealtime())
                    scope.launch { handle(OptimisticPlayback.Kind.PLAY_PAUSE, ui.command(view, Action.TOGGLE)) }
                },
                skip = { next ->
                    haptics.performHapticFeedback(HapticFeedbackType.VirtualKey)
                    scope.launch { handle(null, ui.command(view, if (next) Action.NEXT else Action.PREVIOUS)) }
                },
                seek = { ms ->
                    haptics.performHapticFeedback(HapticFeedbackType.GestureEnd)
                    optimistic = optimistic.onSeek(ms, SystemClock.elapsedRealtime(), playing, info.rate)
                    scope.launch { handle(OptimisticPlayback.Kind.SEEK, ui.command(view, Action.SEEK, ms)) }
                },
            )
            val parts = PlayerParts(
                ui = ui,
                view = view,
                playing = playing,
                position = position,
                lyricsPosition = lyricsPosition,
                controls = controls,
                timing = LyricsTiming(
                    offsetMs = offsetMs,
                    outputLatencyMs = if (autoLatency) view.outputLatencyMs else 0,
                    set = { ms -> engine.settings.setLyricOffset(info.appId, ms) },
                ),
                lyrics = lyrics,
                searching = searching,
                translations = translations,
                lyricsSize = lyricsSize,
                reduced = reduced,
                canSeek = view.canControl && Action.SEEK in info.actions && (info.durationMs ?: 0) > 0,
                openQueue = { queueOpen = true },
            )

            SharedTransitionLayout(Modifier.weight(1f).fillMaxWidth()) {
                AnimatedContent(
                    targetState = lyricsMode,
                    transitionSpec = {
                        if (reduced) fadeIn(snap()) togetherWith fadeOut(snap())
                        else fadeIn(tween(320, delayMillis = 60)) togetherWith fadeOut(tween(180))
                    },
                    label = "playerMode",
                ) { showLyrics ->
                    val morph = Morph(this@SharedTransitionLayout, this, reduced)
                    when {
                        landscape && showLyrics -> LandscapeLyrics(parts, morph)
                        landscape -> LandscapePlayer(parts, morph)
                        showLyrics -> PortraitLyrics(parts, morph)
                        else -> PortraitPlayer(parts, morph)
                    }
                }
            }
        }
        TargetSheet(ui)
        QueueSheet(ui, view, queueOpen) { queueOpen = false }
    }
}

// ============================================================================ layouts

@Composable
private fun PortraitPlayer(p: PlayerParts, m: Morph) {
    Column(Modifier.fillMaxSize()) {
        ArtworkBlock(p, m, Modifier.weight(1f).fillMaxWidth())
        Spacer(Modifier.height(24.dp))
        Progress(p, m, inlineTimes = false)
        Spacer(Modifier.height(18.dp))
        Transport(p, m, p.ui.backdrop, small = 64.dp, big = 80.dp, spacing = 32.dp)
        Spacer(Modifier.height(18.dp))
        VolumeRow(p.ui, p.view)
        Spacer(Modifier.height(16.dp))
        ActionRow(p.ui, p.view, p.lyricsAvailable, m, onQueue = p.openQueue.takeIf { p.info.queue != null })
    }
}

/**
 * Lyrics mode (portrait): slim header (artwork · title · options), lyrics over the full height,
 * and the compact glass dock floating over the lyrics' faded tail.
 */
@Composable
private fun PortraitLyrics(p: PlayerParts, m: Morph) {
    val density = LocalDensity.current
    val lyricsLayer = rememberLayerBackdrop()
    val dockBackdrop = rememberCombinedBackdrop(p.ui.backdrop, lyricsLayer)
    var dockHeight by remember { mutableStateOf(140.dp) }
    var optionsOpen by remember { mutableStateOf(false) }
    Box(Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize()) {
            LyricsHeader(p, m, optionsOpen) { optionsOpen = !optionsOpen }
            Box(Modifier.weight(1f).fillMaxWidth().layerBackdrop(lyricsLayer)) {
                LyricsBody(p, Modifier.fillMaxSize(), bottomFade = dockHeight + 48.dp, bottomObscured = dockHeight)
            }
        }
        Column(
            Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .onSizeChanged { dockHeight = with(density) { it.height.toDp() } }
                .padding(top = 8.dp),
        ) {
            Progress(p, m, inlineTimes = true)
            Spacer(Modifier.height(6.dp))
            DockRow(p, m, dockBackdrop)
        }
        LyricsOptions(p, optionsOpen, Modifier.align(Alignment.TopEnd).padding(top = 64.dp)) { optionsOpen = false }
    }
}

@Composable
private fun LandscapePlayer(p: PlayerParts, m: Morph) {
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val art = minOf(maxHeight, maxWidth * 0.46f)
        Row(Modifier.fillMaxSize(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(36.dp)) {
            Box(Modifier.width(art).fillMaxHeight(), contentAlignment = Alignment.Center) {
                ArtworkImage(p, m, art)
            }
            Column(
                Modifier.weight(1f).fillMaxHeight().verticalScroll(rememberScrollState()).padding(vertical = 12.dp),
                verticalArrangement = Arrangement.Center,
            ) {
                // Compact: progress with inline times and one row of lyrics · transport · source
                // (like the lyrics dock); Settings sits at the end of the title row.
                TitleRow(p, m, large = true) {
                    CustomActionButtons(p)
                    if (p.info.queue != null) QueueButton(p.openQueue)
                    SettingsButton(p.ui)
                }
                Spacer(Modifier.height(16.dp))
                Progress(p, m, inlineTimes = true)
                Spacer(Modifier.height(8.dp))
                DockRow(p, m, p.ui.backdrop)
                Spacer(Modifier.height(6.dp))
                VolumeRow(p.ui, p.view)
            }
        }
    }
}

/** Lyrics mode (landscape): artwork, title and the dock on the left; lyrics over the full height on the right. */
@Composable
private fun LandscapeLyrics(p: PlayerParts, m: Morph) {
    val lyricsLayer = rememberLayerBackdrop()
    val dockBackdrop = rememberCombinedBackdrop(p.ui.backdrop, lyricsLayer)
    var optionsOpen by remember { mutableStateOf(false) }
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val leftWidth = (maxWidth * 0.4f).coerceIn(288.dp, 400.dp)
        val art = minOf(leftWidth * 0.46f, maxHeight * 0.34f)
        Row(Modifier.fillMaxSize(), horizontalArrangement = Arrangement.spacedBy(28.dp)) {
            Column(
                // Vertical padding inside the scroll keeps the glass buttons' rims and shadows unclipped.
                Modifier.width(leftWidth).fillMaxHeight().verticalScroll(rememberScrollState()).padding(vertical = 12.dp),
                verticalArrangement = Arrangement.Center,
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    ArtworkImage(p, m, art)
                }
                Spacer(Modifier.height(14.dp))
                TitleRow(p, m, large = false)
                Spacer(Modifier.height(14.dp))
                Progress(p, m, inlineTimes = true)
                Spacer(Modifier.height(6.dp))
                DockRow(p, m, dockBackdrop)
            }
            Box(Modifier.weight(1f).fillMaxHeight()) {
                Box(Modifier.fillMaxSize().layerBackdrop(lyricsLayer)) {
                    LyricsBody(p, Modifier.fillMaxSize(), bottomFade = 72.dp, bottomObscured = 0.dp)
                }
                if (hasLyricsOptions(p)) {
                    OptionsButton(p, m, optionsOpen, Modifier.align(Alignment.TopEnd).padding(top = 8.dp)) { optionsOpen = !optionsOpen }
                }
                LyricsOptions(p, optionsOpen, Modifier.align(Alignment.TopEnd).padding(top = 60.dp)) { optionsOpen = false }
            }
        }
    }
}

// ============================================================================ pieces

/** Artwork (swipe for next / previous, tap for lyrics) with the title row under it. */
@Composable
private fun ArtworkBlock(p: PlayerParts, m: Morph, modifier: Modifier) {
    val density = LocalDensity.current
    BoxWithConstraints(modifier) {
        // Title block (two text lines at the current font scale) + its spacing below the artwork.
        val titleBlock = with(density) { TitleLarge.lineHeight.toDp() + ArtistLarge.lineHeight.toDp() } + 24.dp
        val side = minOf(maxWidth, maxHeight - titleBlock).coerceAtLeast(96.dp)
        Column(Modifier.fillMaxSize()) {
            Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
                ArtworkImage(p, m, side)
            }
            Spacer(Modifier.height(24.dp))
            TitleRow(p, m, large = true, trailing = if (p.info.customActions.isNotEmpty()) ({ CustomActionButtons(p) }) else null)
        }
    }
}

@Composable
private fun TitleRow(p: PlayerParts, m: Morph, large: Boolean, trailing: (@Composable () -> Unit)? = null) {
    val glass = LocalGlass.current
    Row(verticalAlignment = Alignment.CenterVertically) {
        TitleBlock(p.info, Modifier.weight(1f).sharedText(m, "title"), large = large)
        if (!p.view.connected) {
            Spacer(Modifier.width(8.dp))
            StatusChip(stringResource(R.string.stale), dot = glass.warn)
        }
        if (trailing != null) {
            Spacer(Modifier.width(8.dp))
            trailing()
        }
    }
}

/** The big artwork: shrinks while paused, swipes for next / previous, tap opens lyrics. */
@Composable
private fun ArtworkImage(p: PlayerParts, m: Morph, side: androidx.compose.ui.unit.Dp) {
    val haptics = LocalHapticFeedback.current
    val scope = rememberCoroutineScope()
    val view = p.view
    val info = view.info
    // Horizontal artwork offset: set synchronously while dragging; only the release animates.
    var swipe by remember { mutableFloatStateOf(0f) }
    var swipeJob by remember { mutableStateOf<kotlinx.coroutines.Job?>(null) }
    val artScale by animateFloatAsState(
        if (p.playing || p.reduced) 1f else 0.86f,
        if (p.reduced) snap() else spring(dampingRatio = 0.72f, stiffness = 220f),
        label = "artScale",
    )
    val unavailable = stringResource(R.string.lyrics_unavailable)
    Artwork(
        view.art,
        Modifier
            .shared(m, "artwork")
            .size(side)
            .graphicsLayer {
                scaleX = artScale
                scaleY = artScale
                translationX = swipe
                alpha = 1f - (abs(swipe) / size.width.coerceAtLeast(1f)).coerceIn(0f, 0.6f)
            }
            // Horizontal swipe: next (left) / previous (right), when supported.
            .pointerInput(view.key, info.actions, view.canControl) {
                val threshold = 72.dp.toPx()
                val widthPx = size.width.toFloat()
                var crossed = false
                fun settle(block: suspend () -> Unit) {
                    swipeJob?.cancel()
                    swipeJob = scope.launch { block() }
                }
                suspend fun animateSwipe(to: Float, spec: androidx.compose.animation.core.AnimationSpec<Float>) =
                    animate(swipe, to, animationSpec = spec) { v, _ -> swipe = v }
                detectHorizontalDragGestures(
                    onDragStart = {
                        crossed = false
                        swipeJob?.cancel()
                    },
                    onHorizontalDrag = { change, dx ->
                        change.consume()
                        swipe += dx * 0.6f
                        if (!crossed && abs(swipe) > threshold) {
                            crossed = true
                            haptics.performHapticFeedback(HapticFeedbackType.GestureThresholdActivate)
                        }
                    },
                    onDragEnd = {
                        val next = swipe < 0
                        val action = if (next) Action.NEXT else Action.PREVIOUS
                        val allowed = view.canControl && action in info.actions
                        val past = abs(swipe) > threshold
                        if (past && allowed) p.controls.skip(next)
                        if (past && !allowed) haptics.performHapticFeedback(HapticFeedbackType.Reject)
                        settle {
                            if (past && allowed && !p.reduced) {
                                // Slide out, re-enter from the other side.
                                animateSwipe(if (next) -widthPx else widthPx, tween(160))
                                swipe = if (next) widthPx * 0.4f else -widthPx * 0.4f
                            }
                            animateSwipe(0f, if (p.reduced) snap() else spring(dampingRatio = 0.75f, stiffness = 300f))
                        }
                    },
                    onDragCancel = { settle { animateSwipe(0f, spring()) } },
                )
            }
            .clickable(remember { MutableInteractionSource() }, indication = null) {
                if (p.lyricsAvailable) p.ui.setNav(p.ui.nav.copy(lyricsMode = true)) else p.ui.toast(unavailable)
            },
    )
}

/** Lyrics mode header: small artwork + title (tap: back to the artwork) and the options button. */
@Composable
private fun LyricsHeader(p: PlayerParts, m: Morph, optionsOpen: Boolean, onOptions: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .padding(top = 4.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Row(
            Modifier
                .weight(1f)
                .clickable(remember { MutableInteractionSource() }, indication = null) { p.ui.setNav(p.ui.nav.copy(lyricsMode = false)) },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Artwork(p.view.art, Modifier.shared(m, "artwork").size(52.dp))
            Spacer(Modifier.width(12.dp))
            TitleBlock(p.info, Modifier.weight(1f).sharedText(m, "title"), large = false)
        }
        if (hasLyricsOptions(p)) {
            Spacer(Modifier.width(8.dp))
            OptionsButton(p, m, optionsOpen, Modifier, onOptions)
        }
    }
}

private fun hasLyricsOptions(p: PlayerParts): Boolean = p.lyrics?.synced == true

/**
 * One small glass button for the lyrics options (translation, timing) instead of a row of buttons.
 *
 * Liquid glass must never be drawn half transparent: under the mode cross-fade's alpha its blurred
 * shadow and lens composite into a dark smudge for a few frames. So the button exists only while
 * lyrics mode is settled — it leaves at once when the mode starts to change and pops in with a
 * scale-only spring once the cross-fade is over. Its space is kept, so the title never reflows.
 */
@Composable
private fun OptionsButton(p: PlayerParts, m: Morph, open: Boolean, modifier: Modifier, onClick: () -> Unit) {
    val modeTransition = m.visibility.transition
    val settled = modeTransition.currentState == EnterExitState.Visible && modeTransition.targetState == EnterExitState.Visible
    Box(modifier.size(40.dp)) {
        AnimatedVisibility(
            visible = settled,
            enter = if (p.reduced) EnterTransition.None else scaleIn(spring(dampingRatio = 0.6f, stiffness = 500f), initialScale = 0.5f),
            exit = ExitTransition.None,
        ) {
            OptionsGlassButton(p, open, onClick)
        }
    }
}

@Composable
private fun OptionsGlassButton(p: PlayerParts, open: Boolean, onClick: () -> Unit) {
    val haptics = LocalHapticFeedback.current
    GlassIconButton(
        AppIcons.More,
        stringResource(R.string.lyrics_options),
        {
            haptics.performHapticFeedback(HapticFeedbackType.ContextClick)
            onClick()
        },
        p.ui.backdrop,
        Modifier,
        size = 40.dp,
        iconSize = 20.dp,
        active = open || p.timing.offsetMs != 0,
    )
}

/** The lyrics themselves: synced (karaoke) or plain, a quiet "looking for lyrics" while searching. */
@Composable
private fun LyricsBody(p: PlayerParts, modifier: Modifier, bottomFade: androidx.compose.ui.unit.Dp, bottomObscured: androidx.compose.ui.unit.Dp) {
    Crossfade(targetState = p.lyrics, animationSpec = tween(if (p.reduced) 0 else 360), modifier = modifier, label = "lyricsBody") { shown ->
        val synced = shown?.lyrics
        when {
            shown == null -> LyricsSearching(Modifier.fillMaxSize().padding(bottom = bottomObscured), p.reduced)
            synced != null && !synced.isEmpty -> SyncedLyrics(
                synced, p.lyricsPosition, if (p.canSeek) p.controls.seek else null, p.ui.backdrop, Modifier.fillMaxSize(),
                showTranslations = p.translations, bottomFade = bottomFade, bottomObscured = bottomObscured,
                textScale = p.lyricsSize.scale,
            )
            else -> PlainLyrics(
                shown.plain,
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(top = 16.dp, bottom = bottomObscured + 16.dp),
            )
        }
    }
}

@Composable
private fun LyricsSearching(modifier: Modifier, reduced: Boolean) {
    val color = MaterialTheme.colorScheme.onSurfaceVariant
    val t = if (reduced) null else rememberInfiniteTransition(label = "searching")
    val phase = t?.animateFloat(0f, 1f, infiniteRepeatable(tween(1_400, easing = LinearEasing)), label = "phase")
    Column(modifier, verticalArrangement = Arrangement.Center, horizontalAlignment = Alignment.Start) {
        Row(Modifier.height(16.dp), horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
            for (i in 0 until 3) {
                Box(
                    Modifier
                        .size(8.dp)
                        .graphicsLayer {
                            val v = phase?.value ?: 0f
                            val local = ((v * 3f - i) % 3f + 3f) % 3f
                            alpha = 0.3f + 0.7f * (1f - (local / 1.5f).coerceAtMost(1f))
                        }
                        .background(color, RoundedCornerShape(50)),
                )
            }
        }
        Spacer(Modifier.height(12.dp))
        Text(stringResource(R.string.lyrics_searching), style = MaterialTheme.typography.titleMedium, color = color)
    }
}

/**
 * Lyrics options popover: translation on / off (when the lyrics have translations) and the
 * per-app timing offset (−/+ 0.1 s, reset; the automatic output delay is noted). Closes on an
 * outside tap or 5 s after the last touch.
 */
@Composable
private fun LyricsOptions(p: PlayerParts, open: Boolean, modifier: Modifier, onDismiss: () -> Unit) {
    var touch by remember { mutableIntStateOf(0) }
    LaunchedEffect(open, touch) {
        if (open) {
            delay(5_000)
            onDismiss()
        }
    }
    if (open) {
        Box(Modifier.fillMaxSize().clickable(remember { MutableInteractionSource() }, indication = null, onClick = onDismiss))
    }
    AnimatedVisibility(
        visible = open,
        enter = if (p.reduced) fadeIn(snap()) else fadeIn(tween(160)) + scaleIn(spring(dampingRatio = 0.7f, stiffness = 500f), initialScale = 0.85f, transformOrigin = TransformOrigin(1f, 0f)),
        exit = if (p.reduced) fadeOut(snap()) else fadeOut(tween(140)) + scaleOut(targetScale = 0.9f, transformOrigin = TransformOrigin(1f, 0f)),
        modifier = modifier,
    ) {
        GlassCard(p.ui.backdrop, Modifier.width(296.dp), strong = true, cornerRadius = 26.dp, contentPadding = PaddingValues(horizontal = 18.dp, vertical = 10.dp)) {
            val settings = p.ui.engine.settings
            if (p.lyrics?.lyrics?.hasTranslations == true) {
                InfoRow(
                    stringResource(R.string.settings_translations),
                    leading = { Icon(AppIcons.Translate, null, Modifier.size(20.dp)) },
                    trailing = {
                        LiquidToggle({ p.translations }, {
                            touch++
                            settings.setShowTranslations(it)
                        }, p.ui.backdrop, contentDescription = stringResource(R.string.settings_translations))
                    },
                    onClick = {
                        touch++
                        settings.setShowTranslations(!p.translations)
                    },
                    role = Role.Switch,
                )
            }
            if (p.lyrics?.synced == true) {
                if (p.lyrics.lyrics?.hasTranslations == true) {
                    HorizontalDivider(Modifier.padding(start = 36.dp), thickness = 0.5.dp, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f))
                }
                LyricsSizePicker(p.lyricsSize) {
                    touch++
                    settings.setLyricsSize(it)
                }
                HorizontalDivider(Modifier.padding(start = 36.dp), thickness = 0.5.dp, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f))
                OffsetStepper(p.timing) { touch++ }
            }
        }
    }
}

/** Lyrics text size: three steps (small · standard · large) as one segmented pill. */
@Composable
fun LyricsSizePicker(current: LyricsSize, onPick: (LyricsSize) -> Unit) {
    val glass = LocalGlass.current
    val haptics = LocalHapticFeedback.current
    Row(Modifier.fillMaxWidth().heightIn(min = 52.dp), verticalAlignment = Alignment.CenterVertically) {
        Icon(AppIcons.TextSize, null, Modifier.size(20.dp))
        Spacer(Modifier.width(16.dp))
        Text(stringResource(R.string.lyrics_size), style = MaterialTheme.typography.bodyLarge)
    }
    Row(
        Modifier
            .fillMaxWidth()
            .padding(bottom = 6.dp)
            .clip(RoundedCornerShape(50))
            .background(glass.fill)
            .padding(4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        for ((size, label, sp) in listOf(
            Triple(LyricsSize.SMALL, R.string.lyrics_size_small, 13),
            Triple(LyricsSize.STANDARD, R.string.lyrics_size_standard, 15),
            Triple(LyricsSize.LARGE, R.string.lyrics_size_large, 18),
        )) {
            val selected = size == current
            Box(
                Modifier
                    .weight(1f)
                    .heightIn(min = 36.dp)
                    .clip(RoundedCornerShape(50))
                    .background(if (selected) glass.controlActive else Color.Transparent)
                    .clickable(role = Role.RadioButton) {
                        if (!selected) {
                            haptics.performHapticFeedback(HapticFeedbackType.SegmentTick)
                            onPick(size)
                        }
                    },
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    stringResource(label),
                    fontSize = sp.sp,
                    fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal,
                    color = if (selected) glass.accent else MaterialTheme.colorScheme.onSurface,
                )
            }
        }
    }
}

/** Lyrics timing: −0.1 s / value / +0.1 s / reset, with haptics. Positive = lyrics later. */
@Composable
private fun OffsetStepper(timing: LyricsTiming, onTouch: () -> Unit) {
    val glass = LocalGlass.current
    val haptics = LocalHapticFeedback.current
    fun step(steps: Int) {
        onTouch()
        val next = LyricsDelay.step(timing.offsetMs, steps)
        if (next == timing.offsetMs) {
            haptics.performHapticFeedback(HapticFeedbackType.Reject)
        } else {
            haptics.performHapticFeedback(HapticFeedbackType.SegmentTick)
            timing.set(next)
        }
    }
    Row(Modifier.fillMaxWidth().heightIn(min = 56.dp), verticalAlignment = Alignment.CenterVertically) {
        Icon(AppIcons.Timer, null, Modifier.size(20.dp))
        Spacer(Modifier.width(16.dp))
        Column(Modifier.weight(1f)) {
            Text(stringResource(R.string.lyrics_timing), style = MaterialTheme.typography.bodyLarge)
            Text(
                if (timing.outputLatencyMs > 0) stringResource(R.string.lyrics_offset_auto, timing.outputLatencyMs)
                else stringResource(R.string.lyrics_offset_hint),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 2,
            )
        }
    }
    Row(
        Modifier
            .fillMaxWidth()
            .padding(bottom = 6.dp)
            .clip(RoundedCornerShape(50))
            .background(glass.fill),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        OffsetButton(AppIcons.Minus, stringResource(R.string.lyrics_offset_earlier)) { step(-1) }
        Text(
            stringResource(R.string.lyrics_offset_value, LyricsDelay.signedSeconds(timing.offsetMs)),
            style = MaterialTheme.typography.titleMedium,
            color = if (timing.offsetMs != 0) glass.accent else MaterialTheme.colorScheme.onSurface,
            textAlign = TextAlign.Center,
            modifier = Modifier.weight(1f),
        )
        OffsetButton(AppIcons.Plus, stringResource(R.string.lyrics_offset_later)) { step(1) }
        OffsetButton(AppIcons.Restore, stringResource(R.string.lyrics_offset_reset), enabled = timing.offsetMs != 0) {
            onTouch()
            haptics.performHapticFeedback(HapticFeedbackType.Confirm)
            timing.set(0)
        }
    }
}

@Composable
private fun OffsetButton(icon: ImageVector, description: String, enabled: Boolean = true, onClick: () -> Unit) {
    Box(
        Modifier
            .size(44.dp)
            .clip(RoundedCornerShape(50))
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            icon, description, Modifier.size(20.dp),
            tint = MaterialTheme.colorScheme.onSurface.copy(alpha = if (enabled) 1f else 0.35f),
        )
    }
}

/**
 * Seek bar with times: under the bar (player) or beside it on one row (lyrics dock, saves a row).
 * The dragged value is shown live and committed only on release; the optimistic position then
 * holds until the device confirms (no rubber-banding).
 */
@Composable
private fun Progress(p: PlayerParts, m: Morph, inlineTimes: Boolean) {
    val session = p.info
    val position = p.position
    var dragFraction by remember { mutableStateOf<Float?>(null) }
    val duration = session.durationMs
    // Quantised (≈ 1/2000 of the track) so the slider is not re-targeted every frame.
    val live by remember(duration) {
        derivedStateOf {
            if (duration != null && duration > 0) ((position.value.toFloat() / duration).coerceIn(0f, 1f) * 2000f).roundToInt() / 2000f else 0f
        }
    }
    var scrubbing by remember { mutableStateOf(false) }
    // Time labels change once per second, not every frame; they light up while scrubbing.
    val seconds by remember { derivedStateOf { position.value / 1000 } }
    val labelColor by animateColorAsState(
        if (scrubbing) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurfaceVariant,
        label = "timeLabels",
    )
    val shown = dragFraction?.let { f -> duration?.let { (f * it).toLong() } } ?: (seconds * 1000)
    val elapsed = formatTime(shown)
    val remaining = if (duration != null) stringResource(R.string.time_remaining, formatTime(((duration - shown) / 1000 * 1000).coerceAtLeast(0))) else formatTime(null)
    val slider = @Composable { modifier: Modifier ->
        MediaSlider(
            value = { dragFraction ?: live },
            onValueChange = { dragFraction = it },
            modifier = modifier.shared(m, "progress"),
            enabled = p.canSeek,
            contentDescription = stringResource(R.string.a11y_playback_position),
            onActiveChange = { scrubbing = it },
            onValueChangeFinished = { f ->
                if (duration != null) p.controls.seek((f * duration).toLong())
                dragFraction = null
            },
        )
    }
    val labelStyle = MaterialTheme.typography.labelMedium.copy(fontFeatureSettings = "tnum")
    if (inlineTimes) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(elapsed, style = labelStyle, color = labelColor, modifier = Modifier.widthIn(min = 40.dp))
            Spacer(Modifier.width(10.dp))
            slider(Modifier.weight(1f))
            Spacer(Modifier.width(10.dp))
            Text(remaining, style = labelStyle, color = labelColor, textAlign = TextAlign.End, modifier = Modifier.widthIn(min = 44.dp))
        }
    } else {
        slider(Modifier.fillMaxWidth())
        Row(Modifier.fillMaxWidth()) {
            Text(elapsed, style = labelStyle, color = labelColor)
            Spacer(Modifier.weight(1f))
            Text(remaining, style = labelStyle, color = labelColor)
        }
    }
}

/** Previous · Play/Pause · Next as liquid-glass buttons (shared between the modes, so they morph). */
@Composable
private fun Transport(p: PlayerParts, m: Morph, backdrop: Backdrop, small: androidx.compose.ui.unit.Dp, big: androidx.compose.ui.unit.Dp, spacing: androidx.compose.ui.unit.Dp) {
    Row(
        Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(spacing, Alignment.CenterHorizontally),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        TransportButtons(p, m, backdrop, small, big)
    }
}

@Composable
private fun TransportButtons(p: PlayerParts, m: Morph, backdrop: Backdrop, small: androidx.compose.ui.unit.Dp, big: androidx.compose.ui.unit.Dp) {
    val session = p.info
    val canControl = p.view.canControl
    GlassIconButton(
        AppIcons.Previous, stringResource(R.string.action_previous), { p.controls.skip(false) }, backdrop,
        Modifier.shared(m, "prev"),
        size = small, iconSize = small * 0.42f,
        enabled = canControl && Action.PREVIOUS in session.actions,
    )
    GlassIconButton(
        if (p.playing) AppIcons.Pause else AppIcons.Play,
        stringResource(if (p.playing) R.string.action_pause else R.string.action_play),
        p.controls.togglePlay, backdrop,
        Modifier.shared(m, "play"),
        size = big, iconSize = big * 0.42f,
        enabled = canControl && (Action.TOGGLE in session.actions || Action.PLAY in session.actions || Action.PAUSE in session.actions),
    )
    GlassIconButton(
        AppIcons.Next, stringResource(R.string.action_next), { p.controls.skip(true) }, backdrop,
        Modifier.shared(m, "next"),
        size = small, iconSize = small * 0.42f,
        enabled = canControl && Action.NEXT in session.actions,
    )
}

/** The lyrics dock's single row: Lyrics · ◀◀ ▶ ▶▶ · Source. */
@Composable
private fun DockRow(p: PlayerParts, m: Morph, backdrop: Backdrop) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        LyricsToggle(p.ui, p.lyricsAvailable, m)
        Spacer(Modifier.weight(1f))
        Row(horizontalArrangement = Arrangement.spacedBy(14.dp), verticalAlignment = Alignment.CenterVertically) {
            TransportButtons(p, m, backdrop, small = 48.dp, big = 60.dp)
        }
        Spacer(Modifier.weight(1f))
        SourceIconButton(p.ui, p.view, m)
    }
}

/** Lyrics on / off: accent with a soft fill while on, dimmed (not hidden) when there are none. */
@Composable
private fun LyricsToggle(ui: UiContext, lyricsAvailable: Boolean, m: Morph?) {
    val haptics = LocalHapticFeedback.current
    val glass = LocalGlass.current
    val secondary = MaterialTheme.colorScheme.onSurfaceVariant
    val active = ui.nav.lyricsMode && lyricsAvailable
    val unavailable = stringResource(R.string.lyrics_unavailable)
    Box(
        Modifier
            .then(if (m != null) Modifier.shared(m, "lyricsBtn") else Modifier)
            .size(48.dp)
            .clip(RoundedCornerShape(14.dp))
            .background(if (active) glass.fill else Color.Transparent)
            .clickable(role = Role.Button, onClickLabel = if (lyricsAvailable) null else unavailable) {
                if (!lyricsAvailable) {
                    haptics.performHapticFeedback(HapticFeedbackType.Reject)
                    ui.toast(unavailable)
                } else {
                    haptics.performHapticFeedback(if (active) HapticFeedbackType.ToggleOff else HapticFeedbackType.ToggleOn)
                    ui.setNav(ui.nav.copy(lyricsMode = !active))
                }
            },
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            AppIcons.QuoteBubble,
            when {
                !lyricsAvailable -> unavailable
                active -> stringResource(R.string.lyrics_hide)
                else -> stringResource(R.string.lyrics_show)
            },
            Modifier.size(24.dp),
            tint = when {
                active -> glass.accent
                lyricsAvailable -> secondary
                else -> secondary.copy(alpha = secondary.alpha * 0.4f)
            },
        )
    }
}

/** Where the playback is, icon only (lyrics dock); opens the source sheet. */
@Composable
private fun SourceIconButton(ui: UiContext, view: PlayerView, m: Morph) {
    val haptics = LocalHapticFeedback.current
    Box(
        Modifier
            .size(48.dp)
            .clip(RoundedCornerShape(14.dp))
            .clickable(role = Role.Button, onClickLabel = stringResource(R.string.source_title)) {
                haptics.performHapticFeedback(HapticFeedbackType.ContextClick)
                ui.setNav(ui.nav.copy(sheetOpen = true))
            },
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            if (view.side == Side.PC) AppIcons.Computer else AppIcons.Phone,
            deviceLabel(view),
            Modifier.shared(m, "source").size(22.dp),
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

/**
 * One small, non-blocking status pill for the most important issue (missing access, no paired
 * PC, PC not connected, app versions that don't match); tapping it opens the relevant settings
 * page. "This phone only" mode shows only the access issue.
 */
@Composable
private fun StatusPill(ui: UiContext, view: PlayerView?) {
    val peers by ui.engine.trust.peers.collectAsStateWithLifecycle()
    val link by ui.engine.link.status.collectAsStateWithLifecycle()
    val mode by ui.engine.settings.mode.collectAsStateWithLifecycle()
    val dismissed by ui.engine.settings.dismissedVersionWarning.collectAsStateWithLifecycle()
    val current = link.current
    val versionWarning = current?.let { c ->
        val local = app.mediaduet.BuildConfig.VERSION_NAME
        c.takeIf { VersionCompat.compare(local, it.peer.app) == VersionCompat.Level.MISMATCH && dismissed != "$local|${it.peer.app}" }
    }
    val (text, route) = when {
        !ui.permissions.listenerAccess -> stringResource(R.string.banner_access) to SettingsRoute.PERMISSIONS
        !mode.linked -> return
        peers.isEmpty() -> stringResource(R.string.banner_pair) to SettingsRoute.CONNECTION
        current == null || view?.fallback == true -> stringResource(R.string.pill_pc_disconnected) to SettingsRoute.CONNECTION
        versionWarning != null -> stringResource(R.string.version_pill, versionWarning.peer.name) to SettingsRoute.CONNECTION
        else -> return
    }
    val glass = LocalGlass.current
    Row(
        Modifier
            .padding(bottom = 12.dp)
            .clip(RoundedCornerShape(50))
            .background(glass.fill)
            .clickable(role = Role.Button) { ui.openSettings(route) }
            .heightIn(min = 40.dp)
            .padding(horizontal = 16.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(8.dp).background(glass.warn, RoundedCornerShape(50)))
        Spacer(Modifier.width(10.dp))
        Text(text, style = MaterialTheme.typography.labelLarge, maxLines = 2)
        Icon(AppIcons.ChevronRight, null, Modifier.size(18.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@OptIn(ExperimentalSharedTransitionApi::class)
@Composable
private fun Modifier.shared(m: Morph, key: String): Modifier = with(m.shared) {
    this@shared.sharedElement(
        rememberSharedContentState(key),
        m.visibility,
        boundsTransform = { _, _ -> if (m.reduced) snap() else spring(dampingRatio = 0.86f, stiffness = 260f) },
    )
}

/** Text that changes size between the modes: both versions scale into the animated bounds. */
@OptIn(ExperimentalSharedTransitionApi::class)
@Composable
private fun Modifier.sharedText(m: Morph, key: String): Modifier = with(m.shared) {
    this@sharedText.sharedBounds(
        rememberSharedContentState(key),
        m.visibility,
        enter = fadeIn(if (m.reduced) snap() else tween(220)),
        exit = fadeOut(if (m.reduced) snap() else tween(160)),
        boundsTransform = { _, _ -> if (m.reduced) snap() else spring(dampingRatio = 0.86f, stiffness = 260f) },
        resizeMode = SharedTransitionScope.ResizeMode.scaleToBounds(ContentScale.FillWidth, Alignment.CenterStart),
    )
}

/** "This phone · App" / "PC name · App". */
@Composable
fun deviceLabel(view: PlayerView): String {
    val device = if (view.side == Side.PHONE) stringResource(R.string.this_phone) else view.deviceName ?: stringResource(R.string.pc_generic)
    val app = view.info.appName.ifEmpty { view.info.appId }
    return if (app.isEmpty()) device else listOf(device, app).joinToString(stringResource(R.string.list_separator))
}

/**
 * Apple-Music-style action row at the bottom of the player: Lyrics (left), playback source with a
 * caption (centre, opens the source sheet) and Settings (right). Plain icons tinted like secondary
 * text with 48 dp targets.
 */
@Composable
private fun ActionRow(ui: UiContext, view: PlayerView?, lyricsAvailable: Boolean, morph: Morph?, onQueue: (() -> Unit)?) {
    val haptics = LocalHapticFeedback.current
    val secondary = MaterialTheme.colorScheme.onSurfaceVariant
    Row(Modifier.fillMaxWidth().heightIn(min = 56.dp), verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.weight(1f), contentAlignment = Alignment.CenterStart) {
            LyricsToggle(ui, lyricsAvailable, morph)
        }
        // Centre: where the playback is (icon + caption, like Apple Music's AirPlay row).
        val sink by ui.engine.remote.sink.collectAsStateWithLifecycle()
        val base = when {
            view == null -> stringResource(R.string.source_title)
            view.automatic -> stringResource(R.string.target_auto_caption, deviceLabel(view))
            else -> deviceLabel(view)
        }
        val caption = if (sink?.state == SinkState.ON) stringResource(R.string.pc_speaker_caption, base) else base
        Column(
            Modifier
                .weight(2f)
                .clip(RoundedCornerShape(14.dp))
                .clickable(role = Role.Button, onClickLabel = stringResource(R.string.source_title)) {
                    haptics.performHapticFeedback(HapticFeedbackType.ContextClick)
                    ui.setNav(ui.nav.copy(sheetOpen = true))
                }
                .heightIn(min = 48.dp)
                .padding(horizontal = 8.dp, vertical = 4.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            Icon(
                when (view?.side) {
                    Side.PC -> AppIcons.Computer
                    Side.PHONE -> AppIcons.Phone
                    null -> AppIcons.Devices
                },
                null,
                (if (morph != null) Modifier.shared(morph, "source") else Modifier).size(22.dp),
                tint = secondary,
            )
            Text(
                caption,
                style = MaterialTheme.typography.labelSmall,
                color = secondary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center,
                modifier = Modifier.padding(top = 2.dp),
            )
        }
        Box(Modifier.weight(1f), contentAlignment = Alignment.CenterEnd) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (onQueue != null) QueueButton(onQueue)
                SettingsButton(ui)
            }
        }
    }
}

/** "Up next": opens the queue sheet. */
@Composable
private fun QueueButton(onClick: () -> Unit) {
    val haptics = LocalHapticFeedback.current
    Box(
        Modifier
            .size(48.dp)
            .clip(RoundedCornerShape(14.dp))
            .clickable(role = Role.Button) {
                haptics.performHapticFeedback(HapticFeedbackType.ContextClick)
                onClick()
            },
        contentAlignment = Alignment.Center,
    ) {
        Icon(AppIcons.Playlist, stringResource(R.string.queue_title), Modifier.size(24.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

/**
 * The player's own buttons (Android custom actions: like / favourite, shuffle, …) next to the
 * title, Apple-Music-style: the app's icon tinted like secondary text (its title when it has
 * none), at most three; a press answers with a tick or, when refused, a toast.
 */
@Composable
private fun CustomActionButtons(p: PlayerParts) {
    val actions = p.info.customActions.take(3)
    if (actions.isEmpty() || p.view.side != Side.PHONE) return
    val haptics = LocalHapticFeedback.current
    val scope = rememberCoroutineScope()
    val failText = commandFailureText()
    for (a in actions) {
        val hash = a.icon?.hash
        val icon = remember(hash) {
            hash?.let { p.ui.engine.artwork.bytes(it) }?.let { android.graphics.BitmapFactory.decodeByteArray(it, 0, it.size)?.asImageBitmap() }
        }
        Box(
            Modifier
                .heightIn(min = 44.dp)
                .widthIn(min = 44.dp)
                .clip(RoundedCornerShape(50))
                .clickable(enabled = p.view.canControl, role = Role.Button, onClickLabel = a.title.ifEmpty { null }) {
                    haptics.performHapticFeedback(HapticFeedbackType.Confirm)
                    scope.launch {
                        val r = p.ui.customAction(p.view, a.id)
                        if (!r.ok) {
                            haptics.performHapticFeedback(HapticFeedbackType.Reject)
                            p.ui.toast(failText(r.error?.code))
                        }
                    }
                },
            contentAlignment = Alignment.Center,
        ) {
            if (icon != null) {
                Icon(icon, a.title, Modifier.size(24.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                Text(
                    a.title,
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.widthIn(max = 72.dp).padding(horizontal = 8.dp),
                )
            }
        }
    }
}

/**
 * "Up next": the session's play queue (current entry highlighted and scrolled into view); tapping
 * an entry plays it. A glass sheet like the source sheet.
 */
@Composable
private fun QueueSheet(ui: UiContext, view: PlayerView?, open: Boolean, onClose: () -> Unit) {
    val queues by ui.engine.media.queues.collectAsStateWithLifecycle()
    val pcQueues by ui.engine.remote.queues.collectAsStateWithLifecycle()
    val queue = view?.let { if (it.side == Side.PHONE) queues[it.key] else pcQueues[it.key] }
    val reduced = rememberReducedMotion()
    val glass = LocalGlass.current
    val haptics = LocalHapticFeedback.current
    val scope = rememberCoroutineScope()
    val failText = commandFailureText()
    val visible = open && queue != null
    LaunchedEffect(open, queue == null) { if (open && queue == null) onClose() }
    AnimatedVisibility(visible, enter = fadeIn(if (reduced) snap() else tween(200)), exit = fadeOut(if (reduced) snap() else tween(200))) {
        Box(
            Modifier
                .fillMaxSize()
                .background(Color.Black.copy(alpha = if (glass.isDark) 0.4f else 0.18f))
                .clickable(remember { MutableInteractionSource() }, indication = null, onClick = onClose),
        )
    }
    AnimatedVisibility(
        visible,
        enter = if (reduced) fadeIn(snap()) else slideInVertically(spring(dampingRatio = 0.85f, stiffness = 400f)) { it },
        exit = if (reduced) fadeOut(snap()) else slideOutVertically(tween(220)) { it },
        modifier = Modifier.fillMaxSize(),
    ) {
        val q = queue ?: return@AnimatedVisibility
        Box(
            Modifier.fillMaxSize().padding(start = 16.dp, end = 16.dp, top = 72.dp, bottom = LocalBottomInset.current + 8.dp),
            contentAlignment = Alignment.BottomCenter,
        ) {
            GlassCard(ui.backdrop, Modifier.widthIn(max = 560.dp).fillMaxWidth(), strong = true, cornerRadius = 28.dp,
                contentPadding = PaddingValues(start = 20.dp, end = 20.dp, top = 20.dp, bottom = 8.dp)) {
                Text(stringResource(R.string.queue_title), style = MaterialTheme.typography.titleMedium)
                val subtitle = listOf(q.title, pluralStringResource(R.plurals.queue_count, q.total, q.total))
                    .filter { it.isNotEmpty() }.joinToString(stringResource(R.string.list_separator))
                Text(subtitle, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Spacer(Modifier.height(8.dp))
                val activeIndex = q.items.indexOfFirst { it.id == q.activeId }
                val listState = androidx.compose.foundation.lazy.rememberLazyListState((activeIndex - 1).coerceAtLeast(0))
                androidx.compose.foundation.lazy.LazyColumn(Modifier.fillMaxWidth().heightIn(max = 480.dp), state = listState) {
                    // No item keys: players may repeat a queue id (the same song twice, or every
                    // entry 0), and duplicate keys crash the list.
                    items(q.items.size) { i ->
                        val e = q.items[i]
                        val current = i == activeIndex // a repeated id is current only once
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .clip(RoundedCornerShape(12.dp))
                                .background(if (current) glass.fill else Color.Transparent)
                                .clickable(enabled = view.canControl && !current, role = Role.Button) {
                                    haptics.performHapticFeedback(HapticFeedbackType.Confirm)
                                    scope.launch {
                                        val r = ui.playQueueItem(view, e.id)
                                        if (r.ok) onClose() else {
                                            haptics.performHapticFeedback(HapticFeedbackType.Reject)
                                            ui.toast(failText(r.error?.code))
                                        }
                                    }
                                }
                                .heightIn(min = 56.dp)
                                .padding(horizontal = 10.dp, vertical = 8.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Box(Modifier.width(28.dp), contentAlignment = Alignment.CenterStart) {
                                if (current) Icon(AppIcons.VolumeUp, stringResource(R.string.queue_now_playing), Modifier.size(18.dp), tint = glass.accent)
                                else Text("${q.offset + i + 1}", style = MaterialTheme.typography.labelMedium.copy(fontFeatureSettings = "tnum"), color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                            Column(Modifier.weight(1f)) {
                                Text(
                                    e.title.ifEmpty { stringResource(R.string.unknown_title) },
                                    style = MaterialTheme.typography.bodyLarge,
                                    color = if (current) glass.accent else MaterialTheme.colorScheme.onSurface,
                                    maxLines = 1, overflow = TextOverflow.Ellipsis,
                                )
                                if (e.subtitle.isNotEmpty()) {
                                    Text(e.subtitle, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun SettingsButton(ui: UiContext) {
    val haptics = LocalHapticFeedback.current
    Box(
        Modifier
            .size(48.dp)
            .clip(RoundedCornerShape(14.dp))
            .clickable(role = Role.Button) {
                haptics.performHapticFeedback(HapticFeedbackType.ContextClick)
                ui.openSettings(null)
            },
        contentAlignment = Alignment.Center,
    ) {
        Icon(AppIcons.Gear, stringResource(R.string.settings_title), Modifier.size(24.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

/**
 * A light tick when Auto moves the player to another source while the app is in front. Changes
 * that happened in the background (seen when the app comes back) are silent.
 */
@Composable
private fun AutoSwitchHaptic(view: PlayerView) {
    val haptics = LocalHapticFeedback.current
    val lifecycle = androidx.lifecycle.compose.LocalLifecycleOwner.current.lifecycle
    var resumedAt by remember { androidx.compose.runtime.mutableLongStateOf(SystemClock.elapsedRealtime()) }
    androidx.lifecycle.compose.LifecycleResumeEffect(Unit) {
        resumedAt = SystemClock.elapsedRealtime()
        onPauseOrDispose { }
    }
    val source = "${view.side}:${view.key}"
    var last by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(source) {
        val fresh = SystemClock.elapsedRealtime() - resumedAt > 600
        val visible = lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.RESUMED)
        if (last != null && last != source && view.automatic && visible && fresh) {
            haptics.performHapticFeedback(HapticFeedbackType.SegmentTick)
        }
        last = source
    }
}

/** Player actions, already wired to optimistic state, haptics and error toasts. */
class PlayerControls(val togglePlay: () -> Unit, val skip: (next: Boolean) -> Unit, val seek: (Long) -> Unit)

/** Manual lyric offset for the current app (+ the automatic output latency, for display). */
class LyricsTiming(val offsetMs: Int, val outputLatencyMs: Int, val set: (Int) -> Unit)

@Composable
fun commandFailureText(): (String?) -> String {
    val unsupported = stringResource(R.string.error_action_unsupported)
    val timeout = stringResource(R.string.error_timeout)
    val notConnected = stringResource(R.string.error_not_connected)
    val generic = stringResource(R.string.error_command_failed)
    return { code ->
        when (code) {
            ErrorCode.ACTION_UNSUPPORTED -> unsupported
            ErrorCode.TIMEOUT -> timeout
            ErrorCode.NO_SESSION, ErrorCode.LINK_LOST, ErrorCode.NOT_FOUND -> notConnected
            else -> generic.format(code ?: ErrorCode.INTERNAL)
        }
    }
}


/**
 * Artwork corner radius and shadow follow its current size (8 dp small … 20 dp large), so the
 * shared-element morph between the modes animates them continuously instead of snapping.
 */
private object ArtworkShape : Shape {
    override fun createOutline(size: Size, layoutDirection: LayoutDirection, density: Density): Outline {
        val minSide = size.minDimension
        val radius = with(density) {
            val t = ((minSide - 56.dp.toPx()) / (300.dp.toPx() - 56.dp.toPx())).coerceIn(0f, 1f)
            (8.dp.toPx() + (20.dp.toPx() - 8.dp.toPx()) * t)
        }
        return Outline.Rounded(androidx.compose.ui.geometry.RoundRect(0f, 0f, size.width, size.height, radius, radius))
    }
}

/** Fixed-size artwork with a placeholder; new artwork cross-fades in (no layout jump). */
@Composable
private fun Artwork(art: Bitmap?, modifier: Modifier) {
    val glass = LocalGlass.current
    Box(
        modifier
            .graphicsLayer {
                shape = ArtworkShape
                clip = true
                // Elevation grows with the size: a soft shadow for the big artwork only.
                shadowElevation = ((size.minDimension - 64.dp.toPx()) / 10f).coerceIn(0f, 24.dp.toPx())
            }
            .background(glass.fill),
        contentAlignment = Alignment.Center,
    ) {
        Crossfade(targetState = art, animationSpec = tween(300), label = "art") { a ->
            if (a != null) {
                Image(a.asImageBitmap(), null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
            } else {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Icon(AppIcons.Music, null, Modifier.fillMaxSize(0.4f), tint = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
    }
}

@Composable
private fun TitleBlock(session: MediaSessionInfo, modifier: Modifier, large: Boolean) {
    // Keyed on the visible text only (not the whole session, whose position changes constantly).
    Crossfade(targetState = session.title to session.artist.ifEmpty { session.album }, animationSpec = tween(260), modifier = modifier, label = "title") { (title, sub) ->
        TitleText(title, sub, large)
    }
}

@Composable
private fun TitleText(title: String, sub: String, large: Boolean) {
    Column(Modifier.fillMaxWidth()) {
        Text(
            title.ifEmpty { stringResource(R.string.unknown_title) },
            style = if (large) TitleLarge else TitleSmall,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        if (sub.isNotEmpty()) {
            Text(
                sub,
                style = if (large) ArtistLarge else ArtistSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}


/**
 * Liquid-glass volume control for the target device: the phone's media volume (AudioManager) or
 * the PC's volume (`volume.set` / `volume.state`). PC sessions that expose their own app volume
 * get a second slider (`volume.set` with `key`). Tapping the speaker toggles mute.
 */
@Composable
private fun VolumeRow(ui: UiContext, view: PlayerView) {
    val vol = rememberVolumeView(ui.engine, view) ?: return
    val scope = rememberCoroutineScope()
    val appVolume = if (view.side == app.mediaduet.media.Side.PC) view.info.volume else null
    if (appVolume == null) {
        VolumeSlider(ui, null, vol.level, vol.muted, vol.enabled, key = view.side) { req -> scope.launch { ui.setVolume(view, req) } }
    } else {
        val pcName = view.deviceName ?: stringResource(R.string.pc_generic)
        VolumeSlider(ui, pcName, vol.level, vol.muted, vol.enabled, key = "device") { req -> scope.launch { ui.setVolume(view, req) } }
        VolumeSlider(
            ui, view.info.appName.ifEmpty { view.info.appId }, appVolume.level.toFloat(), appVolume.muted,
            enabled = view.connected, key = "app:" + view.key,
        ) { req -> scope.launch { ui.setVolume(view, req.copy(key = view.key)) } }
    }
    if (vol.fixed) {
        Text(stringResource(R.string.volume_fixed), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun VolumeSlider(
    ui: UiContext,
    label: String?,
    level: Float,
    muted: Boolean,
    enabled: Boolean,
    key: Any,
    send: (VolumeSet) -> Unit,
) {
    // No jitter: device values are ignored while dragging and for 600 ms after; sends are
    // throttled to ≤ 10/s and the final value is sent on release (see SliderSync).
    var sync by remember(key) { mutableStateOf(SliderSync()) }
    val haptics = LocalHapticFeedback.current
    var tick by remember { androidx.compose.runtime.mutableLongStateOf(0L) }
    LaunchedEffect(sync.holdUntil) {
        val wait = sync.holdUntil - SystemClock.elapsedRealtime()
        if (wait > 0) {
            delay(wait)
            tick = SystemClock.elapsedRealtime()
        }
    }
    val remote = if (muted) 0f else level
    val shown = sync.displayed(remote, maxOf(tick, SystemClock.elapsedRealtime()))
    if (label != null) {
        Text(
            label,
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.fillMaxWidth().padding(start = 48.dp, top = 2.dp),
        )
    }
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Box(
            Modifier
                .size(48.dp)
                .clip(RoundedCornerShape(50))
                .clickable(enabled = enabled, role = Role.Button) {
                    haptics.performHapticFeedback(if (muted) HapticFeedbackType.ToggleOn else HapticFeedbackType.ToggleOff)
                    send(VolumeSet(muted = !muted))
                },
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                if (muted) AppIcons.VolumeOff else AppIcons.VolumeDown,
                stringResource(if (muted) R.string.volume_unmute else R.string.volume_mute),
                Modifier.size(20.dp),
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        MediaSlider(
            value = { shown },
            onValueChange = { f ->
                val (next, value) = sync.onDrag(f, SystemClock.elapsedRealtime())
                sync = next
                if (value != null) send(VolumeSet(level = value.toDouble()))
            },
            modifier = Modifier.weight(1f),
            enabled = enabled,
            contentDescription = label ?: stringResource(R.string.a11y_volume),
            onValueChangeFinished = { f ->
                val (next, value) = sync.onRelease(f, SystemClock.elapsedRealtime())
                sync = next
                if (value != null) send(VolumeSet(level = value.toDouble()))
            },
        )
        Icon(AppIcons.VolumeUp, null, Modifier.padding(start = 14.dp, end = 2.dp).size(20.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}


/** Control-target switcher: Automatic, this phone's sessions and the PC's sessions. */
@Composable
private fun TargetSheet(ui: UiContext) {
    val engine = ui.engine
    val open = ui.nav.sheetOpen
    val remote by engine.remote.state.collectAsStateWithLifecycle()
    val local by engine.localStates.collectAsStateWithLifecycle()
    val choice by engine.settings.target.collectAsStateWithLifecycle()
    val resolved by engine.target.collectAsStateWithLifecycle()
    val link by engine.link.status.collectAsStateWithLifecycle()
    val policy by engine.remote.policy.collectAsStateWithLifecycle()
    val sink by engine.remote.sink.collectAsStateWithLifecycle()
    val mode by engine.settings.mode.collectAsStateWithLifecycle()
    val reduced = rememberReducedMotion()
    val glass = LocalGlass.current
    val haptics = LocalHapticFeedback.current
    val close = { ui.setNav(ui.nav.copy(sheetOpen = false)) }
    AnimatedVisibility(open, enter = fadeIn(if (reduced) snap() else tween(200)), exit = fadeOut(if (reduced) snap() else tween(200))) {
        Box(
            Modifier
                .fillMaxSize()
                .background(Color.Black.copy(alpha = if (glass.isDark) 0.4f else 0.18f))
                .clickable(remember { MutableInteractionSource() }, indication = null, onClick = close),
        )
    }
    AnimatedVisibility(
        open,
        enter = if (reduced) fadeIn(snap()) else slideInVertically(spring(dampingRatio = 0.85f, stiffness = 400f)) { it },
        exit = if (reduced) fadeOut(snap()) else slideOutVertically(tween(220)) { it },
        modifier = Modifier.fillMaxSize(),
    ) {
        Box(
            Modifier.fillMaxSize().padding(start = 16.dp, end = 16.dp, top = 48.dp, bottom = LocalBottomInset.current + 8.dp),
            contentAlignment = Alignment.BottomCenter,
        ) {
            GlassCard(
                ui.backdrop,
                Modifier.widthIn(max = 560.dp).fillMaxWidth().verticalScroll(rememberScrollState()),
                strong = true,
                cornerRadius = 28.dp,
            ) {
                Text(stringResource(R.string.source_title), style = MaterialTheme.typography.titleMedium)
                Spacer(Modifier.height(8.dp))
                // Sound output first: one switch sends this phone's audio to the PC's speakers.
                if (mode.linked && link.current != null && remote?.connected == true) {
                    sink?.let { PcSpeakerRow(ui, it, link.current?.peer?.name ?: stringResource(R.string.pc_generic)) }
                    HandoffRows(ui, close)
                }
                fun pick(c: TargetChoice) {
                    haptics.performHapticFeedback(HapticFeedbackType.Confirm)
                    engine.settings.setTarget(c)
                    close()
                }
                SourceRow(AppIcons.Auto, stringResource(R.string.target_auto), stringResource(R.string.target_auto_text), selected = choice.side == null) {
                    pick(TargetChoice.AUTO)
                }

                SheetDivider()
                SheetHeader(stringResource(R.string.this_phone))
                if (local.isEmpty()) {
                    SheetNote(stringResource(R.string.phone_empty))
                } else {
                    val phoneActive = SnapshotBuilder.pickActive(local)?.key
                    for (s in local) {
                        SourceRow(
                            AppIcons.Phone,
                            s.appName,
                            listOf(s.title, statusLabel(s.status)).filter { it.isNotEmpty() }.joinToString(stringResource(R.string.list_separator)),
                            selected = choice.side == Side.PHONE && resolved?.side == Side.PHONE && resolved?.key == s.key,
                        ) { pick(TargetChoice(Side.PHONE, if (s.key == phoneActive) null else s.key)) }
                    }
                }

                if (mode.linked) {
                    SheetDivider()
                    val connected = link.current != null && remote?.connected == true
                    SheetHeader(link.current?.peer?.name ?: stringResource(R.string.pc_generic))
                    val st = remote
                    when {
                        !connected -> SourceRow(
                            AppIcons.Link, stringResource(R.string.pill_pc_disconnected), stringResource(R.string.sheet_pc_disconnected_text),
                            selected = false, chevron = true,
                        ) {
                            close()
                            ui.openSettings(SettingsRoute.CONNECTION)
                        }
                        st == null || st.snapshot.sessions.isEmpty() -> SheetNote(stringResource(R.string.empty_no_media))
                        else -> for (s in st.snapshot.sessions) {
                            SourceRow(
                                AppIcons.Computer,
                                s.appName.ifEmpty { s.appId },
                                listOf(s.title, statusLabel(s.status)).filter { it.isNotEmpty() }.joinToString(stringResource(R.string.list_separator)),
                                selected = choice.side == Side.PC && resolved?.side == Side.PC && resolved?.key == s.key,
                            ) { pick(TargetChoice(Side.PC, if (s.key == st.snapshot.active) null else s.key)) }
                        }
                    }
                    policy?.takeIf { connected }?.let { pol ->
                        SheetNote(stringResource(R.string.settings_policy_current, policyModeLabel(pol.mode)))
                    }
                }
            }
        }
    }
}

/**
 * "Play on PC": the PC receives this phone's Bluetooth audio (Windows A2DP sink, `audio.sink.set`).
 * A switch with the live state underneath; when the phone is not a paired Bluetooth audio source
 * of the PC the row explains what to do instead of offering a switch that cannot work.
 */
@Composable
private fun PcSpeakerRow(ui: UiContext, sink: SinkState, pcName: String) {
    val scope = rememberCoroutineScope()
    val glass = LocalGlass.current
    val failText = stringResource(R.string.error_not_connected)
    val set: (Boolean) -> Unit = { on ->
        scope.launch { if (!ui.engine.setPcSpeaker(on)) ui.toast(failText) }
    }
    val usable = sink.available || sink.active
    val subtitle = when {
        sink.state == SinkState.CONNECTING -> stringResource(R.string.pc_speaker_connecting)
        sink.state == SinkState.ON -> stringResource(R.string.pc_speaker_on, pcName)
        sink.state == SinkState.ERROR -> pcSpeakerError(sink.error)
        !sink.available -> stringResource(R.string.pc_speaker_unpaired)
        else -> stringResource(R.string.pc_speaker_text, pcName)
    }
    InfoRow(
        stringResource(R.string.pc_speaker_title),
        subtitle = subtitle,
        leading = {
            Icon(AppIcons.Speaker, null, Modifier.size(22.dp), tint = if (sink.state == SinkState.ON) glass.accent else MaterialTheme.colorScheme.onSurfaceVariant)
        },
        trailing = if (usable) {
            { LiquidToggle({ sink.active }, set, ui.backdrop, contentDescription = stringResource(R.string.pc_speaker_title)) }
        } else null,
        onClick = if (usable) ({ set(!sink.active) }) else null,
        role = Role.Switch,
    )
    SheetDivider()
}

/**
 * Handoff (§8.7): continue the song the player shows on the other device, in one of its music
 * apps, from the current position; this side pauses. The PC coordinates and answers when it plays
 * (a toast then says where, or why not). Shown only when the other side has a music app for it.
 */
@Composable
private fun HandoffRows(ui: UiContext, close: () -> Unit) {
    val engine = ui.engine
    val context = LocalContext.current
    val haptics = LocalHapticFeedback.current
    val resolved by engine.target.collectAsStateWithLifecycle()
    val remote by engine.remote.state.collectAsStateWithLifecycle()
    val local by engine.localStates.collectAsStateWithLifecycle()
    val link by engine.link.status.collectAsStateWithLifecycle()
    val pcApps by engine.pcHandoffApps.collectAsStateWithLifecycle()
    val phoneApps by engine.phoneHandoffApps.collectAsStateWithLifecycle()
    val busy by engine.handoffBusy.collectAsStateWithLifecycle()
    val t = resolved ?: return
    if (Caps.HANDOFF !in link.current?.peerCaps.orEmpty()) return
    val (title, appId) = when (t.side) {
        Side.PHONE -> local.firstOrNull { it.key == t.key }?.let { it.title to it.packageName }
        Side.PC -> remote?.snapshot?.sessions?.firstOrNull { it.key == t.key }?.let { it.title to it.appId }
    } ?: return
    if (title.isBlank()) return
    val own = HandoffApp.ofSession(appId)
    // The source's own app first when the other side has it.
    val apps = (if (t.side == Side.PHONE) pcApps else phoneApps).sortedByDescending { it == own }
    if (apps.isEmpty()) return
    val pcName = link.current?.peer?.name ?: stringResource(R.string.pc_generic)
    SheetHeader(stringResource(R.string.handoff_title))
    for (app in apps) {
        SourceRow(
            if (t.side == Side.PHONE) AppIcons.Computer else AppIcons.Phone,
            if (t.side == Side.PHONE) stringResource(R.string.handoff_to_pc, pcName) else stringResource(R.string.handoff_to_phone),
            if (busy) stringResource(R.string.handoff_working) else stringResource(R.string.handoff_in_app, handoffAppLabel(app)),
            selected = false,
            chevron = true,
        ) {
            if (busy) return@SourceRow
            haptics.performHapticFeedback(HapticFeedbackType.Confirm)
            close()
            val side = t.side
            val key = t.key
            // Outlives the sheet (and the player page): the PC answers when the song plays.
            engine.scope.launch {
                val o = engine.startHandoff(side, key, app)
                val text = handoffOutcomeText(context, o, title, app)
                withContext(Dispatchers.Main) { ui.toast(text) }
            }
        }
    }
    SheetDivider()
}

@Composable
private fun handoffAppLabel(app: HandoffApp): String = stringResource(HANDOFF_APP_LABELS.getValue(app))

private val HANDOFF_APP_LABELS = mapOf(
    HandoffApp.NETEASE to R.string.handoff_app_netease,
    HandoffApp.QQMUSIC to R.string.handoff_app_qqmusic,
    HandoffApp.SPOTIFY to R.string.handoff_app_spotify,
)

/** What a toast says about a finished handoff. */
private fun handoffOutcomeText(context: android.content.Context, o: HandoffOutcome, title: String, asked: HandoffApp): String {
    val app = context.getString(HANDOFF_APP_LABELS.getValue(o.app ?: asked))
    val song = o.title.ifEmpty { title }
    if (o.ok) {
        return when {
            o.state != PlayResult.STATE_PLAYING -> context.getString(R.string.handoff_opened, song, app)
            o.to == "phone" -> context.getString(R.string.handoff_done_phone, song)
            else -> context.getString(R.string.handoff_done_pc, song)
        }
    }
    return when (o.error) {
        HandoffErrors.APP_UNAVAILABLE -> context.getString(R.string.handoff_err_app_unavailable, app)
        HandoffErrors.NOT_FOUND -> context.getString(R.string.handoff_err_not_found, app)
        HandoffErrors.NETWORK -> context.getString(R.string.handoff_err_network)
        HandoffErrors.TIMEOUT -> context.getString(R.string.handoff_err_timeout, app)
        HandoffErrors.LINK_LOST -> context.getString(R.string.handoff_err_link_lost)
        HandoffErrors.BUSY -> context.getString(R.string.handoff_err_busy)
        HandoffErrors.NO_SESSION -> context.getString(R.string.handoff_err_no_session)
        HandoffErrors.LAUNCH_FAILED -> context.getString(R.string.handoff_err_launch, app)
        else -> context.getString(R.string.handoff_err_other, o.error ?: "?")
    }
}

/** `audio.sink.state` error codes → text (unknown codes: [R.string.pc_speaker_error_failed]). */
val PC_SPEAKER_ERRORS = mapOf(
    "no_device" to R.string.pc_speaker_unpaired,
    "ambiguous" to R.string.pc_speaker_error_ambiguous,
    "timeout" to R.string.pc_speaker_error_timeout,
    "denied" to R.string.pc_speaker_error_denied,
    "failed" to R.string.pc_speaker_error_failed,
)

@Composable
private fun pcSpeakerError(code: String?): String = stringResource(PC_SPEAKER_ERRORS[code] ?: R.string.pc_speaker_error_failed)

@Composable
private fun SheetDivider() =
    HorizontalDivider(Modifier.padding(vertical = 8.dp), thickness = 0.5.dp, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f))

@Composable
private fun SheetHeader(text: String) =
    Text(text, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(vertical = 4.dp))

@Composable
private fun SheetNote(text: String) =
    Text(text, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(vertical = 6.dp))

@Composable
private fun SourceRow(icon: ImageVector, title: String, subtitle: String, selected: Boolean, chevron: Boolean = false, onClick: () -> Unit) {
    val glass = LocalGlass.current
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .clickable(role = if (chevron) Role.Button else Role.RadioButton, onClick = onClick)
            .heightIn(min = 56.dp)
            .padding(vertical = 8.dp, horizontal = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(icon, null, Modifier.size(22.dp), tint = if (selected) glass.accent else MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge, color = if (selected) glass.accent else MaterialTheme.colorScheme.onSurface, maxLines = 2, overflow = TextOverflow.Ellipsis)
            if (subtitle.isNotEmpty()) {
                Text(subtitle, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
        }
        when {
            selected -> Icon(AppIcons.Check, stringResource(R.string.a11y_selected), Modifier.size(20.dp), tint = glass.accent)
            chevron -> Icon(AppIcons.ChevronRight, null, Modifier.size(20.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

