package app.mediaduet.ui.screens

import android.Manifest
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.mediaduet.R
import app.mediaduet.service.LinkService
import app.mediaduet.service.PermissionChecks
import app.mediaduet.transport.TransportController
import app.mediaduet.ui.SettingsRoute
import app.mediaduet.ui.UiContext
import app.mediaduet.ui.components.AppIcons
import app.mediaduet.ui.components.GlassButton
import app.mediaduet.ui.components.GlassCard
import app.mediaduet.ui.components.InfoRow
import app.mediaduet.ui.components.SectionHeader
import app.mediaduet.ui.components.liquid.LiquidToggle
import app.mediaduet.ui.theme.LocalGlass

/**
 * Permissions & background. Each row: state icon + label, one primary action, explanation
 * collapsed by default. "Fix all" walks through every missing critical item. State is
 * re-evaluated automatically when returning from system settings (Root's onResume refresh).
 */
@Composable
fun PermissionsPage(ui: UiContext, onBack: () -> Unit) {
    val context = LocalContext.current
    val p = ui.permissions
    val fgsRunning by LinkService.running.collectAsStateWithLifecycle()
    val fgsEnabled by ui.engine.settings.foregroundService.collectAsStateWithLifecycle()
    val listenerBound by ui.engine.listenerConnected.collectAsStateWithLifecycle()
    val request = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { ui.refreshPermissions() }

    val missingRuntime = buildList {
        if (Build.VERSION.SDK_INT >= 33 && !p.notifications && !PermissionChecks.granted(context, Manifest.permission.POST_NOTIFICATIONS)) {
            add(Manifest.permission.POST_NOTIFICATIONS)
        }
        if (Build.VERSION.SDK_INT >= 31 && !p.bluetooth) add(Manifest.permission.BLUETOOTH_CONNECT)
        if (p.localNetworkRequired && !p.localNetwork) add(TransportController.PERMISSION_LOCAL_NETWORK)
    }
    val openListener = {
        if (!context.launchSafely(PermissionChecks.listenerSettingsIntent(context))) {
            context.launchSafely(PermissionChecks.appDetailsIntent(context))
        }
    }
    // Guided "Fix all": runtime permissions first (one system prompt), then notification access.
    val fixAll = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        ui.refreshPermissions()
        if (!PermissionChecks.snapshot(context).listenerAccess) openListener()
    }
    val needsFix = missingRuntime.isNotEmpty() || !p.listenerAccess

    ScreenScaffold(stringResource(R.string.permissions_title), stringResource(R.string.permissions_subtitle), ui.backdrop, onBack) {
        if (needsFix) {
            GlassButton(
                {
                    if (missingRuntime.isNotEmpty()) fixAll.launch(missingRuntime.toTypedArray()) else openListener()
                },
                ui.backdrop, Modifier.fillMaxWidth(), prominent = true,
            ) { Text(stringResource(R.string.perm_fix_all)) }
        }
        SectionHeader(stringResource(R.string.perm_required_header))
        GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
            PermissionRow(
                ui, stringResource(R.string.perm_listener), stringResource(R.string.perm_listener_text),
                ok = p.listenerAccess,
                extra = if (Build.VERSION.SDK_INT >= 33 && !p.listenerAccess) stringResource(R.string.perm_listener_restricted) else null,
                action = stringResource(R.string.open_settings),
                onAction = openListener,
            )
            if (p.notificationsRequired) {
                Divider()
                PermissionRow(
                    ui, stringResource(R.string.perm_notifications), stringResource(R.string.perm_notifications_text),
                    ok = p.notifications, action = stringResource(R.string.grant),
                ) {
                    if (Build.VERSION.SDK_INT >= 33 && !PermissionChecks.granted(context, Manifest.permission.POST_NOTIFICATIONS)) {
                        request.launch(Manifest.permission.POST_NOTIFICATIONS)
                    } else {
                        context.launchSafely(PermissionChecks.notificationSettingsIntent(context))
                    }
                }
            }
            if (p.bluetoothRequired) {
                Divider()
                PermissionRow(
                    ui, stringResource(R.string.perm_bluetooth), stringResource(R.string.perm_bluetooth_text),
                    ok = p.bluetooth, action = stringResource(R.string.grant),
                ) {
                    if (Build.VERSION.SDK_INT >= 31) request.launch(Manifest.permission.BLUETOOTH_CONNECT)
                }
            }
            if (p.localNetworkRequired) {
                Divider()
                PermissionRow(
                    ui, stringResource(R.string.perm_local_network), stringResource(R.string.perm_local_network_text),
                    ok = p.localNetwork, action = stringResource(R.string.grant),
                ) { request.launch(TransportController.PERMISSION_LOCAL_NETWORK) }
            }
        }

        SectionHeader(stringResource(R.string.perm_background_header))
        GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
            val setFgs: (Boolean) -> Unit = { on ->
                ui.engine.settings.setForegroundService(on)
                if (on) LinkService.start(context, "settings") else LinkService.stop(context)
            }
            InfoRow(
                stringResource(R.string.settings_fgs),
                subtitle = stringResource(
                    if (fgsRunning) R.string.state_running else R.string.state_not_running,
                ),
                trailing = {
                    LiquidToggle({ fgsEnabled }, setFgs, ui.backdrop, contentDescription = stringResource(R.string.settings_fgs))
                },
                onClick = { setFgs(!fgsEnabled) },
                role = Role.Switch,
            )
            Divider()
            PermissionRow(
                ui, stringResource(R.string.perm_listener_bound), stringResource(R.string.perm_listener_bound_text),
                ok = listenerBound, okText = stringResource(R.string.state_bound), badText = stringResource(R.string.state_not_bound),
            )
            Divider()
            PermissionRow(
                ui, stringResource(R.string.perm_battery), stringResource(R.string.perm_battery_text),
                ok = p.batteryUnrestricted, okText = stringResource(R.string.state_unrestricted), badText = stringResource(R.string.state_optimized),
                action = stringResource(R.string.open_settings),
            ) { context.launchSafely(PermissionChecks.batterySettingsIntent()) }
            if (p.backgroundRestricted) {
                Divider()
                PermissionRow(
                    ui, stringResource(R.string.perm_bg_restricted), stringResource(R.string.perm_bg_restricted_text),
                    ok = false, badText = stringResource(R.string.state_restricted), action = stringResource(R.string.open_settings),
                ) { context.launchSafely(PermissionChecks.appDetailsIntent(context)) }
            }
            Divider()
            PermissionRow(
                ui, stringResource(R.string.perm_companion), stringResource(R.string.perm_companion_text),
                ok = p.companionAssociated,
                okText = stringResource(R.string.state_associated),
                badText = stringResource(if (p.companionSupported) R.string.state_not_associated else R.string.state_unsupported),
                action = if (p.companionSupported) stringResource(R.string.set_up) else null,
            ) { ui.openSettings(SettingsRoute.CONNECTION) }
        }

        SectionHeader(stringResource(R.string.oem_header))
        GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
            Text(stringResource(R.string.oem_intro), style = MaterialTheme.typography.bodyMedium)
            Spacer(Modifier.height(8.dp))
            for (line in listOf(R.string.oem_xiaomi, R.string.oem_huawei, R.string.oem_oppo, R.string.oem_vivo, R.string.oem_samsung, R.string.oem_pixel)) {
                Text(
                    stringResource(R.string.bullet_item, stringResource(line)),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(vertical = 2.dp),
                )
            }
            Spacer(Modifier.height(16.dp))
            GlassButton({ context.launchSafely(PermissionChecks.appDetailsIntent(context)) }, ui.backdrop) {
                Text(stringResource(R.string.open_app_info))
            }
        }
    }
}

@Composable
private fun Divider() {
    HorizontalDivider(Modifier.padding(vertical = 4.dp), thickness = 0.5.dp, color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f))
}

@Composable
private fun ColumnScope.PermissionRow(
    ui: UiContext,
    title: String,
    text: String,
    ok: Boolean,
    okText: String? = null,
    badText: String? = null,
    extra: String? = null,
    action: String? = null,
    onAction: () -> Unit = {},
) {
    val glass = LocalGlass.current
    // Collapsed by default; expanded automatically for items that need attention and have
    // something extra to say (e.g. restricted settings on sideloaded installs).
    var expanded by rememberSaveable(title) { mutableStateOf(!ok && extra != null) }
    val rotation by animateFloatAsState(if (expanded) 180f else 0f, label = "expand")
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .clickable(role = Role.Button, onClickLabel = stringResource(R.string.a11y_details)) { expanded = !expanded }
            .heightIn(min = 56.dp)
            .padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(
            if (ok) AppIcons.CheckCircle else AppIcons.Warning,
            null,
            Modifier.size(22.dp),
            tint = if (ok) glass.good else glass.warn,
        )
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            Text(
                if (ok) okText ?: stringResource(R.string.state_granted) else badText ?: stringResource(R.string.state_missing),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        if (!ok && action != null) {
            Spacer(Modifier.width(8.dp))
            GlassButton(onAction, ui.backdrop, height = 40.dp, prominent = true) { Text(action, style = MaterialTheme.typography.labelLarge) }
        }
        Icon(AppIcons.ExpandMore, null, Modifier.padding(start = 4.dp).size(20.dp).rotate(rotation), tint = MaterialTheme.colorScheme.onSurfaceVariant)
    }
    AnimatedVisibility(expanded) {
        Column(Modifier.padding(start = 34.dp, bottom = 8.dp)) {
            Text(text, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (extra != null) {
                Spacer(Modifier.height(4.dp))
                Text(extra, style = MaterialTheme.typography.bodySmall, color = glass.warn)
            }
        }
    }
}
