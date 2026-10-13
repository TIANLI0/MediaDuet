package app.mediaduet.service

import android.app.Activity
import android.os.Bundle

/**
 * Transparent, immediately finishing activity opened from the "volume control paused"
 * notification. Starting the foreground service while an activity of ours is visible (a user
 * action) grants it while-in-use capability, which Android 17 requires for background volume
 * changes. Not a trampoline: nothing else is launched.
 */
class ResumeLinkActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        LinkService.start(this, "user (volume notice)")
        Notifications.cancel(this, Notifications.ID_VOLUME)
        finish()
    }
}
