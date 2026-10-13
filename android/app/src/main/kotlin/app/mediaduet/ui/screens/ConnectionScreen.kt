package app.mediaduet.ui.screens

import android.app.Activity
import android.content.IntentSender
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.IntentSenderRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.material3.AlertDialog
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.mediaduet.R
import app.mediaduet.link.TransportKind
import app.mediaduet.link.TrustedPeer
import app.mediaduet.service.CompanionHelper
import app.mediaduet.transport.BluetoothListenState
import app.mediaduet.transport.NsdAdvertiser
import app.mediaduet.ui.ConfirmRequest
import app.mediaduet.ui.SettingsRoute
import androidx.compose.ui.semantics.Role
import app.mediaduet.ui.UiContext
import app.mediaduet.ui.components.AppIcons
import app.mediaduet.ui.components.GlassButton
import app.mediaduet.ui.components.GlassCard
import app.mediaduet.ui.components.InfoRow
import app.mediaduet.ui.components.SectionHeader
import app.mediaduet.ui.components.StatusChip
import app.mediaduet.ui.components.liquid.LiquidToggle
import app.mediaduet.ui.theme.LocalGlass
import java.text.DateFormat
import java.util.Date

@Composable
fun ConnectionPage(ui: UiContext, onBack: () -> Unit) {
    val engine = ui.engine
    val glass = LocalGlass.current
    val link by engine.link.status.collectAsStateWithLifecycle()
    val transports by engine.transports.state.collectAsStateWithLifecycle()
    val pairing by engine.pairing.state.collectAsStateWithLifecycle()
    val peers by engine.trust.peers.collectAsStateWithLifecycle()
    val deviceName by engine.settings.deviceName.collectAsStateWithLifecycle()
    val now by rememberTicker(1_000)
    val current = link.current

    ScreenScaffold(stringResource(R.string.connection_title), backdrop = ui.backdrop, onBack = onBack) {
        // ---- status ------------------------------------------------------------------------
        GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
            if (current != null) {
                Text(current.peer.name, style = MaterialTheme.typography.titleLarge)
                Spacer(Modifier.height(4.dp))
                Text(
                    listOfNotNull(
                        current.rttMs?.let { stringResource(R.string.conn_rtt, it) },
                        stringResource(R.string.conn_since, durationText(now - current.attachedAtMs)),
                    ).joinToString(stringResource(R.string.list_separator)),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(12.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    for (t in current.transports) {
                        StatusChip(
                            if (t.primary) stringResource(R.string.transport_primary, transportLabel(t.kind), stringResource(R.string.conn_primary))
                            else transportLabel(t.kind),
                            icon = if (t.kind == TransportKind.BT) AppIcons.Bluetooth else AppIcons.Wifi,
                            dot = glass.good,
                        )
                    }
                }
            } else {
                Text(stringResource(R.string.conn_waiting_title), style = MaterialTheme.typography.titleLarge)
                Spacer(Modifier.height(4.dp))
                Text(stringResource(R.string.conn_waiting_text), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }

        // ---- pairing -----------------------------------------------------------------------
        SectionHeader(stringResource(R.string.pairing_header))
        GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
            val until = pairing.windowUntilMs
            val remaining = until?.let { (it - now).coerceAtLeast(0) }
            val open = remaining != null && remaining > 0
            val setPairing: (Boolean) -> Unit = { on -> if (on) engine.pairing.open() else engine.pairing.closeWindow() }
            InfoRow(
                stringResource(R.string.pairing_mode),
                subtitle = if (open) stringResource(R.string.pairing_open_for, formatTime(remaining)) else stringResource(R.string.pairing_mode_hint),
                trailing = {
                    LiquidToggle(
                        selected = { open },
                        onSelect = setPairing,
                        backdrop = ui.backdrop,
                        contentDescription = stringResource(R.string.pairing_mode),
                    )
                },
                onClick = { setPairing(!open) },
                role = Role.Switch,
            )
            if (open) {
                Spacer(Modifier.height(8.dp))
                Text(
                    stringResource(R.string.pairing_steps, deviceName),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }

        // ---- trusted devices ---------------------------------------------------------------
        SectionHeader(stringResource(R.string.trusted_header))
        val revokeTitle = stringResource(R.string.revoke_title)
        val revokeText = stringResource(R.string.revoke_text)
        val removeLabel = stringResource(R.string.trusted_revoke)
        val cancelLabel = stringResource(R.string.cancel)
        GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
            if (peers.isEmpty()) {
                Text(stringResource(R.string.trusted_empty), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            for (p in peers) {
                val connected = link.sessions.any { it.peer.id == p.id && it.connected }
                InfoRow(
                    p.name,
                    subtitle = if (connected) {
                        stringResource(R.string.trusted_connected)
                    } else {
                        stringResource(
                            R.string.trusted_last_seen,
                            if (p.lastSeenMs > 0) DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT).format(Date(p.lastSeenMs))
                            else stringResource(R.string.placeholder_none),
                        )
                    },
                    leading = { Icon(AppIcons.Computer, null, Modifier.size(24.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant) },
                    trailing = {
                        TextButton({
                            ui.confirm(
                                ConfirmRequest(revokeTitle.format(p.name), revokeText, removeLabel, cancelLabel, destructive = true) {
                                    engine.link.revoke(p.id)
                                },
                            )
                        }) { Text(removeLabel, color = glass.bad) }
                    },
                )
            }
        }

        // ---- companion device --------------------------------------------------------------
        SectionHeader(stringResource(R.string.cdm_header))
        CompanionCard(ui)

        // ---- this device -------------------------------------------------------------------
        SectionHeader(stringResource(R.string.settings_device_header))
        DeviceNameCard(ui)

        // ---- transports --------------------------------------------------------------------
        SectionHeader(stringResource(R.string.transports_header))
        GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
            val btOk = transports.bluetooth == BluetoothListenState.LISTENING
            InfoRow(
                stringResource(R.string.transport_bt_rfcomm),
                subtitle = stringResource(
                    when (transports.bluetooth) {
                        BluetoothListenState.LISTENING -> R.string.bt_listening
                        BluetoothListenState.OFF -> R.string.bt_off
                        BluetoothListenState.NO_PERMISSION -> R.string.bt_no_permission
                        BluetoothListenState.UNSUPPORTED -> R.string.bt_unsupported
                        BluetoothListenState.ERROR -> R.string.bt_error
                    },
                ),
                leading = { Icon(AppIcons.Bluetooth, null, Modifier.size(24.dp), tint = if (btOk) glass.good else glass.warn) },
                trailing = if (transports.bluetooth == BluetoothListenState.NO_PERMISSION) {
                    { TextButton({ ui.openSettings(SettingsRoute.PERMISSIONS) }) { Text(stringResource(R.string.fix)) } }
                } else {
                    null
                },
            )
            val lanOk = transports.lanAddresses.isNotEmpty() && transports.localNetworkAllowed && transports.lanPort != null
            val lanText = when {
                transports.lanPort == null -> stringResource(R.string.lan_error, transports.lanError ?: stringResource(R.string.placeholder_none))
                !transports.localNetworkAllowed -> stringResource(R.string.lan_no_permission, transports.lanPort!!)
                transports.lanAddresses.isEmpty() -> stringResource(R.string.lan_no_network, transports.lanPort!!)
                else -> {
                    val port = transports.lanPort!!
                    transports.lanAddresses.map { stringResource(R.string.lan_endpoint, it, port) }.joinToString("\n")
                }
            }
            InfoRow(
                stringResource(R.string.transport_lan_tcp),
                subtitle = lanText,
                leading = { Icon(AppIcons.Wifi, null, Modifier.size(24.dp), tint = if (lanOk) glass.good else glass.warn) },
                trailing = if (!transports.localNetworkAllowed) {
                    { TextButton({ ui.openSettings(SettingsRoute.PERMISSIONS) }) { Text(stringResource(R.string.fix)) } }
                } else {
                    null
                },
            )
            InfoRow(
                stringResource(R.string.nsd_title),
                subtitle = stringResource(
                    when (transports.nsd) {
                        NsdAdvertiser.State.REGISTERED -> R.string.nsd_registered
                        NsdAdvertiser.State.REGISTERING -> R.string.nsd_registering
                        NsdAdvertiser.State.FAILED -> R.string.nsd_failed
                        NsdAdvertiser.State.OFF -> R.string.nsd_off
                    },
                ),
            )
        }

        // ---- phone media (diagnostics) -----------------------------------------------------
        SectionHeader(stringResource(R.string.phone_title))
        PhoneMediaSection(ui)
    }
}

@Composable
private fun CompanionCard(ui: UiContext) {
    val context = LocalContext.current
    val glass = LocalGlass.current
    val helper = remember { CompanionHelper(context.applicationContext) }
    var associations by remember { mutableStateOf(helper.associations()) }
    var message by remember { mutableStateOf<String?>(null) }
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.StartIntentSenderForResult()) { result ->
        if (result.resultCode == Activity.RESULT_OK) {
            helper.observeAll()
            message = null
        }
        associations = helper.associations()
        ui.refreshPermissions()
    }
    val failedText = stringResource(R.string.cdm_failed)
    val unknownText = stringResource(R.string.placeholder_none)
    val cdmRemoveTitle = stringResource(R.string.cdm_remove_title)
    val cdmRemoveText = stringResource(R.string.cdm_remove_text)
    val cdmRemoveLabel = stringResource(R.string.cdm_remove)
    val cancelLabel = stringResource(R.string.cancel)
    GlassCard(ui.backdrop, Modifier.fillMaxWidth()) {
        Text(stringResource(R.string.cdm_text), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        if (!helper.isSupported) {
            Spacer(Modifier.height(8.dp))
            Text(stringResource(R.string.cdm_unsupported), color = glass.warn, style = MaterialTheme.typography.bodyMedium)
            return@GlassCard
        }
        for (a in associations) {
            InfoRow(
                a.name,
                subtitle = a.address,
                leading = { Icon(AppIcons.Computer, null, Modifier.size(24.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant) },
                trailing = {
                    TextButton({
                        ui.confirm(
                            ConfirmRequest(cdmRemoveTitle.format(a.name), cdmRemoveText, cdmRemoveLabel, cancelLabel, destructive = true) {
                                helper.disassociate(a)
                                associations = helper.associations()
                                ui.refreshPermissions()
                            },
                        )
                    }) { Text(cdmRemoveLabel, color = glass.bad) }
                },
            )
        }
        if (associations.isNotEmpty() && !helper.canObservePresence) {
            Text(stringResource(R.string.cdm_no_presence), style = MaterialTheme.typography.bodySmall, color = glass.warn)
        }
        Spacer(Modifier.height(16.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            GlassButton({
                helper.associate(object : CompanionHelper.Callback {
                    override fun onChooser(sender: IntentSender) {
                        launcher.launch(IntentSenderRequest.Builder(sender).build())
                    }

                    override fun onFailure(error: String?) {
                        message = failedText.format(error ?: unknownText)
                    }
                })
            }, ui.backdrop, height = 40.dp) {
                Icon(AppIcons.Link, null, Modifier.size(18.dp))
                Text(stringResource(R.string.cdm_associate))
            }
        }
        message?.let {
            Spacer(Modifier.height(8.dp))
            Text(it, color = glass.bad, style = MaterialTheme.typography.bodySmall)
        }
    }
}
