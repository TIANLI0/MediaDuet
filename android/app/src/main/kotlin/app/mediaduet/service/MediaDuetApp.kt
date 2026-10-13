package app.mediaduet.service

import android.app.Application
import android.content.Context
import android.os.Build
import android.util.Log
import app.mediaduet.BuildConfig
import app.mediaduet.util.CrashLog
import app.mediaduet.util.MdLog
import java.io.File

class MediaDuetApp : Application() {
    override fun attachBaseContext(base: Context) {
        super.attachBaseContext(AppLanguage.wrap(base))
    }

    override fun onCreate() {
        super.onCreate()
        val crashes = CrashLog(File(filesDir, "crash"), header = {
            "app ${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE}) · ${Build.MANUFACTURER} ${Build.MODEL} · Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})" + "\n"
        })
        CrashLog.instance = crashes
        // Uncaught exceptions: write a local report (stack + last log lines), then let the
        // platform handle the crash as usual (the system restarts the sticky service).
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, e ->
            try {
                crashes.recordCrash(thread.name, e)
            } finally {
                previous?.uncaughtException(thread, e)
            }
        }
        MdLog.sink = MdLog.Sink { level, tag, message, error ->
            crashes.log(level, tag, message, error)
            when (level) {
                MdLog.Level.DEBUG -> Log.d(tag, message, error)
                MdLog.Level.INFO -> Log.i(tag, message, error)
                MdLog.Level.WARN -> Log.w(tag, message, error)
                MdLog.Level.ERROR -> Log.e(tag, message, error)
            }
        }
        // The engine object is owned by the process; components start it on demand.
        Engine.get(this)
    }
}
