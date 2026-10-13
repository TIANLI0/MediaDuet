package app.mediaduet.ui.screens

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.mediaduet.R
import app.mediaduet.media.LocalSessionState
import app.mediaduet.ui.SettingsRoute
import app.mediaduet.ui.UiContext
import app.mediaduet.ui.components.AppIcons
import app.mediaduet.ui.components.EmptyState
import app.mediaduet.ui.components.GlassButton
import app.mediaduet.ui.components.GlassCard
import app.mediaduet.ui.components.StatusChip
import app.mediaduet.ui.theme.LocalGlass
import java.text.DateFormat
import java.util.Date

/** What Windows sees: the phone's media sessions exactly as they are sent in snapshots. */
@Composable
fun PhoneMediaSection(ui: UiContext) {
    val engine = ui.engine
    val states by engine.localStates.collectAsStateWithLifecycle()
    val access by engine.media.accessGranted.collectAsStateWithLifecycle()
    val lastSent by engine.lastSentSnapshot.collectAsStateWithLifecycle()
    val lastCommand by engine.media.lastCommand.collectAsStateWithLifecycle()
    val now by rememberTicker(500)

    Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Text(
            stringResource(R.string.phone_subtitle),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(horizontal = 8.dp),
        )
        when {
            !access -> GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
                Text(stringResource(R.string.phone_no_access), style = MaterialTheme.typography.bodyMedium)
                Spacer(Modifier.height(12.dp))
                GlassButton({ ui.openSettings(SettingsRoute.PERMISSIONS) }, ui.backdrop, height = 40.dp, prominent = true) {
                    Text(stringResource(R.string.setup_permissions))
                }
            }
            states.isEmpty() -> EmptyState(AppIcons.Music, stringResource(R.string.phone_empty))
            else -> for (s in states) SessionCard(ui, s, isActive = s.key == lastSent?.active, nowMs = now)
        }
        Column(Modifier.padding(horizontal = 8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(
                lastSent?.let { pluralStringResource(R.plurals.phone_sync_gen, it.sessions.size, it.gen, it.sessions.size) }
                    ?: stringResource(R.string.phone_sync_none),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            lastCommand?.let { c ->
                Text(
                    stringResource(
                        R.string.phone_last_command,
                        actionLabel(c.action),
                        stringResource(if (c.origin == "policy") R.string.origin_policy else R.string.origin_user),
                        DateFormat.getTimeInstance(DateFormat.MEDIUM).format(Date(c.atWallMs)),
                    ),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun SessionCard(ui: UiContext, s: LocalSessionState, isActive: Boolean, nowMs: Long) {
    val glass = LocalGlass.current
    val hash = s.artwork?.hash
    val thumb = remember(hash) {
        hash?.let { ui.engine.artwork.bytes(it) }?.let { BitmapFactory.decodeByteArray(it, 0, it.size)?.asImageBitmap() }
    }
    GlassCard(ui.backdrop, Modifier.fillMaxWidth(), contentPadding = PaddingValues(16.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier.size(64.dp).clip(RoundedCornerShape(12.dp)).background(glass.fill),
                contentAlignment = Alignment.Center,
            ) {
                if (thumb != null) {
                    Image(thumb, null, Modifier.size(64.dp), contentScale = ContentScale.Crop)
                } else {
                    Icon(AppIcons.Music, null, Modifier.size(28.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            Spacer(Modifier.width(16.dp))
            Column(Modifier.weight(1f)) {
                Text(
                    s.title.ifEmpty { stringResource(R.string.unknown_title) },
                    style = MaterialTheme.typography.titleMedium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                val sub = s.artist.ifEmpty { s.album }
                if (sub.isNotEmpty()) {
                    Text(sub, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                Text(s.appName, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
        Spacer(Modifier.height(12.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
            StatusChip(statusLabel(s.status), dot = if (s.isActivelyPlaying) glass.good else null)
            if (isActive) StatusChip(stringResource(R.string.phone_active), dot = glass.accent)
            Spacer(Modifier.weight(1f))
            Text(
                stringResource(R.string.position_of_duration, formatTime(s.estimatedPosition(nowMs)), formatTime(s.durationMs)),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Spacer(Modifier.height(8.dp))
        val actions = s.protocolActions()
        Text(
            if (actions.isEmpty()) {
                stringResource(R.string.phone_no_actions)
            } else {
                stringResource(R.string.phone_actions, actions.map { actionLabel(it) }.joinToString(stringResource(R.string.list_separator)))
            },
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}
