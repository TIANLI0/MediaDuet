package app.mediaduet.ui.screens

import android.content.Intent
import androidx.compose.animation.AnimatedVisibility
import androidx.core.net.toUri
import androidx.compose.animation.core.snap
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.mediaduet.BuildConfig
import app.mediaduet.R
import app.mediaduet.service.WorkMode
import app.mediaduet.ui.UiContext
import app.mediaduet.ui.components.AppIcons
import app.mediaduet.ui.components.GlassButton
import app.mediaduet.ui.components.GlassCard
import app.mediaduet.ui.theme.LocalGlass
import app.mediaduet.util.VersionCompat
import com.kyant.backdrop.Backdrop

// ============================================================================ work mode

/** Settings: "With my PC" / "This phone only" as a two-row radio group. */
@Composable
fun WorkModeCard(ui: UiContext) {
    val mode by ui.engine.settings.mode.collectAsStateWithLifecycle()
    val haptics = LocalHapticFeedback.current
    GlassCard(ui.backdrop, Modifier.fillMaxWidth(), contentPadding = PaddingValues(vertical = 4.dp)) {
        ModeRow(AppIcons.Computer, stringResource(R.string.mode_linked), stringResource(R.string.mode_linked_text), mode.linked) {
            haptics.performHapticFeedback(HapticFeedbackType.Confirm)
            ui.engine.settings.setMode(WorkMode.LINKED)
        }
        HorizontalDivider(Modifier.padding(start = 58.dp), thickness = 0.5.dp, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f))
        ModeRow(AppIcons.Phone, stringResource(R.string.mode_standalone), stringResource(R.string.mode_standalone_text), !mode.linked) {
            haptics.performHapticFeedback(HapticFeedbackType.Confirm)
            ui.engine.settings.setMode(WorkMode.STANDALONE)
        }
    }
}

@Composable
private fun ModeRow(icon: ImageVector, title: String, text: String, selected: Boolean, onClick: () -> Unit) {
    val glass = LocalGlass.current
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(role = Role.RadioButton, onClick = onClick)
            .heightIn(min = 64.dp)
            .padding(horizontal = 20.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(icon, null, Modifier.size(22.dp), tint = if (selected) glass.accent else MaterialTheme.colorScheme.onSurface)
        Spacer(Modifier.width(16.dp))
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge, color = if (selected) glass.accent else MaterialTheme.colorScheme.onSurface)
            Text(text, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Spacer(Modifier.width(12.dp))
        Box(Modifier.size(22.dp), contentAlignment = Alignment.Center) {
            if (selected) Icon(AppIcons.Check, stringResource(R.string.a11y_selected), Modifier.size(20.dp), tint = glass.accent)
        }
    }
}

/**
 * First run (no choice yet and nothing paired): a calm welcome over the player asking how the app
 * will be used. Two large glass cards; the choice is changeable in Settings.
 */
@Composable
fun ModeWelcome(ui: UiContext, backdrop: Backdrop, reduced: Boolean) {
    val mode by ui.engine.settings.mode.collectAsStateWithLifecycle()
    val peers by ui.engine.trust.peers.collectAsStateWithLifecycle()
    val glass = LocalGlass.current
    val show = mode == WorkMode.UNSET && peers.isEmpty()
    var pick by remember { mutableStateOf(WorkMode.LINKED) }
    AnimatedVisibility(show, enter = fadeIn(if (reduced) snap() else tween(240)), exit = fadeOut(if (reduced) snap() else tween(240))) {
        Box(
            Modifier
                .fillMaxSize()
                // Calm and nearly opaque: only the blurred artwork colour shows through.
                .background(glass.background.copy(alpha = 0.97f))
                .clickable(remember { MutableInteractionSource() }, indication = null) { },
            contentAlignment = Alignment.Center,
        ) {
            Column(
                Modifier
                    .widthIn(max = 520.dp)
                    .fillMaxWidth()
                    .verticalScroll(rememberScrollState())
                    .statusBarsPadding()
                    .navigationBarsPadding()
                    .padding(horizontal = 24.dp, vertical = 24.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                BrandMark(Modifier.size(72.dp))
                Spacer(Modifier.height(20.dp))
                Text(stringResource(R.string.mode_welcome_title), style = MaterialTheme.typography.headlineSmall, textAlign = TextAlign.Center)
                Spacer(Modifier.height(8.dp))
                Text(
                    stringResource(R.string.mode_welcome_text),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                )
                Spacer(Modifier.height(28.dp))
                ModeChoice(backdrop, AppIcons.Computer, stringResource(R.string.mode_linked), stringResource(R.string.mode_linked_text), pick == WorkMode.LINKED) {
                    pick = WorkMode.LINKED
                }
                Spacer(Modifier.height(12.dp))
                ModeChoice(backdrop, AppIcons.Phone, stringResource(R.string.mode_standalone), stringResource(R.string.mode_standalone_text), pick == WorkMode.STANDALONE) {
                    pick = WorkMode.STANDALONE
                }
                Spacer(Modifier.height(28.dp))
                GlassButton({ ui.engine.settings.setMode(pick) }, backdrop, Modifier.fillMaxWidth(), prominent = true, height = 52.dp) {
                    Text(stringResource(R.string.mode_continue), style = MaterialTheme.typography.labelLarge)
                }
            }
        }
    }
}

@Composable
private fun ModeChoice(backdrop: Backdrop, icon: ImageVector, title: String, text: String, selected: Boolean, onClick: () -> Unit) {
    val glass = LocalGlass.current
    val haptics = LocalHapticFeedback.current
    GlassCard(
        backdrop,
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(24.dp))
            .border(if (selected) 2.dp else 0.dp, if (selected) glass.accent else Color.Transparent, RoundedCornerShape(24.dp))
            .clickable(role = Role.RadioButton) {
                haptics.performHapticFeedback(HapticFeedbackType.SegmentTick)
                onClick()
            },
        strong = selected,
        contentPadding = PaddingValues(20.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier.size(44.dp).background(if (selected) glass.accent.copy(alpha = 0.16f) else glass.fill, CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                Icon(icon, null, Modifier.size(22.dp), tint = if (selected) glass.accent else MaterialTheme.colorScheme.onSurface)
            }
            Spacer(Modifier.width(16.dp))
            Column(Modifier.weight(1f)) {
                Text(title, style = MaterialTheme.typography.titleMedium)
                Spacer(Modifier.height(2.dp))
                Text(text, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

// ============================================================================ versions

/** The paired PC's version as known now: the live session's, else the last one seen. */
@Composable
private fun peerVersion(ui: UiContext): Pair<String, String>? {
    val link by ui.engine.link.status.collectAsStateWithLifecycle()
    val peers by ui.engine.trust.peers.collectAsStateWithLifecycle()
    link.current?.peer?.let { return it.name to it.app }
    val last = peers.maxByOrNull { it.lastSeenMs } ?: return null
    return last.name to last.appVersion
}

/**
 * This phone's and the PC's MediaDuet versions side by side; a different major / minor version is
 * a warning (with "don't remind me"), a different patch a quiet note.
 */
@Composable
fun VersionCard(ui: UiContext, showWhenSame: Boolean) {
    val (name, version) = peerVersion(ui) ?: return
    val local = BuildConfig.VERSION_NAME
    val level = VersionCompat.compare(local, version)
    if (!showWhenSame && (level == VersionCompat.Level.SAME || level == VersionCompat.Level.UNKNOWN)) return
    val glass = LocalGlass.current
    val dismissed by ui.engine.settings.dismissedVersionWarning.collectAsStateWithLifecycle()
    GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
        if (level == VersionCompat.Level.MISMATCH) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(AppIcons.Warning, null, Modifier.size(20.dp), tint = glass.warn)
                Spacer(Modifier.width(10.dp))
                Text(stringResource(R.string.version_mismatch_title), style = MaterialTheme.typography.titleMedium)
            }
            Spacer(Modifier.height(12.dp))
        }
        VersionLine(AppIcons.Phone, stringResource(R.string.version_this_phone), local, warn = false)
        Spacer(Modifier.height(8.dp))
        VersionLine(AppIcons.Computer, name, version, warn = level == VersionCompat.Level.MISMATCH)
        val note = when (level) {
            VersionCompat.Level.MISMATCH -> stringResource(R.string.version_mismatch_text, name, version, local)
            VersionCompat.Level.PATCH -> stringResource(R.string.version_patch_text, name, version, local)
            else -> null
        }
        if (note != null) {
            Spacer(Modifier.height(12.dp))
            Text(note, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        val pair = "$local|$version"
        if (level == VersionCompat.Level.MISMATCH && dismissed != pair) {
            Spacer(Modifier.height(4.dp))
            TextButton({ ui.engine.settings.dismissVersionWarning(pair) }, contentPadding = PaddingValues(horizontal = 0.dp)) {
                Text(stringResource(R.string.version_dismiss), color = glass.accent)
            }
        }
    }
}

@Composable
private fun VersionLine(icon: ImageVector, device: String, version: String, warn: Boolean) {
    val glass = LocalGlass.current
    Row(verticalAlignment = Alignment.CenterVertically) {
        Icon(icon, null, Modifier.size(18.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.width(12.dp))
        Text(device, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
        Spacer(Modifier.width(12.dp))
        Text(
            if (version.isBlank()) stringResource(R.string.version_unknown) else stringResource(R.string.version_value, version),
            style = MaterialTheme.typography.bodyMedium.copy(fontFeatureSettings = "tnum"),
            color = if (warn) glass.warn else MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

// ============================================================================ about

/** Developer card: avatar, name, role and a mail link. */
@Composable
fun DeveloperCard(ui: UiContext) {
    val context = LocalContext.current
    val glass = LocalGlass.current
    val email = stringResource(R.string.about_developer_email)
    val chooser = stringResource(R.string.about_email_chooser)
    GlassCard(ui.backdrop, Modifier.fillMaxWidth(), contentPadding = PaddingValues(horizontal = 20.dp, vertical = 16.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Image(
                painterResource(R.drawable.developer_avatar),
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = Modifier
                    .size(56.dp)
                    .clip(CircleShape)
                    .border(1.dp, MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f), CircleShape),
            )
            Spacer(Modifier.width(16.dp))
            Column(Modifier.weight(1f)) {
                Text(stringResource(R.string.about_developer_name), style = MaterialTheme.typography.titleMedium)
                Text(stringResource(R.string.about_developer), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        Spacer(Modifier.height(12.dp))
        HorizontalDivider(thickness = 0.5.dp, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f))
        Row(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(12.dp))
                .clickable(role = Role.Button) {
                    val send = Intent(Intent.ACTION_SENDTO, "mailto:$email".toUri())
                    context.launchSafely(Intent.createChooser(send, chooser))
                }
                .heightIn(min = 52.dp)
                .padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(AppIcons.Mail, null, Modifier.size(20.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.width(16.dp))
            Text(stringResource(R.string.about_email), style = MaterialTheme.typography.bodyLarge, modifier = Modifier.weight(1f))
            Text(email, style = MaterialTheme.typography.bodyMedium, color = glass.accent, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}
