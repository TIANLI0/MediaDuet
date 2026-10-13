package app.mediaduet.ui.screens

import android.text.format.Formatter
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import app.mediaduet.R
import app.mediaduet.link.TransportKind
import app.mediaduet.protocol.Action
import app.mediaduet.protocol.PlayStatus

/** Localised labels for protocol values and units shown in the UI. */

@Composable
fun bytesText(bytes: Long): String = Formatter.formatShortFileSize(LocalContext.current, bytes)

@Composable
fun durationText(ms: Long): String {
    val s = (ms / 1000).coerceAtLeast(0)
    return when {
        s >= 86_400 -> stringResource(R.string.duration_days_hours, s / 86_400, (s % 86_400) / 3600)
        s >= 3600 -> stringResource(R.string.duration_hours_minutes, s / 3600, (s % 3600) / 60)
        s >= 60 -> stringResource(R.string.duration_minutes_seconds, s / 60, s % 60)
        else -> stringResource(R.string.duration_seconds, s)
    }
}

@Composable
fun transportLabel(kind: TransportKind): String =
    stringResource(if (kind == TransportKind.BT) R.string.transport_bt else R.string.transport_lan)

@Composable
fun actionLabel(action: String): String = when (action) {
    Action.PLAY -> stringResource(R.string.action_play)
    Action.PAUSE -> stringResource(R.string.action_pause)
    Action.TOGGLE -> stringResource(R.string.action_toggle)
    Action.NEXT -> stringResource(R.string.action_next)
    Action.PREVIOUS -> stringResource(R.string.action_previous)
    Action.SEEK -> stringResource(R.string.action_seek)
    Action.STOP -> stringResource(R.string.action_stop)
    else -> action
}

/** Pairing failure reasons (§3.3 pair.result reasons plus local outcomes). */
@Composable
fun pairingReasonLabel(reason: String?): String = when (reason) {
    "rejected" -> stringResource(R.string.reason_rejected)
    "timeout" -> stringResource(R.string.reason_timeout)
    "mac_mismatch" -> stringResource(R.string.reason_mac_mismatch)
    "pairing_disabled" -> stringResource(R.string.reason_pairing_disabled)
    "busy" -> stringResource(R.string.reason_busy)
    "connection_lost" -> stringResource(R.string.reason_connection_lost)
    "peer_error" -> stringResource(R.string.reason_peer_error)
    else -> stringResource(R.string.reason_other, reason ?: stringResource(R.string.placeholder_none))
}

/** States reported in policy.status; unknown values from newer peers are shown verbatim. */
@Composable
fun policyStateLabel(state: String): String = when (state) {
    "playing" -> stringResource(R.string.status_playing)
    "paused" -> stringResource(R.string.status_paused)
    "stopped", "idle" -> stringResource(R.string.status_stopped)
    "policy_paused" -> stringResource(R.string.policy_state_policy_paused)
    "user_paused" -> stringResource(R.string.policy_state_user_paused)
    else -> state
}

@Composable
fun policyWhatLabel(what: String): String = when (what) {
    "pause_remote" -> stringResource(R.string.policy_what_pause_remote)
    "resume_remote" -> stringResource(R.string.policy_what_resume_remote)
    "pause_local" -> stringResource(R.string.policy_what_pause_local)
    "resume_local" -> stringResource(R.string.policy_what_resume_local)
    else -> what
}

@Composable
fun statusLabel(status: String): String = stringResource(
    when (status) {
        PlayStatus.PLAYING -> R.string.status_playing
        PlayStatus.PAUSED -> R.string.status_paused
        PlayStatus.STOPPED -> R.string.status_stopped
        PlayStatus.BUFFERING -> R.string.status_buffering
        else -> R.string.status_unknown
    },
)

@Composable
fun policyModeLabel(mode: String): String = stringResource(
    when (mode) {
        "off" -> R.string.policy_mode_off
        "windows_first" -> R.string.policy_mode_windows_first
        "android_first" -> R.string.policy_mode_android_first
        "latest" -> R.string.policy_mode_latest
        "custom" -> R.string.policy_mode_custom
        else -> R.string.status_unknown
    },
)
