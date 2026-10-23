package app.mediaduet.ui.screens

import androidx.compose.animation.AnimatedVisibility
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
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.mediaduet.R
import app.mediaduet.service.Engine
import app.mediaduet.ui.components.GlassButton
import app.mediaduet.ui.components.GlassCard
import app.mediaduet.ui.theme.LocalGlass
import com.kyant.backdrop.Backdrop
import kotlinx.coroutines.delay

/** SAS comparison dialog (§3.3) as a liquid-glass sheet over the page, plus a result banner. */
@Composable
fun PairingOverlay(engine: Engine, backdrop: Backdrop, onRetry: () -> Unit) {
    val state by engine.pairing.state.collectAsStateWithLifecycle()
    val glass = LocalGlass.current
    val prompt = state.prompt

    AnimatedVisibility(prompt != null, enter = fadeIn(), exit = fadeOut()) {
        Box(
            Modifier
                .fillMaxSize()
                .background(Color.Black.copy(alpha = if (glass.isDark) 0.45f else 0.2f))
                .clickable(remember { MutableInteractionSource() }, indication = null) { },
            contentAlignment = Alignment.Center,
        ) {
            val p = prompt ?: return@Box
            AnimatedVisibility(true, enter = scaleIn(initialScale = 0.92f) + fadeIn()) {
                GlassCard(backdrop, Modifier.padding(24.dp).widthIn(max = 400.dp), strong = true, cornerRadius = 32.dp, contentPadding = PaddingValues(24.dp)) {
                    Text(stringResource(R.string.pair_dialog_title), style = MaterialTheme.typography.titleLarge)
                    Spacer(Modifier.height(8.dp))
                    Text(
                        stringResource(if (p.outgoing) R.string.pair_dialog_text_outgoing else R.string.pair_dialog_text, p.peerName, transportLabel(p.transport)),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.height(24.dp))
                    Text(
                        stringResource(R.string.sas_format, p.sas.substring(0, 3), p.sas.substring(3)),
                        modifier = Modifier.fillMaxWidth(),
                        textAlign = TextAlign.Center,
                        fontSize = 44.sp,
                        fontWeight = FontWeight.SemiBold,
                        fontFamily = FontFamily.Monospace,
                        letterSpacing = 4.sp,
                        color = MaterialTheme.colorScheme.onSurface,
                    )
                    Spacer(Modifier.height(24.dp))
                    if (p.localAccepted) {
                        Text(
                            stringResource(R.string.pair_waiting_peer),
                            modifier = Modifier.fillMaxWidth(),
                            textAlign = TextAlign.Center,
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    } else {
                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                            GlassButton({ engine.pairing.reject(p.attemptId) }, backdrop, Modifier.weight(1f), height = 48.dp) {
                                Text(stringResource(R.string.pair_reject))
                            }
                            GlassButton({ engine.pairing.accept(p.attemptId) }, backdrop, Modifier.weight(1f), height = 48.dp, prominent = true) {
                                Text(stringResource(R.string.pair_accept))
                            }
                        }
                    }
                }
            }
        }
    }

    val outcome = state.lastOutcome
    LaunchedEffect(outcome) {
        // Success disappears by itself; failures stay until the user retries or dismisses.
        if (outcome != null && outcome.success) {
            delay(4_000)
            engine.pairing.clearOutcome()
        }
    }
    AnimatedVisibility(
        outcome != null && prompt == null,
        enter = slideInVertically { -it } + fadeIn(),
        exit = slideOutVertically { -it } + fadeOut(),
    ) {
        Box(Modifier.fillMaxSize().statusBarsPadding().padding(16.dp), contentAlignment = Alignment.TopCenter) {
            val o = outcome ?: return@Box
            GlassCard(backdrop, Modifier.widthIn(max = 480.dp).fillMaxWidth(), strong = true, contentPadding = PaddingValues(16.dp)) {
                Text(
                    if (o.success) stringResource(R.string.pairing_success, o.peerName)
                    else stringResource(R.string.pairing_failed, o.peerName, pairingReasonLabel(o.reason)),
                    style = MaterialTheme.typography.bodyLarge,
                )
                if (!o.success) {
                    Spacer(Modifier.height(12.dp))
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        GlassButton({
                            engine.pairing.clearOutcome()
                            engine.pairing.open()
                            onRetry()
                        }, backdrop, height = 40.dp, prominent = true) { Text(stringResource(R.string.pairing_retry)) }
                        GlassButton({ engine.pairing.clearOutcome() }, backdrop, height = 40.dp) { Text(stringResource(R.string.dismiss)) }
                    }
                }
            }
        }
    }
}
