package app.mediaduet.service

import android.companion.AssociationInfo
import android.companion.CompanionDeviceService
import android.companion.DevicePresenceEvent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import androidx.annotation.RequiresApi
import app.mediaduet.util.MdLog

/** BOOT_COMPLETED / app update: connectedDevice FGS may be started from these broadcasts. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        when (intent.action) {
            Intent.ACTION_BOOT_COMPLETED, Intent.ACTION_MY_PACKAGE_REPLACED -> {
                MdLog.i("MD/Boot", "starting after ${intent.action}")
                LinkService.start(context, intent.action ?: "boot")
            }
        }
    }
}

/**
 * Bound by the system while an associated companion device (the PC) is nearby. Presence events
 * may start foreground services from the background (REQUEST_COMPANION_START_FOREGROUND_SERVICES_FROM_BACKGROUND).
 */
@RequiresApi(Build.VERSION_CODES.S)
class CompanionPresenceService : CompanionDeviceService() {

    private fun appeared(what: String) {
        MdLog.i(TAG, "companion device appeared: $what")
        LinkService.start(this, "companion")
    }

    @Deprecated("Replaced by onDeviceAppeared(AssociationInfo) on API 33")
    override fun onDeviceAppeared(address: String) = appeared(address)

    @Deprecated("Replaced by onDevicePresenceEvent on API 36")
    @RequiresApi(Build.VERSION_CODES.TIRAMISU)
    override fun onDeviceAppeared(associationInfo: AssociationInfo) = appeared("association ${associationInfo.id}")

    @Deprecated("Replaced by onDevicePresenceEvent on API 36")
    @RequiresApi(Build.VERSION_CODES.TIRAMISU)
    override fun onDeviceDisappeared(associationInfo: AssociationInfo) {
        MdLog.i(TAG, "companion device disappeared: ${associationInfo.id}")
    }

    @Deprecated("Replaced by onDeviceDisappeared(AssociationInfo) on API 33")
    override fun onDeviceDisappeared(address: String) {
        MdLog.i(TAG, "companion device disappeared: $address")
    }

    @RequiresApi(36)
    override fun onDevicePresenceEvent(event: DevicePresenceEvent) {
        when (event.event) {
            DevicePresenceEvent.EVENT_BT_CONNECTED, DevicePresenceEvent.EVENT_BLE_APPEARED,
            DevicePresenceEvent.EVENT_SELF_MANAGED_APPEARED, DevicePresenceEvent.EVENT_SELF_MANAGED_NEARBY,
            -> appeared("event ${event.event} association ${event.associationId}")
            else -> MdLog.i(TAG, "companion presence event ${event.event}")
        }
    }

    private companion object {
        const val TAG = "MD/Companion"
    }
}
