package app.mediaduet.service

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat
import app.mediaduet.R
import app.mediaduet.ui.MainActivity

object Notifications {
    const val CHANNEL_LINK = "link"
    const val CHANNEL_MIRROR = "mirror"
    const val CHANNEL_PAIRING = "pairing"
    const val CHANNEL_ATTENTION = "attention"

    const val ID_LINK = 1
    const val ID_MIRROR = 2
    const val ID_PAIRING = 3
    const val ID_VOLUME = 4

    /** At most one "volume control restricted" notification per this interval. */
    private const val VOLUME_NOTICE_INTERVAL_MS = 10 * 60_000L
    @Volatile private var lastVolumeNoticeMs = Long.MIN_VALUE / 2

    fun createChannels(context: Context) {
        val nm = context.getSystemService(NotificationManager::class.java)
        nm.createNotificationChannels(
            listOf(
                NotificationChannel(CHANNEL_LINK, context.getString(R.string.channel_link), NotificationManager.IMPORTANCE_MIN).apply {
                    description = context.getString(R.string.channel_link_desc)
                    setShowBadge(false)
                },
                NotificationChannel(CHANNEL_MIRROR, context.getString(R.string.channel_mirror), NotificationManager.IMPORTANCE_LOW).apply {
                    description = context.getString(R.string.channel_mirror_desc)
                    setShowBadge(false)
                },
                NotificationChannel(CHANNEL_PAIRING, context.getString(R.string.channel_pairing), NotificationManager.IMPORTANCE_HIGH).apply {
                    description = context.getString(R.string.channel_pairing_desc)
                },
                NotificationChannel(CHANNEL_ATTENTION, context.getString(R.string.channel_attention), NotificationManager.IMPORTANCE_LOW).apply {
                    description = context.getString(R.string.channel_attention_desc)
                    setShowBadge(false)
                },
            ),
        )
    }

    fun canPost(context: Context): Boolean =
        (Build.VERSION.SDK_INT < 33 ||
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) &&
            context.getSystemService(NotificationManager::class.java).areNotificationsEnabled()

    fun openAppIntent(context: Context, tab: String? = null): PendingIntent {
        val intent = Intent(context, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
        if (tab != null) intent.putExtra(MainActivity.EXTRA_TAB, tab)
        return PendingIntent.getActivity(context, tab.hashCode(), intent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
    }

    fun linkNotification(context: Context, text: String): Notification =
        Notification.Builder(context, CHANNEL_LINK)
            .setSmallIcon(R.drawable.ic_stat_link)
            .setContentTitle(context.getString(R.string.app_name))
            .setContentText(text)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .setCategory(Notification.CATEGORY_SERVICE)
            .setContentIntent(openAppIntent(context, "connection"))
            .apply { if (Build.VERSION.SDK_INT >= 31) setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE) }
            .build()

    fun showPairingRequest(context: Context, peerName: String, sas: String) {
        if (!canPost(context)) return
        val n = Notification.Builder(context, CHANNEL_PAIRING)
            .setSmallIcon(R.drawable.ic_stat_link)
            .setContentTitle(context.getString(R.string.pairing_notification_title, peerName))
            .setContentText(context.getString(R.string.pairing_notification_text, sas))
            .setCategory(Notification.CATEGORY_STATUS)
            .setAutoCancel(true)
            .setContentIntent(openAppIntent(context, "connection"))
            .build()
        try {
            context.getSystemService(NotificationManager::class.java).notify(ID_PAIRING, n)
        } catch (e: SecurityException) {
            // Permission revoked between the check and the call: nothing to show.
        }
    }

    /**
     * Quiet, rate-limited notice that Android ignored a volume change from the PC because the app
     * is in the background. Tapping it opens [ResumeLinkActivity] for an instant, which restarts the
     * connection service from a user action so that it may control the volume again.
     */
    fun showVolumeRestricted(context: Context) {
        val now = android.os.SystemClock.elapsedRealtime()
        if (now - lastVolumeNoticeMs < VOLUME_NOTICE_INTERVAL_MS || !canPost(context)) return
        lastVolumeNoticeMs = now
        createChannels(context)
        val tap = PendingIntent.getActivity(
            context, ID_VOLUME,
            Intent(context, ResumeLinkActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val text = context.getString(R.string.volume_restricted_text)
        val n = Notification.Builder(context, CHANNEL_ATTENTION)
            .setSmallIcon(R.drawable.ic_stat_link)
            .setContentTitle(context.getString(R.string.volume_restricted_title))
            .setContentText(text)
            .setStyle(Notification.BigTextStyle().bigText(text))
            .setCategory(Notification.CATEGORY_STATUS)
            .setOnlyAlertOnce(true)
            .setAutoCancel(true)
            .setContentIntent(tap)
            .build()
        try {
            context.getSystemService(NotificationManager::class.java).notify(ID_VOLUME, n)
        } catch (e: SecurityException) {
            // Permission revoked between the check and the call.
        }
    }

    fun cancel(context: Context, id: Int) {
        context.getSystemService(NotificationManager::class.java).cancel(id)
    }
}
