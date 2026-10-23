package app.mediaduet.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.snap
import androidx.compose.animation.core.spring
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import app.mediaduet.ui.components.GlassButton
import app.mediaduet.ui.components.GlassCard
import app.mediaduet.ui.theme.LocalGlass
import com.kyant.backdrop.Backdrop
import kotlinx.coroutines.delay

/** Height of everything floating at the bottom (mini player on the Settings stack + nav bar). */
val LocalBottomInset = compositionLocalOf { 160.dp }

/** True when Settings shows its list and the open page side by side (tablets, wide windows). */
val LocalTwoPane = compositionLocalOf { false }

/** Window width from which Settings uses two panes (Material "expanded"). */
val TWO_PANE_MIN_WIDTH = 840.dp

data class Toast(val id: Long, val text: String)

data class ConfirmRequest(
    val title: String,
    val text: String,
    val confirmLabel: String,
    val cancelLabel: String,
    val destructive: Boolean,
    val onConfirm: () -> Unit,
)

/** Small, non-blocking glass toast above the bottom bars; auto-hides after 2.5 s. */
@Composable
fun ToastHost(toast: Toast?, bottomInset: Dp, backdrop: Backdrop, reduced: Boolean, onDone: () -> Unit) {
    LaunchedEffect(toast?.id) {
        if (toast != null) {
            delay(2_500)
            onDone()
        }
    }
    Box(Modifier.fillMaxSize().padding(bottom = bottomInset + 8.dp), contentAlignment = Alignment.BottomCenter) {
        AnimatedVisibility(
            toast != null,
            enter = if (reduced) fadeIn(snap()) else slideInVertically(spring(dampingRatio = 0.8f)) { it / 2 } + fadeIn(),
            exit = if (reduced) fadeOut(snap()) else slideOutVertically { it / 2 } + fadeOut(),
        ) {
            val t = toast ?: return@AnimatedVisibility
            GlassCard(
                backdrop,
                Modifier.padding(horizontal = 24.dp).widthIn(max = 480.dp),
                strong = true,
                cornerRadius = 20.dp,
                contentPadding = PaddingValues(horizontal = 20.dp, vertical = 12.dp),
            ) {
                Text(t.text, style = MaterialTheme.typography.bodyMedium, textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth())
            }
        }
    }
}

/** Liquid-glass confirmation dialog for destructive actions. */
@Composable
fun ConfirmHost(request: ConfirmRequest?, backdrop: Backdrop, reduced: Boolean, onDismiss: () -> Unit) {
    val glass = LocalGlass.current
    AnimatedVisibility(request != null, enter = fadeIn(if (reduced) snap() else spring()), exit = fadeOut(if (reduced) snap() else spring())) {
        val r = request ?: return@AnimatedVisibility
        Box(
            Modifier
                .fillMaxSize()
                .background(Color.Black.copy(alpha = if (glass.isDark) 0.45f else 0.2f))
                .clickable(remember { MutableInteractionSource() }, indication = null, onClick = onDismiss),
            contentAlignment = Alignment.Center,
        ) {
            AnimatedVisibility(true, enter = if (reduced) fadeIn(snap()) else scaleIn(initialScale = 0.92f) + fadeIn()) {
                GlassCard(
                    backdrop,
                    Modifier
                        .padding(24.dp)
                        .widthIn(max = 400.dp)
                        .clickable(remember { MutableInteractionSource() }, indication = null) { },
                    strong = true,
                    cornerRadius = 28.dp,
                    contentPadding = PaddingValues(24.dp),
                ) {
                    Text(r.title, style = MaterialTheme.typography.titleLarge)
                    Spacer(Modifier.height(8.dp))
                    Text(r.text, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Spacer(Modifier.height(24.dp))
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        GlassButton(onDismiss, backdrop, Modifier.weight(1f), height = 48.dp) { Text(r.cancelLabel) }
                        GlassButton(
                            {
                                r.onConfirm()
                                onDismiss()
                            },
                            backdrop, Modifier.weight(1f), height = 48.dp, prominent = !r.destructive, destructive = r.destructive,
                        ) { Text(r.confirmLabel) }
                    }
                }
            }
        }
    }
}
