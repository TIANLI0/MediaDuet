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
import app.mediaduet.unlock.UnlockActivity

object Notifications {
    const val CHANNEL_LINK = "link"
    const val CHANNEL_MIRROR = "mirror"
    const val CHANNEL_PAIRING = "pairing"
    const val CHANNEL_ATTENTION = "attention"
    const val CHANNEL_HANDOFF = "handoff"
    const val CHANNEL_LYRICS = "lyrics"
    const val CHANNEL_UNLOCK = "unlock"

    const val ID_LINK = 1
    const val ID_MIRROR = 2
    const val ID_PAIRING = 3
    const val ID_VOLUME = 4
    const val ID_HANDOFF = 5
    const val ID_UPDATE = 6
    const val ID_LYRICS = 7
    const val ID_UNLOCK = 8

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
                // LOW, not MIN: Android 16 only promotes notifications (Live Updates) above MIN.
                NotificationChannel(CHANNEL_LYRICS, context.getString(R.string.channel_lyrics), NotificationManager.IMPORTANCE_LOW).apply {
                    description = context.getString(R.string.channel_lyrics_desc)
                    setShowBadge(false)
                    lockscreenVisibility = Notification.VISIBILITY_PUBLIC
                },
                NotificationChannel(CHANNEL_HANDOFF, context.getString(R.string.channel_handoff), NotificationManager.IMPORTANCE_HIGH).apply {
                    description = context.getString(R.string.channel_handoff_desc)
                    setShowBadge(false)
                },
                NotificationChannel(CHANNEL_UNLOCK, context.getString(R.string.channel_unlock), NotificationManager.IMPORTANCE_HIGH).apply {
                    description = context.getString(R.string.channel_unlock_desc)
                    setShowBadge(false)
                    lockscreenVisibility = Notification.VISIBILITY_PUBLIC
                },
            ),
        )
    }

    fun canPost(context: Context): Boolean =
        (Build.VERSION.SDK_INT < 33 ||
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) &&
            context.getSystemService(NotificationManager::class.java).areNotificationsEnabled()

    /** Android 16+: Live Updates (promoted notifications) are allowed for this app; true before 16. */
    fun canPostPromoted(context: Context): Boolean =
        Build.VERSION.SDK_INT < 36 || context.getSystemService(NotificationManager::class.java).canPostPromotedNotifications()

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

    /**
     * Handoff from the PC while MediaDuet is in the background, where Android does not let it open
     * another app: a heads-up notification that opens [open] (the music app on the song) when
     * tapped. Returns false when notifications cannot be shown.
     */
    fun showHandoff(context: Context, open: Intent, title: String, appName: String): Boolean {
        if (!canPost(context)) return false
        createChannels(context)
        val tap = PendingIntent.getActivity(context, ID_HANDOFF, open, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val text = context.getString(R.string.handoff_notification_text, appName, title)
        val n = Notification.Builder(context, CHANNEL_HANDOFF)
            .setSmallIcon(R.drawable.ic_stat_link)
            .setContentTitle(context.getString(R.string.handoff_notification_title))
            .setContentText(text)
            .setStyle(Notification.BigTextStyle().bigText(text))
            .setCategory(Notification.CATEGORY_RECOMMENDATION)
            .setAutoCancel(true)
            .setTimeoutAfter(HANDOFF_NOTICE_MS)
            .setContentIntent(tap)
            .build()
        return try {
            context.getSystemService(NotificationManager::class.java).notify(ID_HANDOFF, n)
            true
        } catch (e: SecurityException) {
            false
        }
    }

    /** A new MediaDuet version (automatic check, once per version): opens Settings > About. */
    fun showUpdate(context: Context, version: String) {
        if (!canPost(context)) return
        createChannels(context)
        val text = context.getString(R.string.update_notification_text)
        val n = Notification.Builder(context, CHANNEL_ATTENTION)
            .setSmallIcon(R.drawable.ic_stat_link)
            .setContentTitle(context.getString(R.string.update_notification_title, version))
            .setContentText(text)
            .setStyle(Notification.BigTextStyle().bigText(text))
            .setCategory(Notification.CATEGORY_RECOMMENDATION)
            .setAutoCancel(true)
            .setContentIntent(openAppIntent(context, "about"))
            .build()
        try {
            context.getSystemService(NotificationManager::class.java).notify(ID_UPDATE, n)
        } catch (e: SecurityException) {
            // Permission revoked between the check and the call.
        }
    }

    /** The update is ready but MediaDuet went to the background: tap to confirm the installation. */
    fun showUpdateConfirm(context: Context, confirm: Intent, version: String) {
        if (!canPost(context)) return
        createChannels(context)
        val tap = PendingIntent.getActivity(context, ID_UPDATE, confirm, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val n = Notification.Builder(context, CHANNEL_HANDOFF)
            .setSmallIcon(R.drawable.ic_stat_link)
            .setContentTitle(context.getString(R.string.update_confirm_title, version))
            .setContentText(context.getString(R.string.update_confirm_text))
            .setCategory(Notification.CATEGORY_STATUS)
            .setAutoCancel(true)
            .setContentIntent(tap)
            .build()
        try {
            context.getSystemService(NotificationManager::class.java).notify(ID_UPDATE, n)
        } catch (e: SecurityException) {
            // Permission revoked between the check and the call.
        }
    }

    /** Android 14+ asks the user to allow full-screen notifications; granted at install before. */
    fun canUseFullScreen(context: Context): Boolean =
        Build.VERSION.SDK_INT < 34 || context.getSystemService(NotificationManager::class.java).canUseFullScreenIntent()

    /**
     * A PC asks to be unlocked (or to set that up, [enroll]): a heads-up notification that opens
     * [UnlockActivity] (the system's fingerprint prompt). With the full-screen permission the
     * activity also comes up by itself on a locked or dark phone. Stays until the request ends.
     */
    fun showUnlock(context: Context, pcName: String, enroll: Boolean, timeoutMs: Long) {
        if (!canPost(context)) return
        createChannels(context)
        val tap = PendingIntent.getActivity(
            context, ID_UNLOCK, UnlockActivity.intent(context), PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val text = context.getString(if (enroll) R.string.unlock_enroll_notification_text else R.string.unlock_notification_text, pcName)
        val n = Notification.Builder(context, CHANNEL_UNLOCK)
            .setSmallIcon(R.drawable.ic_stat_link)
            .setContentTitle(context.getString(if (enroll) R.string.unlock_enroll_notification_title else R.string.unlock_notification_title, pcName))
            .setContentText(text)
            .setStyle(Notification.BigTextStyle().bigText(text))
            .setCategory(Notification.CATEGORY_STATUS)
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .setOngoing(true)
            .setTimeoutAfter(timeoutMs)
            .setContentIntent(tap)
            .apply { if (canUseFullScreen(context)) setFullScreenIntent(tap, true) }
            .build()
        try {
            context.getSystemService(NotificationManager::class.java).notify(ID_UNLOCK, n)
        } catch (e: SecurityException) {
            // Permission revoked between the check and the call.
        }
    }

    /** How long a handoff notification stays (the PC waits as long for the tap). */
    const val HANDOFF_NOTICE_MS = 90_000L

    fun cancel(context: Context, id: Int) {
        context.getSystemService(NotificationManager::class.java).cancel(id)
    }
}
