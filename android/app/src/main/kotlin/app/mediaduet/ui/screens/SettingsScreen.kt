package app.mediaduet.ui.screens

import android.content.Intent
import android.os.Build
import android.provider.Settings
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.heightIn
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.mediaduet.BuildConfig
import app.mediaduet.R
import app.mediaduet.audio.AudioReceiver
import app.mediaduet.protocol.Caps
import app.mediaduet.protocol.PolicyStatus
import app.mediaduet.service.AppLanguage
import app.mediaduet.service.Notifications
import app.mediaduet.service.PermissionChecks
import app.mediaduet.ui.SettingsRoute
import app.mediaduet.ui.UiContext
import app.mediaduet.ui.components.AppIcons
import app.mediaduet.ui.components.GlassButton
import app.mediaduet.update.UpdateInfo
import app.mediaduet.update.Updater
import androidx.core.net.toUri
import app.mediaduet.ui.components.GlassCard
import app.mediaduet.ui.components.InfoRow
import app.mediaduet.ui.components.SectionHeader
import app.mediaduet.ui.components.liquid.LiquidToggle
import app.mediaduet.ui.theme.LocalGlass
import kotlinx.coroutines.launch

/** iOS-style grouped settings list; every row shows a live status summary. */
@Composable
fun SettingsRoot(ui: UiContext, onBack: () -> Unit) {
    val engine = ui.engine
    val context = LocalContext.current
    val link by engine.link.status.collectAsStateWithLifecycle()
    val policy by engine.remote.policy.collectAsStateWithLifecycle()
    val translations by engine.settings.showTranslations.collectAsStateWithLifecycle()
    val audioOn by engine.settings.audioReceiveEnabled.collectAsStateWithLifecycle()
    val audio by engine.audio.status.collectAsStateWithLifecycle()
    val mode by engine.settings.mode.collectAsStateWithLifecycle()
    val linked = mode.linked
    val current = link.current
    val p = ui.permissions
    val missing = listOf(
        !p.listenerAccess,
        p.notificationsRequired && !p.notifications,
        p.bluetoothRequired && !p.bluetooth,
        p.localNetworkRequired && !p.localNetwork,
    ).count { it }

    ScreenScaffold(stringResource(R.string.settings_title), backdrop = ui.backdrop, onBack = onBack) {
        SectionHeader(stringResource(R.string.mode_header))
        WorkModeCard(ui)
        GlassCard(ui.backdrop, Modifier.fillMaxWidth(), contentPadding = PaddingValues(vertical = 4.dp)) {
            if (linked) NavRow(
                AppIcons.Link, stringResource(R.string.connection_title),
                current?.let { s ->
                    val primary = s.transports.firstOrNull { it.primary } ?: s.transports.firstOrNull()
                    listOfNotNull(primary?.let { transportLabel(it.kind) }, s.peer.name).joinToString(stringResource(R.string.list_separator))
                } ?: stringResource(R.string.conn_disconnected),
            ) { ui.openSettings(SettingsRoute.CONNECTION) }
            if (linked) RowDivider()
            NavRow(
                AppIcons.Shield, stringResource(R.string.permissions_title),
                if (missing == 0) stringResource(R.string.summary_all_set) else pluralStringResource(R.plurals.summary_missing, missing, missing),
                attention = missing > 0,
            ) { ui.openSettings(SettingsRoute.PERMISSIONS) }
        }
        GlassCard(ui.backdrop, Modifier.fillMaxWidth(), contentPadding = PaddingValues(vertical = 4.dp)) {
            if (linked) {
                NavRow(
                    AppIcons.Tune, stringResource(R.string.settings_policy_header),
                    policy?.let { policyModeLabel(it.mode) } ?: stringResource(R.string.placeholder_none),
                ) { ui.openSettings(SettingsRoute.ARBITRATION) }
                RowDivider()
            }
            NavRow(
                AppIcons.QuoteBubble, stringResource(R.string.settings_lyrics_header),
                stringResource(if (translations) R.string.summary_translations_on else R.string.summary_translations_off),
            ) { ui.openSettings(SettingsRoute.LYRICS) }
            RowDivider()
            if (linked) {
                NavRow(
                    AppIcons.Speaker, stringResource(R.string.settings_audio_row),
                    stringResource(
                        when {
                            !audioOn -> R.string.summary_off
                            audio.phase == AudioReceiver.Phase.STREAMING -> R.string.summary_streaming
                            else -> R.string.summary_on
                        },
                    ),
                ) { ui.openSettings(SettingsRoute.AUDIO) }
                RowDivider()
            }
            NavRow(
                AppIcons.Globe, stringResource(R.string.settings_language_header),
                languageLabel(remember { AppLanguage.current(context) }),
            ) { ui.openSettings(SettingsRoute.LANGUAGE) }
        }
        GlassCard(ui.backdrop, Modifier.fillMaxWidth(), contentPadding = PaddingValues(vertical = 4.dp)) {
            NavRow(
                AppIcons.Pulse, stringResource(R.string.settings_diagnostics_header),
                current?.rttMs?.let { stringResource(R.string.rtt_ms, it) } ?: stringResource(R.string.placeholder_none),
            ) { ui.openSettings(SettingsRoute.DIAGNOSTICS) }
            RowDivider()
            val update by engine.updater.state.collectAsStateWithLifecycle()
            val newVersion = (update as? Updater.State.Available)?.release?.version
            NavRow(
                AppIcons.Info, stringResource(R.string.about_title),
                newVersion?.let { stringResource(R.string.update_summary_available, it) } ?: BuildConfig.VERSION_NAME,
                attention = newVersion != null,
            ) { ui.openSettings(SettingsRoute.ABOUT) }
        }
    }
}

@Composable
private fun NavRow(icon: ImageVector, title: String, summary: String, attention: Boolean = false, onClick: () -> Unit) {
    val glass = LocalGlass.current
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(role = Role.Button, onClick = onClick)
            .heightIn(min = 60.dp)
            .padding(horizontal = 20.dp, vertical = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(icon, null, Modifier.size(22.dp), tint = MaterialTheme.colorScheme.onSurface)
        Spacer(Modifier.width(16.dp))
        Text(title, style = MaterialTheme.typography.bodyLarge, modifier = Modifier.weight(1f), maxLines = 2, overflow = TextOverflow.Ellipsis)
        Spacer(Modifier.width(8.dp))
        Text(
            summary,
            style = MaterialTheme.typography.bodyMedium,
            color = if (attention) glass.warn else MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 2,
            textAlign = androidx.compose.ui.text.style.TextAlign.End,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.widthIn(max = 168.dp),
        )
        Spacer(Modifier.width(4.dp))
        Icon(AppIcons.ChevronRight, null, Modifier.size(20.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun RowDivider() {
    HorizontalDivider(Modifier.padding(start = 58.dp), thickness = 0.5.dp, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f))
}

@Composable
private fun languageLabel(option: AppLanguage.Option): String = stringResource(
    when (option) {
        AppLanguage.Option.SYSTEM -> R.string.language_system
        AppLanguage.Option.ZH_CN -> R.string.language_zh_cn
        AppLanguage.Option.EN -> R.string.language_en
    },
)

/** Device name editor (Connection page). */
@Composable
fun DeviceNameCard(ui: UiContext) {
    val settings = ui.engine.settings
    val glass = LocalGlass.current
    val name by settings.deviceName.collectAsStateWithLifecycle()
    GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
        var draft by remember(name) { mutableStateOf(name) }
        val focus = androidx.compose.ui.platform.LocalFocusManager.current
        val savedText = stringResource(R.string.saved)
        val commit = {
            if (draft.trim() != name) {
                settings.setDeviceName(draft)
                ui.toast(savedText)
            }
        }
        OutlinedTextField(
            value = draft,
            onValueChange = { draft = it.take(64) },
            label = { Text(stringResource(R.string.settings_device_name)) },
            supportingText = { Text(stringResource(R.string.settings_device_name_hint)) },
            singleLine = true,
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
            keyboardActions = KeyboardActions(onDone = {
                commit()
                focus.clearFocus()
            }),
            colors = OutlinedTextFieldDefaults.colors(
                focusedBorderColor = glass.accent,
                unfocusedBorderColor = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.16f),
                focusedContainerColor = Color.Transparent,
                unfocusedContainerColor = Color.Transparent,
            ),
            modifier = Modifier
                .fillMaxWidth()
                .onFocusChanged { if (!it.isFocused) commit() },
        )
        Spacer(Modifier.height(8.dp))
        Text(
            stringResource(R.string.device_id_label, settings.deviceId),
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ArbitrationPage(ui: UiContext, onBack: () -> Unit) {
    val engine = ui.engine
    val scope = rememberCoroutineScope()
    val link by engine.link.status.collectAsStateWithLifecycle()
    val policy by engine.remote.policy.collectAsStateWithLifecycle()
    val current = link.current
    ScreenScaffold(stringResource(R.string.settings_policy_header), stringResource(R.string.settings_policy_text), ui.backdrop, onBack) {
        GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
            val canConfigure = current != null && Caps.POLICY_HOST in current.peerCaps
            var feedback by remember { mutableStateOf<String?>(null) }
            Text(
                policy?.let { stringResource(R.string.settings_policy_current, policyModeLabel(it.mode)) }
                    ?: stringResource(R.string.settings_policy_unknown),
                style = MaterialTheme.typography.bodyLarge,
            )
            policy?.lastAction?.let { a ->
                Spacer(Modifier.height(4.dp))
                Text(
                    stringResource(R.string.policy_last_action, policyWhatLabel(a.what), a.reason, a.app),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Spacer(Modifier.height(16.dp))
            val sentText = stringResource(R.string.settings_policy_sent)
            val failText = stringResource(R.string.settings_policy_failed)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                for (mode in PolicyStatus.MODES) {
                    GlassButton(
                        { scope.launch { feedback = if (engine.link.sendPolicyConfig(mode)) sentText else failText } },
                        ui.backdrop,
                        height = 36.dp,
                        enabled = canConfigure,
                        selected = policy?.mode == mode,
                    ) { Text(policyModeLabel(mode), style = MaterialTheme.typography.labelLarge) }
                }
            }
            if (!canConfigure) {
                Spacer(Modifier.height(8.dp))
                Text(stringResource(R.string.settings_policy_unavailable), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            feedback?.let {
                Spacer(Modifier.height(8.dp))
                Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

@Composable
fun LyricsDisplayPage(ui: UiContext, onBack: () -> Unit) {
    val settings = ui.engine.settings
    val glass = LocalGlass.current
    val translations by settings.showTranslations.collectAsStateWithLifecycle()
    val mirror by settings.mirrorEnabled.collectAsStateWithLifecycle()
    val online by settings.onlineLyrics.collectAsStateWithLifecycle()
    val lyricsSize by settings.lyricsSize.collectAsStateWithLifecycle()
    val mode by settings.mode.collectAsStateWithLifecycle()
    ScreenScaffold(stringResource(R.string.settings_lyrics_header), backdrop = ui.backdrop, onBack = onBack) {
        GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
            InfoRow(
                stringResource(R.string.settings_translations),
                subtitle = stringResource(R.string.settings_translations_text),
                trailing = {
                    LiquidToggle({ translations }, { settings.setShowTranslations(it) }, ui.backdrop, contentDescription = stringResource(R.string.settings_translations))
                },
                onClick = { settings.setShowTranslations(!translations) },
                role = Role.Switch,
            )
            HorizontalDivider(thickness = 0.5.dp, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f))
            InfoRow(
                stringResource(R.string.settings_online_lyrics),
                subtitle = stringResource(R.string.settings_online_lyrics_text),
                trailing = {
                    LiquidToggle({ online }, { settings.setOnlineLyrics(it) }, ui.backdrop, contentDescription = stringResource(R.string.settings_online_lyrics))
                },
                onClick = { settings.setOnlineLyrics(!online) },
                role = Role.Switch,
            )
            HorizontalDivider(thickness = 0.5.dp, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f))
            Spacer(Modifier.height(4.dp))
            LyricsSizePicker(lyricsSize) { settings.setLyricsSize(it) }
        }
        LyricsNotificationSettings(ui)
        LyricsTimingSettings(ui)
        if (!mode.linked) return@ScreenScaffold
        SectionHeader(stringResource(R.string.settings_mirror_header))
        GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
            InfoRow(
                stringResource(R.string.settings_mirror),
                subtitle = stringResource(R.string.settings_mirror_text),
                trailing = { LiquidToggle({ mirror }, { settings.setMirror(it) }, ui.backdrop, contentDescription = stringResource(R.string.settings_mirror)) },
                onClick = { settings.setMirror(!mirror) },
                role = Role.Switch,
            )
            if (mirror && !ui.permissions.notifications) {
                Text(stringResource(R.string.settings_mirror_needs_notifications), style = MaterialTheme.typography.bodySmall, color = glass.warn)
            }
        }
    }
}

/**
 * Settings › Lyrics › Lock screen & status bar: the lyrics notification, with a shortcut to the
 * system's Live Updates switch on Android 16+ (ColorOS 16 shows them in Fluid Cloud).
 */
@Composable
private fun LyricsNotificationSettings(ui: UiContext) {
    val settings = ui.engine.settings
    val glass = LocalGlass.current
    val context = LocalContext.current
    val enabled by settings.lyricsNotification.collectAsStateWithLifecycle()
    // Re-read whenever the permissions are refreshed (returning from system settings).
    val promoted = remember(ui.permissions) { Notifications.canPostPromoted(context) }
    SectionHeader(stringResource(R.string.settings_lyrics_notification_header))
    GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
        InfoRow(
            stringResource(R.string.settings_lyrics_notification),
            subtitle = stringResource(R.string.settings_lyrics_notification_text),
            trailing = {
                LiquidToggle({ enabled }, { settings.setLyricsNotification(it) }, ui.backdrop, contentDescription = stringResource(R.string.settings_lyrics_notification))
            },
            onClick = { settings.setLyricsNotification(!enabled) },
            role = Role.Switch,
        )
        if (enabled && !ui.permissions.notifications) {
            Text(stringResource(R.string.settings_mirror_needs_notifications), style = MaterialTheme.typography.bodySmall, color = glass.warn)
        } else if (enabled && !promoted && Build.VERSION.SDK_INT >= 36) {
            HorizontalDivider(thickness = 0.5.dp, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f))
            InfoRow(
                stringResource(R.string.settings_live_updates),
                subtitle = stringResource(R.string.settings_live_updates_text),
                trailing = { Icon(AppIcons.ChevronRight, null, Modifier.size(20.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant) },
                onClick = {
                    val promotedSettings = Intent(Settings.ACTION_APP_NOTIFICATION_PROMOTION_SETTINGS)
                        .putExtra(Settings.EXTRA_APP_PACKAGE, context.packageName)
                    if (!context.launchSafely(promotedSettings)) context.launchSafely(PermissionChecks.notificationSettingsIntent(context))
                },
            )
        }
    }
}

/**
 * Settings › Lyrics › Timing: automatic Bluetooth delay compensation (with the detected route),
 * the assumed Bluetooth delay, and the per-app offsets set in lyrics mode (each resettable).
 */
@Composable
private fun LyricsTimingSettings(ui: UiContext) {
    val settings = ui.engine.settings
    val auto by settings.lyricsAutoLatency.collectAsStateWithLifecycle()
    val btMs by settings.bluetoothLatencyMs.collectAsStateWithLifecycle()
    val offsets by settings.lyricOffsets.collectAsStateWithLifecycle()
    val output by ui.engine.outputRoute.state.collectAsStateWithLifecycle()
    val local by ui.engine.localStates.collectAsStateWithLifecycle()
    val remote by ui.engine.remote.state.collectAsStateWithLifecycle()
    val haptics = androidx.compose.ui.platform.LocalHapticFeedback.current
    SectionHeader(stringResource(R.string.settings_lyrics_timing_header))
    GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
        InfoRow(
            stringResource(R.string.settings_latency_auto),
            subtitle = stringResource(R.string.settings_latency_auto_text),
            trailing = {
                LiquidToggle({ auto }, { settings.setLyricsAutoLatency(it) }, ui.backdrop, contentDescription = stringResource(R.string.settings_latency_auto))
            },
            onClick = { settings.setLyricsAutoLatency(!auto) },
            role = Role.Switch,
        )
        Spacer(Modifier.height(4.dp))
        val routeName = stringResource(
            when (output.route) {
                app.mediaduet.media.OutputRoute.BLUETOOTH -> R.string.route_bluetooth
                app.mediaduet.media.OutputRoute.WIRED -> R.string.route_wired
                app.mediaduet.media.OutputRoute.SPEAKER -> R.string.route_speaker
                app.mediaduet.media.OutputRoute.OTHER -> R.string.route_other
            },
        )
        val routeText = listOfNotNull(routeName, output.name?.takeIf { it.isNotBlank() }).joinToString(stringResource(R.string.list_separator))
        Text(
            stringResource(R.string.settings_latency_route, routeText),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(20.dp))
        var dragMs by remember { mutableStateOf<Int?>(null) }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.settings_bt_delay), style = MaterialTheme.typography.bodyLarge, modifier = Modifier.weight(1f))
            // Live value while dragging.
            Text(stringResource(R.string.rtt_ms, dragMs ?: btMs), style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Spacer(Modifier.height(4.dp))
        app.mediaduet.ui.components.MediaSlider(
            value = { (dragMs ?: btMs) / app.mediaduet.media.OutputRoutes.MAX_BLUETOOTH_MS.toFloat() },
            onValueChange = { f ->
                // 10 ms steps, with a light tick per step.
                val ms = (f * app.mediaduet.media.OutputRoutes.MAX_BLUETOOTH_MS / 10f).toInt() * 10
                if (ms != (dragMs ?: btMs)) haptics.performHapticFeedback(androidx.compose.ui.hapticfeedback.HapticFeedbackType.SegmentFrequentTick)
                dragMs = ms
            },
            enabled = auto,
            contentDescription = stringResource(R.string.settings_bt_delay),
            onValueChangeFinished = { f ->
                settings.setBluetoothLatencyMs((f * app.mediaduet.media.OutputRoutes.MAX_BLUETOOTH_MS / 10f).toInt() * 10)
                dragMs = null
            },
        )
        Spacer(Modifier.height(8.dp))
    }
    SectionHeader(stringResource(R.string.settings_offsets))
    GlassCard(ui.backdrop, Modifier.fillMaxWidth(), contentPadding = PaddingValues(horizontal = 20.dp, vertical = 8.dp)) {
        if (offsets.isEmpty()) {
            Text(
                stringResource(R.string.settings_offsets_empty),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(vertical = 12.dp),
            )
        }
        val names = local.associate { it.packageName to it.appName } +
            remote?.snapshot?.sessions.orEmpty().associate { it.appId to it.appName.ifEmpty { it.appId } }
        for ((i, entry) in offsets.entries.sortedBy { names[it.key] ?: it.key }.withIndex()) {
            if (i > 0) HorizontalDivider(thickness = 0.5.dp, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f))
            InfoRow(
                names[entry.key] ?: entry.key,
                subtitle = stringResource(R.string.lyrics_offset_value, app.mediaduet.remote.LyricsDelay.signedSeconds(entry.value)),
                trailing = {
                    androidx.compose.material3.TextButton({ settings.setLyricOffset(entry.key, 0) }) {
                        Text(stringResource(R.string.reset))
                    }
                },
                modifier = Modifier.heightIn(min = 60.dp),
            )
        }
    }
}

@Composable
fun AudioPage(ui: UiContext, onBack: () -> Unit) {
    val settings = ui.engine.settings
    val audioOn by settings.audioReceiveEnabled.collectAsStateWithLifecycle()
    val audio by ui.engine.audio.status.collectAsStateWithLifecycle()
    ScreenScaffold(stringResource(R.string.settings_audio_header), backdrop = ui.backdrop, onBack = onBack) {
        GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
            InfoRow(
                stringResource(R.string.settings_audio),
                subtitle = stringResource(R.string.settings_audio_text),
                trailing = { LiquidToggle({ audioOn }, { settings.setAudioReceive(it) }, ui.backdrop, contentDescription = stringResource(R.string.settings_audio)) },
                onClick = { settings.setAudioReceive(!audioOn) },
                role = Role.Switch,
            )
            if (audioOn) {
                Spacer(Modifier.height(4.dp))
                Text(
                    when (audio.phase) {
                        AudioReceiver.Phase.IDLE -> stringResource(R.string.audio_idle)
                        AudioReceiver.Phase.WAITING -> stringResource(R.string.audio_waiting)
                        AudioReceiver.Phase.STREAMING -> listOf(
                            stringResource(R.string.audio_streaming, audio.sampleRate, audio.channels),
                            pluralStringResource(R.plurals.audio_packets, audio.packets.coerceAtMost(Int.MAX_VALUE.toLong()).toInt(), audio.packets),
                            pluralStringResource(R.plurals.audio_underruns, audio.underruns, audio.underruns),
                        ).joinToString(stringResource(R.string.list_separator))
                        AudioReceiver.Phase.ERROR -> stringResource(
                            R.string.audio_error,
                            stringResource(
                                when (audio.error) {
                                    AudioReceiver.ERROR_TIMEOUT -> R.string.audio_error_timeout
                                    AudioReceiver.ERROR_PROTOCOL -> R.string.audio_error_protocol
                                    else -> R.string.audio_error_io
                                },
                            ),
                        )
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
fun LanguagePage(ui: UiContext, onBack: () -> Unit) {
    val context = LocalContext.current
    val glass = LocalGlass.current
    val currentLanguage = remember { AppLanguage.current(context) }
    ScreenScaffold(stringResource(R.string.settings_language_header), stringResource(R.string.settings_language_text), ui.backdrop, onBack) {
        GlassCard(ui.backdrop, Modifier.fillMaxWidth(), contentPadding = PaddingValues(vertical = 4.dp)) {
            for ((i, option) in AppLanguage.Option.entries.withIndex()) {
                if (i > 0) RowDivider()
                Row(
                    Modifier
                        .fillMaxWidth()
                        .clickable(role = Role.RadioButton) { context.findActivity()?.let { AppLanguage.set(it, option) } }
                        .heightIn(min = 60.dp)
                        .padding(horizontal = 20.dp, vertical = 16.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(languageLabel(option), style = MaterialTheme.typography.bodyLarge, modifier = Modifier.weight(1f))
                    if (option == currentLanguage) {
                        Icon(AppIcons.Check, stringResource(R.string.a11y_selected), Modifier.size(20.dp), tint = glass.accent)
                    }
                }
            }
        }
    }
}

@Composable
fun DiagnosticsPage(ui: UiContext, onBack: () -> Unit) {
    val engine = ui.engine
    val link by engine.link.status.collectAsStateWithLifecycle()
    val now by rememberTicker(1_000)
    val current = link.current
    ScreenScaffold(stringResource(R.string.settings_diagnostics_header), backdrop = ui.backdrop, onBack = onBack) {
        CrashReportCard(ui)
        GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
            val c = engine.link.counters
            val rows = listOf(
                stringResource(R.string.diag_connection) to (
                    current?.transports?.map {
                        if (it.primary) stringResource(R.string.transport_primary, transportLabel(it.kind), stringResource(R.string.conn_primary))
                        else transportLabel(it.kind)
                    }?.joinToString(stringResource(R.string.list_separator)) ?: stringResource(R.string.conn_disconnected)
                    ),
                stringResource(R.string.diag_rtt) to (current?.rttMs?.let { stringResource(R.string.rtt_ms, it) } ?: stringResource(R.string.placeholder_none)),
                stringResource(R.string.diag_uptime) to (if (engine.startedAtMs > 0) durationText(now - engine.startedAtMs) else stringResource(R.string.placeholder_none)),
                stringResource(R.string.diag_bytes) to stringResource(R.string.traffic_in_out, bytesText(c.bytesIn.get()), bytesText(c.bytesOut.get())),
                stringResource(R.string.diag_messages) to stringResource(R.string.traffic_in_out, c.messagesIn.get().toString(), c.messagesOut.get().toString()),
                stringResource(R.string.diag_blobs) to stringResource(R.string.traffic_in_out, c.blobChunksIn.get().toString(), c.blobChunksOut.get().toString()),
                stringResource(R.string.diag_handshakes) to stringResource(R.string.ok_failed_counts, c.handshakesOk.get(), c.handshakesFailed.get()),
                stringResource(R.string.diag_errors) to c.protocolErrors.get().toString(),
                stringResource(R.string.diag_resumes) to (current?.resumeCount?.toString() ?: stringResource(R.string.placeholder_none)),
            )
            for ((label, value) in rows) {
                Row(Modifier.fillMaxWidth().padding(vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(label, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
                    Text(value, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            current?.transports?.forEach { t ->
                Text(
                    stringResource(R.string.conn_transport_line, t.connectionId, transportLabel(t.kind), t.remote),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

/** "The app crashed last time": share / copy the local report, or dismiss it. Nothing is uploaded. */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun CrashReportCard(ui: UiContext) {
    val log = app.mediaduet.util.CrashLog.instance ?: return
    var report by remember { mutableStateOf(log.latestUnseen()) }
    val file = report ?: return
    val context = LocalContext.current
    val glass = LocalGlass.current
    val text = remember(file) { runCatching { file.readText() }.getOrDefault("") }
    val copied = stringResource(R.string.diag_crash_copied)
    val subject = stringResource(R.string.diag_crash_subject)
    GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(AppIcons.Warning, null, Modifier.size(22.dp), tint = glass.warn)
            Spacer(Modifier.width(12.dp))
            Text(stringResource(R.string.diag_crash_title), style = MaterialTheme.typography.titleMedium)
        }
        Spacer(Modifier.height(8.dp))
        Text(
            stringResource(
                R.string.diag_crash_text,
                java.text.DateFormat.getDateTimeInstance(java.text.DateFormat.MEDIUM, java.text.DateFormat.SHORT)
                    .format(java.util.Date(app.mediaduet.util.CrashLog.timeOf(file))),
            ),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(16.dp))
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            GlassButton(
                {
                    val send = android.content.Intent(android.content.Intent.ACTION_SEND)
                        .setType("text/plain")
                        .putExtra(android.content.Intent.EXTRA_SUBJECT, subject)
                        .putExtra(android.content.Intent.EXTRA_TEXT, text)
                    context.launchSafely(android.content.Intent.createChooser(send, subject))
                },
                ui.backdrop, height = 44.dp, prominent = true,
            ) { Text(stringResource(R.string.diag_crash_share)) }
            GlassButton(
                {
                    val cm = context.getSystemService(android.content.ClipboardManager::class.java)
                    cm?.setPrimaryClip(android.content.ClipData.newPlainText(subject, text))
                    ui.toast(copied)
                },
                ui.backdrop, height = 44.dp,
            ) { Text(stringResource(R.string.diag_crash_copy)) }
            GlassButton(
                {
                    log.markSeen()
                    report = null
                },
                ui.backdrop, height = 44.dp,
            ) { Text(stringResource(R.string.dismiss)) }
        }
    }
}

/**
 * Software update (Settings > About): the state of [Updater] with one action (check / update /
 * allow installing / retry), a progress bar while downloading, the release page and the switch for
 * the daily automatic check.
 */
@Composable
private fun UpdateCard(ui: UiContext) {
    val updater = ui.engine.updater
    val context = LocalContext.current
    val state by updater.state.collectAsStateWithLifecycle()
    val auto by ui.engine.settings.autoUpdateCheck.collectAsStateWithLifecycle()
    val current = BuildConfig.VERSION_NAME
    var title = stringResource(R.string.update_title_current_version, current)
    var detail: String? = null
    var action: Pair<String, () -> Unit>? = stringResource(R.string.update_check) to { updater.checkNow() }
    var progress: Float? = null
    var release: UpdateInfo.Release? = null
    when (val s = state) {
        Updater.State.Idle -> Unit
        Updater.State.Checking -> {
            title = stringResource(R.string.update_checking)
            action = null
        }
        is Updater.State.Current -> {
            title = stringResource(R.string.update_current)
            detail = stringResource(R.string.update_current_detail, current)
        }
        is Updater.State.Available -> {
            release = s.release
            title = stringResource(R.string.update_available, s.release.version)
            detail = stringResource(R.string.update_available_detail, current)
            action = stringResource(R.string.update_install) to { updater.install() }
        }
        is Updater.State.Downloading -> {
            release = s.release
            title = stringResource(R.string.update_downloading, s.release.version)
            detail = "${(s.progress * 100).toInt()}%"
            progress = s.progress
            action = null
        }
        is Updater.State.NeedsPermission -> {
            release = s.release
            title = stringResource(R.string.update_needs_permission, s.release.version)
            detail = stringResource(R.string.update_needs_permission_detail)
            action = stringResource(R.string.update_allow) to { context.launchSafely(updater.openInstallPermission()) }
        }
        is Updater.State.Installing -> {
            release = s.release
            title = stringResource(R.string.update_installing, s.release.version)
            detail = stringResource(R.string.update_installing_detail)
            action = null
        }
        is Updater.State.Error -> {
            release = s.release
            title = stringResource(R.string.update_failed)
            detail = stringResource(UPDATE_ERRORS[s.code] ?: R.string.update_err_install)
            if (s.release != null && UpdateInfo.isNewer(s.release.version, current)) action = stringResource(R.string.update_retry) to { updater.install() }
        }
    }
    // Back from Android's "install unknown apps" page: continue by itself once it is allowed.
    androidx.lifecycle.compose.LifecycleResumeEffect(key1 = state) {
        if (state is Updater.State.NeedsPermission && context.packageManager.canRequestPackageInstalls()) updater.install()
        onPauseOrDispose { }
    }
    GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
        Text(title, style = MaterialTheme.typography.titleMedium)
        detail?.let {
            Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(top = 2.dp))
        }
        progress?.let {
            androidx.compose.material3.LinearProgressIndicator(
                progress = { it },
                modifier = Modifier.fillMaxWidth().padding(top = 12.dp),
                color = LocalGlass.current.accent,
            )
        }
        Spacer(Modifier.height(12.dp))
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            action?.let { (label, run) ->
                GlassButton(run, ui.backdrop, height = 44.dp, prominent = state is Updater.State.Available || state is Updater.State.NeedsPermission) { Text(label) }
            }
            val page = release?.page?.takeIf { release != null && UpdateInfo.isNewer(release.version, current) }
            if (page != null) {
                GlassButton({ context.launchSafely(android.content.Intent(android.content.Intent.ACTION_VIEW, page.toUri())) }, ui.backdrop, height = 44.dp) {
                    Text(stringResource(R.string.update_notes))
                }
            }
        }
    }
    GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
        InfoRow(
            stringResource(R.string.update_auto),
            subtitle = stringResource(R.string.update_auto_text),
            trailing = { LiquidToggle({ auto }, { ui.engine.settings.setAutoUpdateCheck(it) }, ui.backdrop, contentDescription = stringResource(R.string.update_auto)) },
            onClick = { ui.engine.settings.setAutoUpdateCheck(!auto) },
            role = Role.Switch,
        )
    }
}

private val UPDATE_ERRORS = mapOf(
    Updater.ERR_NETWORK to R.string.update_err_network,
    Updater.ERR_NO_RELEASE to R.string.update_err_no_release,
    Updater.ERR_NO_ASSET to R.string.update_err_no_asset,
    Updater.ERR_NO_CHECKSUM to R.string.update_err_no_checksum,
    Updater.ERR_CHECKSUM to R.string.update_err_checksum,
    Updater.ERR_NOT_NEWER to R.string.update_err_not_newer,
    Updater.ERR_INSTALL to R.string.update_err_install,
    Updater.ERR_SIGNATURE to R.string.update_err_signature,
    Updater.ERR_STORAGE to R.string.update_err_storage,
    Updater.ERR_BLOCKED to R.string.update_err_blocked,
)

/** The unified MediaDuet mark (same drawable as the launcher icon) on the brand violet. */
@Composable
fun BrandMark(modifier: Modifier = Modifier) {
    androidx.compose.foundation.layout.Box(
        modifier
            .clip(androidx.compose.foundation.shape.RoundedCornerShape(22))
            .background(androidx.compose.ui.res.colorResource(R.color.launcher_background)),
    ) {
        androidx.compose.foundation.Image(
            androidx.compose.ui.res.painterResource(R.drawable.ic_launcher_foreground),
            contentDescription = stringResource(R.string.app_name),
            modifier = Modifier.fillMaxSize(),
        )
    }
}

@Composable
fun AboutPage(ui: UiContext, onBack: () -> Unit) {
    val mode by ui.engine.settings.mode.collectAsStateWithLifecycle()
    ScreenScaffold(stringResource(R.string.about_title), backdrop = ui.backdrop, onBack = onBack) {
        GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
            BrandMark(Modifier.size(72.dp))
            Spacer(Modifier.height(16.dp))
            Text(stringResource(R.string.app_name), style = MaterialTheme.typography.titleLarge)
            Text(
                stringResource(R.string.version_protocol, BuildConfig.VERSION_NAME, 1),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(12.dp))
            Text(stringResource(R.string.about_text), style = MaterialTheme.typography.bodyMedium)
            Spacer(Modifier.height(8.dp))
            Text(
                stringResource(R.string.about_project_license),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        SectionHeader(stringResource(R.string.update_header))
        UpdateCard(ui)
        SectionHeader(stringResource(R.string.about_developer))
        DeveloperCard(ui)
        if (mode.linked) {
            SectionHeader(stringResource(R.string.version_title))
            VersionCard(ui, showWhenSame = true)
        }
        SectionHeader(stringResource(R.string.about_licenses))
        GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.about_license_backdrop), style = MaterialTheme.typography.bodySmall)
                Text(stringResource(R.string.about_license_androidx), style = MaterialTheme.typography.bodySmall)
                Text(stringResource(R.string.about_license_icons), style = MaterialTheme.typography.bodySmall)
            }
        }
    }
}
