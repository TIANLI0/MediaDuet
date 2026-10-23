package app.mediaduet.ui.screens

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalHapticFeedback
import kotlin.math.abs
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import app.mediaduet.remote.LyricRows
import app.mediaduet.remote.LyricsDelay
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.mediaduet.R
import app.mediaduet.protocol.Action
import app.mediaduet.ui.UiContext
import app.mediaduet.ui.components.AppIcons
import app.mediaduet.ui.theme.LocalGlass
import com.kyant.backdrop.Backdrop
import com.kyant.backdrop.drawBackdrop
import com.kyant.backdrop.effects.blur
import com.kyant.backdrop.effects.lens
import com.kyant.backdrop.effects.vibrancy
import com.kyant.shapes.RoundedRectangle
import kotlinx.coroutines.launch

/** Collapsed player above the tab bar; tap (or swipe up) expands to the full player. */
@Composable
fun MiniPlayer(ui: UiContext, backdrop: Backdrop, modifier: Modifier = Modifier) {
    val engine = ui.engine
    val glass = LocalGlass.current
    val scope = rememberCoroutineScope()
    val lyricsMap by engine.lyrics.entries.collectAsStateWithLifecycle()
    val autoLatency by engine.settings.lyricsAutoLatency.collectAsStateWithLifecycle()
    val offsets by engine.settings.lyricOffsets.collectAsStateWithLifecycle()
    val view = rememberPlayerView(engine) ?: return
    val session = view.info
    val lyrics = engine.lyrics.resolve(session, lyricsMap)
    // The frame position is only read inside derivedStateOf: the mini player recomposes when the
    // lyric line changes, not every frame.
    val position = rememberFramePosition(view.positionKey, view.playing && lyrics?.synced == true, visible = true) { now -> view.position(now) }
    val delayMs = LyricsDelay.totalMs(autoLatency, view.outputLatencyMs, offsets[session.appId] ?: 0)
    // The shared rows: no line before the lyrics, in instrumental gaps and after their end.
    val rows = remember(lyrics) { lyrics?.lyrics?.let { LyricRows.build(it) }.orEmpty() }
    val endMs = remember(rows) { LyricRows.endMs(rows) }
    val lyricLine by remember(rows, delayMs, endMs) {
        derivedStateOf {
            val p = LyricsDelay.displayPosition(position.value, delayMs)
            LyricRows.lineAt(rows, endMs, p)?.line?.text?.ifBlank { null }
        }
    }
    val playing = view.playing
    val canControl = view.canControl
    val art = view.art
    val haptics = LocalHapticFeedback.current
    val failText = commandFailureText()

    Row(
        modifier
            .fillMaxWidth()
            .height(68.dp)
            .drawBackdrop(
                backdrop = backdrop,
                shape = { RoundedRectangle(20.dp) },
                effects = {
                    vibrancy()
                    blur(12.dp.toPx())
                    lens(14.dp.toPx(), 28.dp.toPx())
                },
                onDrawSurface = { drawRect(glass.bar) },
            )
            .clickable(role = Role.Button, onClickLabel = stringResource(R.string.a11y_open_player), onClick = { ui.openPlayer() })
            // Swipe up expands; horizontal swipe = next (left) / previous (right).
            .pointerInput(view.key, view.canControl) {
                val threshold = 56.dp.toPx()
                var dx = 0f
                var dy = 0f
                detectDragGestures(
                    onDragStart = {
                        dx = 0f
                        dy = 0f
                    },
                    onDrag = { change, amount ->
                        change.consume()
                        dx += amount.x
                        dy += amount.y
                    },
                    onDragEnd = {
                        when {
                            dy < -threshold && abs(dy) > abs(dx) -> ui.openPlayer()
                            abs(dx) > threshold && abs(dx) > abs(dy) -> {
                                val next = dx < 0
                                val action = if (next) Action.NEXT else Action.PREVIOUS
                                if (view.canControl && action in session.actions) {
                                    haptics.performHapticFeedback(HapticFeedbackType.GestureThresholdActivate)
                                    scope.launch {
                                        val r = ui.command(view, action)
                                        if (!r.ok) ui.toast(failText(r.error?.code))
                                    }
                                } else {
                                    haptics.performHapticFeedback(HapticFeedbackType.Reject)
                                }
                            }
                        }
                    },
                )
            }
            .padding(start = 10.dp, end = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(48.dp).clip(RoundedCornerShape(10.dp)).background(glass.fill), contentAlignment = Alignment.Center) {
            if (art != null) {
                Image(art.asImageBitmap(), null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
            } else {
                Icon(AppIcons.Music, null, Modifier.size(22.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        Spacer(Modifier.width(14.dp))
        Column(Modifier.weight(1f)) {
            Text(
                session.title.ifEmpty { stringResource(R.string.unknown_title) },
                style = MaterialTheme.typography.bodyLarge,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (lyricLine != null) {
                CurrentLyricLine(lyricLine, style = MaterialTheme.typography.bodyMedium)
            } else {
                Text(
                    listOf(session.artist, deviceLabel(view)).filter { it.isNotEmpty() }.joinToString(stringResource(R.string.list_separator)),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        IconButton(
            onClick = {
                haptics.performHapticFeedback(HapticFeedbackType.VirtualKey)
                scope.launch {
                    val r = ui.command(view, Action.TOGGLE)
                    if (!r.ok) ui.toast(failText(r.error?.code))
                }
            },
            enabled = canControl,
        ) {
            Icon(
                if (playing) AppIcons.Pause else AppIcons.Play,
                stringResource(if (playing) R.string.action_pause else R.string.action_play),
                Modifier.size(28.dp),
                tint = MaterialTheme.colorScheme.onSurface,
            )
        }
        IconButton(
            onClick = {
                haptics.performHapticFeedback(HapticFeedbackType.VirtualKey)
                scope.launch {
                    val r = ui.command(view, Action.NEXT)
                    if (!r.ok) ui.toast(failText(r.error?.code))
                }
            },
            enabled = canControl && Action.NEXT in session.actions,
        ) {
            Icon(AppIcons.Next, stringResource(R.string.action_next), Modifier.size(24.dp), tint = MaterialTheme.colorScheme.onSurface)
        }
        Spacer(Modifier.padding(end = 4.dp))
    }
}
