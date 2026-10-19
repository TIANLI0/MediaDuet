package app.mediaduet.update

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.os.Build
import app.mediaduet.service.Engine

/** Status of the update's [PackageInstaller] session (see [Updater.install]). Not exported. */
class UpdateInstallReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val updater = Engine.get(context).updater
        when (val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                val confirm = if (Build.VERSION.SDK_INT >= 33) {
                    intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableExtra(Intent.EXTRA_INTENT)
                }
                if (confirm != null) updater.onConfirm(confirm) else updater.onFailed(PackageInstaller.STATUS_FAILURE, "no confirmation")
            }
            // Android replaces (and restarts) the app; nothing left to do here.
            PackageInstaller.STATUS_SUCCESS -> Unit
            else -> updater.onFailed(status, intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE))
        }
    }
}
