package app.mediaduet.media

import android.content.ComponentName
import android.content.Context
import android.service.notification.NotificationListenerService
import androidx.core.app.NotificationManagerCompat
import app.mediaduet.service.Engine
import app.mediaduet.service.LinkService
import app.mediaduet.util.MdLog

/**
 * Exists only so that MediaSessionManager.getActiveSessions() is permitted and so that the system
 * keeps our process bound (and therefore alive) while access is granted.
 *
 * Notification contents are never read, stored or forwarded: onNotificationPosted/Removed are not
 * overridden (the base implementations do nothing).
 */
class MediaListenerService : NotificationListenerService() {

    override fun onListenerConnected() {
        super.onListenerConnected()
        MdLog.i(TAG, "notification listener connected")
        val engine = Engine.get(this)
        engine.onListenerConnected()
        // Best effort: upgrade to a foreground service; if the platform refuses (background start
        // restrictions), the engine keeps running inside this system-bound process.
        LinkService.start(this, "listener")
    }

    override fun onListenerDisconnected() {
        super.onListenerDisconnected()
        MdLog.w(TAG, "notification listener disconnected")
        Engine.get(this).onListenerDisconnected()
        // Ask the system to bind us again if access is still granted.
        if (isAccessGranted(this)) runCatching { requestRebind(component(this)) }
    }

    companion object {
        private const val TAG = "MD/Listener"

        fun component(context: Context) = ComponentName(context, MediaListenerService::class.java)

        fun isAccessGranted(context: Context): Boolean =
            NotificationManagerCompat.getEnabledListenerPackages(context).contains(context.packageName)
    }
}
