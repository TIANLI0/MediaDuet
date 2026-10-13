package app.mediaduet.service

import android.Manifest
import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.PowerManager
import android.provider.Settings
import androidx.core.content.ContextCompat
import app.mediaduet.media.MediaListenerService
import app.mediaduet.transport.TransportController

/** Snapshot of everything the app depends on, re-evaluated whenever the UI resumes. */
data class PermissionSnapshot(
    val listenerAccess: Boolean,
    val notifications: Boolean,
    val notificationsRequired: Boolean,
    val bluetooth: Boolean,
    val bluetoothRequired: Boolean,
    val localNetwork: Boolean,
    val localNetworkRequired: Boolean,
    val batteryUnrestricted: Boolean,
    val backgroundRestricted: Boolean,
    val companionAssociated: Boolean,
    val companionSupported: Boolean,
)

object PermissionChecks {
    fun granted(context: Context, permission: String) =
        ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED

    fun snapshot(context: Context): PermissionSnapshot {
        val pm = context.getSystemService(PowerManager::class.java)
        val am = context.getSystemService(ActivityManager::class.java)
        val cdm = CompanionHelper(context)
        return PermissionSnapshot(
            listenerAccess = MediaListenerService.isAccessGranted(context),
            notifications = Notifications.canPost(context),
            notificationsRequired = Build.VERSION.SDK_INT >= 33,
            bluetooth = Build.VERSION.SDK_INT < 31 || granted(context, Manifest.permission.BLUETOOTH_CONNECT),
            bluetoothRequired = Build.VERSION.SDK_INT >= 31,
            localNetwork = Build.VERSION.SDK_INT < 37 || granted(context, TransportController.PERMISSION_LOCAL_NETWORK),
            localNetworkRequired = Build.VERSION.SDK_INT >= 37,
            batteryUnrestricted = pm.isIgnoringBatteryOptimizations(context.packageName),
            backgroundRestricted = am.isBackgroundRestricted,
            companionAssociated = cdm.associations().isNotEmpty(),
            companionSupported = cdm.isSupported,
        )
    }

    fun listenerSettingsIntent(context: Context): Intent =
        if (Build.VERSION.SDK_INT >= 30) {
            Intent(Settings.ACTION_NOTIFICATION_LISTENER_DETAIL_SETTINGS)
                .putExtra(Settings.EXTRA_NOTIFICATION_LISTENER_COMPONENT_NAME, MediaListenerService.component(context).flattenToString())
        } else {
            Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)
        }

    fun appDetailsIntent(context: Context): Intent =
        Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", context.packageName, null))

    fun notificationSettingsIntent(context: Context): Intent =
        Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS).putExtra(Settings.EXTRA_APP_PACKAGE, context.packageName)

    /** The battery-optimisation list; we guide the user there instead of bypassing it. */
    fun batterySettingsIntent(): Intent = Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)
}
