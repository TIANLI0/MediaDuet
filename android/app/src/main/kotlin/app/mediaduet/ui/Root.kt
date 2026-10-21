package app.mediaduet.ui

import android.graphics.Bitmap
import androidx.activity.compose.PredictiveBackHandler
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.Crossfade
import androidx.compose.animation.core.snap
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.slideOutVertically
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.Saver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.LifecycleResumeEffect
import app.mediaduet.R
import app.mediaduet.service.Engine
import app.mediaduet.service.PermissionChecks
import app.mediaduet.service.PermissionSnapshot
import app.mediaduet.ui.components.AppIcons
import app.mediaduet.ui.components.FlowingArtwork
import app.mediaduet.ui.screens.AboutPage
import app.mediaduet.ui.screens.ArbitrationPage
import app.mediaduet.ui.screens.AudioPage
import app.mediaduet.ui.screens.ConnectionPage
import app.mediaduet.ui.screens.DiagnosticsPage
import app.mediaduet.ui.screens.LanguagePage
import app.mediaduet.ui.screens.LyricsDisplayPage
import app.mediaduet.ui.screens.MiniPlayer
import app.mediaduet.ui.screens.PairingOverlay
import app.mediaduet.ui.screens.PermissionsPage
import app.mediaduet.ui.screens.PlayerScreen
import app.mediaduet.ui.screens.SettingsRoot
import app.mediaduet.ui.screens.rememberPlayerView
import app.mediaduet.ui.screens.rememberReducedMotion
import app.mediaduet.ui.theme.LocalGlass
import com.kyant.backdrop.Backdrop
import com.kyant.backdrop.backdrops.layerBackdrop
import com.kyant.backdrop.backdrops.rememberLayerBackdrop
import app.mediaduet.service.WorkMode
import app.mediaduet.ui.screens.ModeWelcome
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay

/** Shared UI state handed to screens. */
class UiContext(
    val engine: Engine,
    /** Background only (artwork / solid colour): what in-page glass refracts. */
    val backdrop: Backdrop,
    /** Background + page content: what floating overlays refract. */
    val overlayBackdrop: Backdrop,
    val permissions: PermissionSnapshot,
    val refreshPermissions: () -> Unit,
    val nav: NavState,
    val setNav: (NavState) -> Unit,
    val toast: (String) -> Unit,
    val confirm: (ConfirmRequest) -> Unit,
) {
    fun openSettings(route: SettingsRoute?) = setNav(nav.open(route))
    fun openPlayer() = setNav(nav.showPlayer())
}

private val NavSaver = Saver<MutableState<NavState>, String>(
    save = { it.value.encode() },
    restore = { mutableStateOf(NavState.decode(it)) },
)

/** Deep-link key (notification / intent extra) for Now Playing; other keys are settings routes. */
const val NAV_KEY_NOW = "now"

/**
 * App shell without tabs: Now Playing is the home screen (full height); Settings is a page pushed
 * on top of it (from the player's action row) with its own sub-pages. The floating glass mini player
 * appears only on the Settings stack. One predictive-back handler drives the whole stack.
 */
@Composable
fun MediaDuetRoot(engine: Engine, requestedPage: MutableState<String?>) {
    val context = LocalContext.current
    val reduced = rememberReducedMotion()
    val density = LocalDensity.current
    val navState = rememberSaveable(saver = NavSaver) { mutableStateOf(NavState()) }
    var nav by navState
    var toast by remember { mutableStateOf<Toast?>(null) }
    var confirm by remember { mutableStateOf<ConfirmRequest?>(null) }
    var miniHeight by remember { mutableStateOf(0.dp) }

    LaunchedEffect(requestedPage.value) {
        when (val key = requestedPage.value) {
            null -> Unit
            NAV_KEY_NOW -> nav = nav.showPlayer()
            else -> nav = nav.open(SettingsRoute.entries.firstOrNull { it.key == key })
        }
        requestedPage.value = null
    }
    var permissions by remember { mutableStateOf(PermissionChecks.snapshot(context)) }
    val refresh = {
        permissions = PermissionChecks.snapshot(context)
        engine.refreshPermissions()
    }
    LifecycleResumeEffect(Unit) {
        // Returning from system settings refreshes every permission state.
        refresh()
        onPauseOrDispose { }
    }

    // Installs from before the work-mode choice that already have a PC paired keep using it.
    val peers by engine.trust.peers.collectAsStateWithLifecycle()
    val mode by engine.settings.mode.collectAsStateWithLifecycle()
    LaunchedEffect(mode, peers.isNotEmpty()) {
        if (mode == WorkMode.UNSET && peers.isNotEmpty()) engine.settings.setMode(WorkMode.LINKED)
    }

    // The background follows the player's control target (phone or PC artwork).
    val playerView = rememberPlayerView(engine)
    val background = playerView?.art

    val glass = LocalGlass.current
    val pageBackdrop = rememberLayerBackdrop()
    val overlayBackdrop = rememberLayerBackdrop()
    val ui = UiContext(
        engine = engine,
        backdrop = pageBackdrop,
        overlayBackdrop = overlayBackdrop,
        permissions = permissions,
        refreshPermissions = refresh,
        nav = nav,
        setNav = { nav = it },
        toast = { text -> if (toast?.text != text) toast = Toast(System.nanoTime(), text) },
        confirm = { confirm = it },
    )

    // One back handler for the whole back stack (see NavState.back). Settings pages follow the
    // predictive-back gesture before they pop. At the root the system takes over (the activity
    // moves to the back, see MainActivity).
    var backProgress by remember { mutableFloatStateOf(0f) }
    // The page the gesture dragged. On commit it keeps its dragged offset while it slides out
    // (resetting first made it jump back to full size, then animate: a visible stutter).
    var backRoute by remember { mutableStateOf<Any?>(Unit) }   // Unit = none; null = Settings root
    var backCommit by remember { mutableIntStateOf(0) }
    PredictiveBackHandler(enabled = confirm != null || nav.back() != null) { events ->
        var committed = false
        try {
            backRoute = nav.route
            events.collect { if (confirm == null && nav.backPopsSettingsPage) backProgress = it.progress }
            committed = true
            if (confirm != null) confirm = null else nav.back()?.let { nav = it }
        } catch (e: CancellationException) {
            throw e
        } finally {
            if (committed && backProgress > 0f) backCommit++ else backProgress = 0f
        }
    }
    LaunchedEffect(backCommit) {
        if (backCommit == 0) return@LaunchedEffect
        delay(320)   // the pop transition
        backProgress = 0f
    }

    // Measured floating inset: the mini player (Settings stack only) plus the navigation bar.
    val navBar = with(density) { WindowInsets.navigationBars.getBottom(this).toDp() }
    val tallEnough = with(density) { androidx.compose.ui.platform.LocalWindowInfo.current.containerSize.height.toDp() } >= 480.dp
    val showMini = nav.settingsOpen && playerView != null && tallEnough
    val bottomInset = navBar + if (showMini) miniHeight else 8.dp

    CompositionLocalProvider(LocalBottomInset provides bottomInset) {
        Box(Modifier.fillMaxSize().background(glass.background)) {
            // Everything inside this box is what the mini player, sheets and dialogs refract.
            Box(Modifier.fillMaxSize().layerBackdrop(overlayBackdrop)) {
                // Flows only behind Now Playing while something plays: Settings and dialogs keep a
                // still background (the glass there would otherwise re-render every frame).
                val flowing = !reduced && nav.section == Section.NOW && playerView?.playing == true
                AmbientBackground(background, flowing, Modifier.fillMaxSize().layerBackdrop(pageBackdrop))
                AnimatedContent(
                    targetState = nav.section,
                    transitionSpec = {
                        when {
                            reduced -> fadeIn(snap()) togetherWith fadeOut(snap())
                            // Push Settings: it slides in from the end, the player recedes a little.
                            targetState == Section.SETTINGS ->
                                slideInHorizontally(tween(320)) { it } togetherWith
                                    (slideOutHorizontally(tween(320)) { -it / 4 } + fadeOut(tween(320)))
                            // Pop back to Now Playing.
                            else ->
                                (slideInHorizontally(tween(300)) { -it / 4 } + fadeIn(tween(300))) togetherWith
                                    slideOutHorizontally(tween(300)) { it }
                        }
                    },
                    label = "section",
                ) { section ->
                    when (section) {
                        Section.NOW -> PlayerScreen(ui)
                        Section.SETTINGS -> AnimatedContent(
                            targetState = nav.route,
                            transitionSpec = {
                                when {
                                    reduced -> fadeIn(snap()) togetherWith fadeOut(snap())
                                    targetState != null ->
                                        slideInHorizontally(tween(300)) { it } togetherWith (slideOutHorizontally(tween(300)) { -it / 4 } + fadeOut(tween(300)))
                                    else ->
                                        (slideInHorizontally(tween(300)) { -it / 4 } + fadeIn(tween(300))) togetherWith slideOutHorizontally(tween(300)) { it }
                                }
                            },
                            label = "settings",
                        ) { r ->
                            val back = { nav = nav.copy(route = null) }
                            Box(
                                Modifier.graphicsLayer {
                                    // The page on top follows the predictive-back gesture.
                                    if (backProgress > 0f && r == backRoute) {
                                        translationX = size.width * 0.3f * backProgress
                                        val s = 1f - 0.06f * backProgress
                                        scaleX = s
                                        scaleY = s
                                    }
                                },
                            ) {
                                when (r) {
                                    null -> SettingsRoot(ui) { nav = nav.showPlayer() }
                                    SettingsRoute.CONNECTION -> ConnectionPage(ui, back)
                                    SettingsRoute.PERMISSIONS -> PermissionsPage(ui, back)
                                    SettingsRoute.ARBITRATION -> ArbitrationPage(ui, back)
                                    SettingsRoute.LYRICS -> LyricsDisplayPage(ui, back)
                                    SettingsRoute.AUDIO -> AudioPage(ui, back)
                                    SettingsRoute.LANGUAGE -> LanguagePage(ui, back)
                                    SettingsRoute.DIAGNOSTICS -> DiagnosticsPage(ui, back)
                                    SettingsRoute.ABOUT -> AboutPage(ui, back)
                                }
                            }
                        }
                    }
                }
            }

            // Floating glass mini player, docked above the gesture inset (Settings stack only).
            AnimatedVisibility(
                visible = showMini,
                enter = if (reduced) fadeIn(snap()) else slideInVertically { it } + fadeIn(),
                exit = if (reduced) fadeOut(snap()) else slideOutVertically { it } + fadeOut(),
                modifier = Modifier.align(Alignment.BottomCenter),
            ) {
                Box(
                    Modifier
                        .onSizeChanged { miniHeight = with(density) { it.height.toDp() } }
                        .padding(horizontal = 16.dp)
                        .padding(top = 8.dp, bottom = navBar + 12.dp)
                        .widthIn(max = 560.dp)
                        .fillMaxWidth(),
                ) {
                    MiniPlayer(ui, overlayBackdrop)
                }
            }
            ToastHost(toast, bottomInset, overlayBackdrop, reduced) { toast = null }
            PairingOverlay(engine, overlayBackdrop) { nav = nav.open(SettingsRoute.CONNECTION) }
            ModeWelcome(ui, ui.backdrop, reduced)
            ConfirmHost(confirm, overlayBackdrop, reduced) { confirm = null }
        }
    }
}

/**
 * The backdrop layer: the current artwork as a slowly flowing, heavily blurred colour field
 * ([FlowingArtwork]) under a flat scrim, or a solid neutral colour. No gradients.
 */
@Composable
private fun AmbientBackground(artwork: Bitmap?, flowing: Boolean, modifier: Modifier) {
    val glass = LocalGlass.current
    Crossfade(targetState = artwork, animationSpec = tween(900), modifier = modifier, label = "bg") { art ->
        if (art == null) {
            Box(Modifier.fillMaxSize().background(glass.background))
        } else {
            Box(Modifier.fillMaxSize().background(glass.background)) {
                FlowingArtwork(art, animate = flowing, modifier = Modifier.fillMaxSize())
                Box(Modifier.fillMaxSize().background(glass.scrim))
            }
        }
    }
}
