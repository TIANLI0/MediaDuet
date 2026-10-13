package app.mediaduet.ui.screens

import android.graphics.Bitmap
import android.os.SystemClock
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.AnimatedVisibilityScope
import androidx.compose.animation.Crossfade
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
import kotlin.math.abs
import kotlin.math.roundToInt

/** Player title / artist (Apple Music scale: large semibold title, regular artist). */
private val TitleLarge = TextStyle(fontSize = 24.sp, lineHeight = 30.sp, fontWeight = FontWeight.SemiBold, letterSpacing = (-0.2).sp)
private val ArtistLarge = TextStyle(fontSize = 19.sp, lineHeight = 25.sp)
private val TitleSmall = TextStyle(fontSize = 17.sp, lineHeight = 22.sp, fontWeight = FontWeight.SemiBold)
private val ArtistSmall = TextStyle(fontSize = 15.sp, lineHeight = 20.sp)

/** Lyrics mode stays on across a track change while the next lyrics are loading, up to this long. */
private const val LYRICS_WAIT_MS = 4_000L

/**
 * Apple-Music-style full-screen player for the chosen control target (this phone or the PC); the
 * app's home screen, laid out like Apple Music: artwork, title / artist, progress, transport,
 * volume and an action row (Lyrics · playback source · Settings). Lyrics mode morphs the artwork
 * into a compact header (shared elements) with synced lyrics in the middle; progress, transport
 * and the action row stay in place, the volume folds away to give the lyrics room. Swipe the
 * artwork for next / previous.
 */
@OptIn(ExperimentalSharedTransitionApi::class)
@Composable
fun PlayerScreen(ui: UiContext) {
    val engine = ui.engine
    val lyricsMap by engine.remote.lyrics.collectAsStateWithLifecycle()
    val translations by engine.settings.showTranslations.collectAsStateWithLifecycle()
    val autoLatency by engine.settings.lyricsAutoLatency.collectAsStateWithLifecycle()
    val offsets by engine.settings.lyricOffsets.collectAsStateWithLifecycle()
    val reduced = rememberReducedMotion()
    val density = LocalDensity.current
    val haptics = LocalHapticFeedback.current
    val scope = rememberCoroutineScope()
    val view = rememberPlayerView(engine)
    val bottomInset = LocalBottomInset.current
    val currentUi by rememberUpdatedState(ui)

    BoxWithConstraints(Modifier.fillMaxSize()) {
        val landscape = maxWidth > maxHeight * 1.15f
        val side = if (maxWidth >= 600.dp && !landscape) 40.dp else 24.dp
        Column(
            Modifier
                .fillMaxSize()
                .statusBarsPadding()
                .padding(start = side, end = side, bottom = bottomInset + 4.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Spacer(Modifier.height(12.dp))
            StatusPill(ui, view?.fallback == true)
            if (view == null) {
                Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
                    EmptyState(AppIcons.Music, stringResource(R.string.empty_nothing_playing_phone))
                }
                ActionRow(ui, null, lyricsAvailable = false)
                return@Column
            }
            val info = view.info
            AutoSwitchHaptic(view)
            val lyrics = lyricsMap[info.trackId]?.takeIf { it.available }
            val lyricsMode = ui.nav.lyricsMode && lyrics != null
            // Lyrics mode follows a track change; it ends only if the new track has no lyrics.
            LaunchedEffect(info.trackId, lyrics == null) {
                if (lyrics == null && currentUi.nav.lyricsMode) {
                    delay(LYRICS_WAIT_MS)
                    currentUi.setNav(currentUi.nav.copy(lyricsMode = false))
                }
            }

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
            val canSeek = view.canControl && Action.SEEK in info.actions && (info.durationMs ?: 0) > 0
            val timing = LyricsTiming(
                offsetMs = offsetMs,
                outputLatencyMs = if (autoLatency) view.outputLatencyMs else 0,
                set = { ms -> engine.settings.setLyricOffset(info.appId, ms) },
            )

            @Composable
            fun Top(modifier: Modifier) {
                SharedTransitionLayout(modifier) {
                    // The lyrics travel with the target state: the outgoing lyrics view keeps the
                    // lyrics it was showing while it fades out (a track change clears them first).
                    AnimatedContent(
                        targetState = if (lyricsMode) lyrics else null,
                        // Player <-> lyrics, and lyrics of one track <-> the next (crossfade).
                        contentKey = { it?.trackId },
                        transitionSpec = {
                            if (reduced) fadeIn(snap()) togetherWith fadeOut(snap())
                            else fadeIn(tween(320, delayMillis = 60)) togetherWith fadeOut(tween(180))
                        },
                        label = "playerMode",
                    ) { shownLyrics ->
                        if (shownLyrics == null) {
                            ArtworkMode(this@SharedTransitionLayout, this, view, playing, reduced, controls) {
                                if (lyrics != null) ui.setNav(ui.nav.copy(lyricsMode = true))
                            }
                        } else {
                            LyricsMode(
                                this@SharedTransitionLayout, this, ui, info, view.art, shownLyrics, lyricsPosition, translations, reduced, timing,
                                onHeaderClick = { ui.setNav(ui.nav.copy(lyricsMode = false)) },
                                onSeek = if (canSeek) controls.seek else null,
                            )
                        }
                    }
                }
            }

            // Apple Music order: progress, transport, volume, then the action row (Lyrics · Source ·
            // Settings), which stays put in both modes.
            @Composable
            fun ColumnScope.Bottom(compactVolume: Boolean) {
                Controls(ui, info, position, playing, view.canControl, controls, compact = landscape)
                if (landscape) {
                    Spacer(Modifier.height(8.dp))
                    VolumeRow(ui, view)
                    Spacer(Modifier.height(8.dp))
                    ActionRow(ui, view, lyricsAvailable = lyrics != null)
                    return
                }
                // The volume folds away in lyrics mode (portrait) - animated, so nothing jumps.
                AnimatedVisibility(
                    visible = !compactVolume,
                    enter = if (reduced) fadeIn(snap()) else expandVertically(spring(dampingRatio = 1f, stiffness = 1_400f)) + fadeIn(tween(200, 60)),
                    exit = if (reduced) fadeOut(snap()) else shrinkVertically(spring(dampingRatio = 0.9f, stiffness = 380f)) + fadeOut(tween(120)),
                ) {
                    Column {
                        Spacer(Modifier.height(18.dp))
                        VolumeRow(ui, view)
                    }
                }
                Spacer(Modifier.height(16.dp))
                ActionRow(ui, view, lyricsAvailable = lyrics != null)
            }

            if (landscape) {
                Row(Modifier.weight(1f).fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(40.dp)) {
                    Top(Modifier.weight(1f).fillMaxHeight())
                    Column(Modifier.weight(1f).fillMaxHeight().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.Center) {
                        Bottom(compactVolume = false)
                    }
                }
            } else {
                Top(Modifier.weight(1f).fillMaxWidth())
                Spacer(Modifier.height(24.dp))
                Bottom(compactVolume = lyricsMode)
            }
        }
        TargetSheet(ui)
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
 * One small, non-blocking status pill for the most important issue (missing access, no paired
 * PC, PC not connected); tapping it opens the relevant settings page.
 */
@Composable
private fun StatusPill(ui: UiContext, fallback: Boolean) {
    val peers by ui.engine.trust.peers.collectAsStateWithLifecycle()
    val link by ui.engine.link.status.collectAsStateWithLifecycle()
    val (text, route) = when {
        !ui.permissions.listenerAccess -> stringResource(R.string.banner_access) to SettingsRoute.PERMISSIONS
        peers.isEmpty() -> stringResource(R.string.banner_pair) to SettingsRoute.CONNECTION
        link.current == null || fallback -> stringResource(R.string.pill_pc_disconnected) to SettingsRoute.CONNECTION
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
private fun Modifier.shared(
    scope: SharedTransitionScope,
    visibility: AnimatedVisibilityScope,
    key: String,
    reduced: Boolean,
): Modifier = with(scope) {
    this@shared.sharedElement(
        rememberSharedContentState(key),
        visibility,
        boundsTransform = { _, _ -> if (reduced) snap() else spring(dampingRatio = 0.86f, stiffness = 260f) },
    )
}

/** Text that changes size between the modes: both versions scale into the animated bounds. */
@OptIn(ExperimentalSharedTransitionApi::class)
@Composable
private fun Modifier.sharedText(
    scope: SharedTransitionScope,
    visibility: AnimatedVisibilityScope,
    key: String,
    reduced: Boolean,
): Modifier = with(scope) {
    this@sharedText.sharedBounds(
        rememberSharedContentState(key),
        visibility,
        enter = fadeIn(if (reduced) snap() else tween(220)),
        exit = fadeOut(if (reduced) snap() else tween(160)),
        boundsTransform = { _, _ -> if (reduced) snap() else spring(dampingRatio = 0.86f, stiffness = 260f) },
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

@OptIn(ExperimentalSharedTransitionApi::class)
@Composable
private fun ArtworkMode(
    shared: SharedTransitionScope,
    visibility: AnimatedVisibilityScope,
    view: PlayerView,
    playing: Boolean,
    reduced: Boolean,
    controls: PlayerControls,
    onArtworkClick: () -> Unit,
) {
    val glass = LocalGlass.current
    val haptics = LocalHapticFeedback.current
    val density = LocalDensity.current
    val scope = rememberCoroutineScope()
    // Horizontal artwork offset: set synchronously while dragging; only the release animates.
    var swipe by remember { mutableFloatStateOf(0f) }
    var swipeJob by remember { mutableStateOf<kotlinx.coroutines.Job?>(null) }
    val info = view.info
    BoxWithConstraints(Modifier.fillMaxSize()) {
        // Title block (two text lines at the current font scale) + its spacing below the artwork.
        val titleBlock = with(density) { TitleLarge.lineHeight.toDp() + ArtistLarge.lineHeight.toDp() } + 24.dp
        val side = minOf(maxWidth, maxHeight - titleBlock).coerceAtLeast(96.dp)
        val widthPx = with(density) { maxWidth.toPx() }
        Column(Modifier.fillMaxSize()) {
            val artScale by animateFloatAsState(
                if (playing || reduced) 1f else 0.86f,
                if (reduced) snap() else spring(dampingRatio = 0.72f, stiffness = 220f),
                label = "artScale",
            )
            Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
                Artwork(
                    view.art,
                    Modifier
                        .shared(shared, visibility, "artwork", reduced)
                        .size(side)
                        .graphicsLayer {
                            scaleX = artScale
                            scaleY = artScale
                            translationX = swipe
                            alpha = 1f - (abs(swipe) / widthPx).coerceIn(0f, 0.6f)
                        }
                        // Horizontal swipe: next (left) / previous (right), when supported.
                        .pointerInput(view.key, info.actions, view.canControl) {
                            val threshold = 72.dp.toPx()
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
                                    if (past && allowed) controls.skip(next)
                                    if (past && !allowed) haptics.performHapticFeedback(HapticFeedbackType.Reject)
                                    settle {
                                        if (past && allowed && !reduced) {
                                            // Slide out, re-enter from the other side.
                                            animateSwipe(if (next) -widthPx else widthPx, tween(160))
                                            swipe = if (next) widthPx * 0.4f else -widthPx * 0.4f
                                        }
                                        animateSwipe(0f, if (reduced) snap() else spring(dampingRatio = 0.75f, stiffness = 300f))
                                    }
                                },
                                onDragCancel = { settle { animateSwipe(0f, spring()) } },
                            )
                        }
                        .clickable(remember { MutableInteractionSource() }, indication = null, onClick = onArtworkClick),
                )
            }
            Spacer(Modifier.height(24.dp))
            // Title / artist, left-aligned (ellipsized).
            Row(verticalAlignment = Alignment.CenterVertically) {
                TitleBlock(info, Modifier.weight(1f).sharedText(shared, visibility, "title", reduced), large = true)
                if (!view.connected) {
                    Spacer(Modifier.width(8.dp))
                    StatusChip(stringResource(R.string.stale), dot = glass.warn)
                }
            }
        }
    }
}

@OptIn(ExperimentalSharedTransitionApi::class)
@Composable
private fun LyricsMode(
    shared: SharedTransitionScope,
    visibility: AnimatedVisibilityScope,
    ui: UiContext,
    session: MediaSessionInfo,
    art: Bitmap?,
    lyrics: LyricsState,
    position: State<Long>,
    translations: Boolean,
    reduced: Boolean,
    timing: LyricsTiming,
    onHeaderClick: () -> Unit,
    onSeek: ((Long) -> Unit)?,
) {
    var timingOpen by remember { mutableStateOf(false) }
    var timingTouch by remember { mutableIntStateOf(0) }
    LaunchedEffect(timingOpen, timingTouch) {
        if (timingOpen) {
            delay(5_000)
            timingOpen = false
        }
    }
    Column(Modifier.fillMaxSize()) {
        Row(
            Modifier
                .fillMaxWidth()
                .padding(top = 4.dp, bottom = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Row(
                Modifier
                    .weight(1f)
                    .clickable(remember { MutableInteractionSource() }, indication = null, onClick = onHeaderClick),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Artwork(art, Modifier.shared(shared, visibility, "artwork", reduced).size(56.dp))
                Spacer(Modifier.width(12.dp))
                TitleBlock(session, Modifier.weight(1f).sharedText(shared, visibility, "title", reduced), large = false)
            }
            // Translation toggle: only when these lyrics carry translations (persisted setting).
            if (lyrics.lyrics?.hasTranslations == true) {
                Spacer(Modifier.width(6.dp))
                GlassIconButton(
                    AppIcons.Translate,
                    stringResource(if (translations) R.string.lyrics_hide_translation else R.string.lyrics_show_translation),
                    { ui.engine.settings.setShowTranslations(!translations) },
                    ui.backdrop, size = 48.dp, iconSize = 22.dp, active = translations,
                )
            }
            if (lyrics.synced) {
                Spacer(Modifier.width(6.dp))
                GlassIconButton(
                    AppIcons.Timer,
                    stringResource(R.string.lyrics_timing),
                    { timingOpen = !timingOpen },
                    ui.backdrop, size = 48.dp, iconSize = 22.dp, active = timingOpen || timing.offsetMs != 0,
                )
            }
        }
        Box(Modifier.weight(1f).fillMaxWidth()) {
            val synced = lyrics.lyrics
            if (synced != null && !synced.isEmpty) {
                SyncedLyrics(synced, position, onSeek, ui.backdrop, Modifier.fillMaxSize(), showTranslations = translations)
            } else {
                PlainLyrics(lyrics.plain, Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 16.dp))
            }
            androidx.compose.animation.AnimatedVisibility(
                visible = timingOpen,
                enter = if (reduced) fadeIn(snap()) else fadeIn(tween(160)) + scaleIn(initialScale = 0.92f, transformOrigin = androidx.compose.ui.graphics.TransformOrigin(1f, 0f)),
                exit = if (reduced) fadeOut(snap()) else fadeOut(tween(140)) + scaleOut(targetScale = 0.92f, transformOrigin = androidx.compose.ui.graphics.TransformOrigin(1f, 0f)),
                modifier = Modifier.align(Alignment.TopEnd).padding(top = 4.dp),
            ) {
                LyricsOffsetControl(timing, ui.backdrop) { timingTouch++ }
            }
        }
    }
}

/**
 * Small glass control for the per-app lyric offset: −0.1 s / value / +0.1 s / reset, with
 * haptics. Positive = lyrics later. The automatic output delay (Bluetooth) is shown below.
 */
@Composable
private fun LyricsOffsetControl(timing: LyricsTiming, backdrop: Backdrop, onTouch: () -> Unit) {
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
    Column(
        Modifier
            .drawBackdrop(
                backdrop = backdrop,
                shape = { Capsule() },
                effects = {
                    vibrancy()
                    blur(16.dp.toPx())
                    lens(10.dp.toPx(), 20.dp.toPx())
                },
                onDrawSurface = { drawRect(glass.surfaceStrong) },
            )
            .padding(horizontal = 6.dp, vertical = 6.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            OffsetButton(AppIcons.Minus, stringResource(R.string.lyrics_offset_earlier)) { step(-1) }
            Column(Modifier.widthIn(min = 76.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                Text(
                    stringResource(R.string.lyrics_offset_value, LyricsDelay.signedSeconds(timing.offsetMs)),
                    style = MaterialTheme.typography.titleMedium,
                    color = if (timing.offsetMs != 0) glass.accent else MaterialTheme.colorScheme.onSurface,
                )
                if (timing.outputLatencyMs > 0) {
                    Text(
                        stringResource(R.string.lyrics_offset_auto, timing.outputLatencyMs),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            OffsetButton(AppIcons.Plus, stringResource(R.string.lyrics_offset_later)) { step(1) }
            OffsetButton(AppIcons.Restore, stringResource(R.string.lyrics_offset_reset), enabled = timing.offsetMs != 0) {
                onTouch()
                haptics.performHapticFeedback(HapticFeedbackType.Confirm)
                timing.set(0)
            }
        }
    }
}

@Composable
private fun OffsetButton(icon: ImageVector, description: String, enabled: Boolean = true, onClick: () -> Unit) {
    Box(
        Modifier
            .size(48.dp)
            .clip(RoundedCornerShape(50))
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            icon, description, Modifier.size(22.dp),
            tint = MaterialTheme.colorScheme.onSurface.copy(alpha = if (enabled) 1f else 0.35f),
        )
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

@Composable
private fun Controls(
    ui: UiContext,
    session: MediaSessionInfo,
    position: State<Long>,
    playing: Boolean,
    canControl: Boolean,
    controls: PlayerControls,
    compact: Boolean = false,
) {
    // Seek: the dragged value is shown live and committed only on release; the optimistic
    // position then holds until the device confirms (no rubber-banding).
    var dragFraction by remember { mutableStateOf<Float?>(null) }
    val duration = session.durationMs
    val canSeek = canControl && Action.SEEK in session.actions && duration != null && duration > 0
    // Quantised (≈ 1/2000 of the track) so the slider is not re-targeted every frame.
    val live by remember(duration) {
        derivedStateOf {
            if (duration != null && duration > 0) ((position.value.toFloat() / duration).coerceIn(0f, 1f) * 2000f).roundToInt() / 2000f else 0f
        }
    }
    var scrubbing by remember { mutableStateOf(false) }
    MediaSlider(
        value = { dragFraction ?: live },
        onValueChange = { dragFraction = it },
        enabled = canSeek,
        contentDescription = stringResource(R.string.a11y_playback_position),
        onActiveChange = { scrubbing = it },
        onValueChangeFinished = { f ->
            if (duration != null) controls.seek((f * duration).toLong())
            dragFraction = null
        },
    )
    // Time labels change once per second, not every frame; they light up while scrubbing.
    val seconds by remember { derivedStateOf { position.value / 1000 } }
    val labelColor by androidx.compose.animation.animateColorAsState(
        if (scrubbing) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurfaceVariant,
        label = "timeLabels",
    )
    Row(Modifier.fillMaxWidth()) {
        val shown = dragFraction?.let { f -> duration?.let { (f * it).toLong() } } ?: (seconds * 1000)
        Text(formatTime(shown), style = MaterialTheme.typography.labelMedium, color = labelColor)
        Spacer(Modifier.weight(1f))
        Text(
            if (duration != null) stringResource(R.string.time_remaining, formatTime(((duration - shown) / 1000 * 1000).coerceAtLeast(0))) else formatTime(null),
            style = MaterialTheme.typography.labelMedium,
            color = labelColor,
        )
    }
    Spacer(Modifier.height(if (compact) 8.dp else 18.dp))
    val small = if (compact) 52.dp else 64.dp
    val big = if (compact) 64.dp else 80.dp
    Row(
        Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(if (compact) 24.dp else 32.dp, Alignment.CenterHorizontally),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        GlassIconButton(
            AppIcons.Previous, stringResource(R.string.action_previous), { controls.skip(false) }, ui.backdrop,
            size = small, iconSize = small * 0.47f,
            enabled = canControl && Action.PREVIOUS in session.actions,
        )
        GlassIconButton(
            if (playing) AppIcons.Pause else AppIcons.Play,
            stringResource(if (playing) R.string.action_pause else R.string.action_play),
            controls.togglePlay, ui.backdrop,
            size = big, iconSize = big * 0.5f,
            enabled = canControl && (Action.TOGGLE in session.actions || Action.PLAY in session.actions || Action.PAUSE in session.actions),
        )
        GlassIconButton(
            AppIcons.Next, stringResource(R.string.action_next), { controls.skip(true) }, ui.backdrop,
            size = small, iconSize = small * 0.47f,
            enabled = canControl && Action.NEXT in session.actions,
        )
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

/**
 * Apple-Music-style action row at the bottom of the player: Lyrics (left), playback source with a
 * caption (centre, opens the source sheet) and Settings (right). Plain icons tinted like secondary
 * text with 48 dp targets; Lyrics turns accent with a soft fill when lyrics mode is on and is shown
 * dimmed (not hidden) when the track has no lyrics, so the row stays balanced.
 */
@Composable
private fun ActionRow(ui: UiContext, view: PlayerView?, lyricsAvailable: Boolean) {
    val haptics = LocalHapticFeedback.current
    val glass = LocalGlass.current
    val secondary = MaterialTheme.colorScheme.onSurfaceVariant
    val active = ui.nav.lyricsMode && lyricsAvailable
    Row(Modifier.fillMaxWidth().heightIn(min = 56.dp), verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.weight(1f), contentAlignment = Alignment.CenterStart) {
            val unavailable = stringResource(R.string.lyrics_unavailable)
            Box(
                Modifier
                    .size(48.dp)
                    .clip(RoundedCornerShape(14.dp))
                    .background(if (active) glass.fill else Color.Transparent)
                    .clickable(
                        role = Role.Button,
                        onClickLabel = if (lyricsAvailable) null else unavailable,
                    ) {
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
        // Centre: where the playback is (icon + caption, like Apple Music's AirPlay row).
        val caption = when {
            view == null -> stringResource(R.string.source_title)
            view.automatic -> stringResource(R.string.target_auto_caption, deviceLabel(view))
            else -> deviceLabel(view)
        }
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
                null, Modifier.size(22.dp), tint = secondary,
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
                Icon(AppIcons.Gear, stringResource(R.string.settings_title), Modifier.size(24.dp), tint = secondary)
            }
        }
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

/** Current synced lyric line for compact places (mini player); [positionMs] is already delayed. */
fun currentLyricText(lyrics: LyricsState?, positionMs: Long): String? {
    val synced = lyrics?.lyrics ?: return null
    val i = synced.indexAt(positionMs)
    return synced.lines.getOrNull(i)?.text?.ifBlank { null }
}
