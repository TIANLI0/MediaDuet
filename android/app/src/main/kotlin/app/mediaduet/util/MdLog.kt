package app.mediaduet.util

/**
 * Minimal logging facade so that protocol / link code stays JVM-testable (android.util.Log is a
 * stub in local unit tests). The Android application installs a sink that forwards to logcat.
 */
object MdLog {
    enum class Level { DEBUG, INFO, WARN, ERROR }

    fun interface Sink {
        fun log(level: Level, tag: String, message: String, error: Throwable?)
    }

    @Volatile
    var sink: Sink = Sink { level, tag, message, error ->
        println("${level.name.first()}/$tag: $message" + (error?.let { " ($it)" } ?: ""))
    }

    fun d(tag: String, message: String) = sink.log(Level.DEBUG, tag, message, null)
    fun i(tag: String, message: String) = sink.log(Level.INFO, tag, message, null)
    fun w(tag: String, message: String, error: Throwable? = null) = sink.log(Level.WARN, tag, message, error)
    fun e(tag: String, message: String, error: Throwable? = null) = sink.log(Level.ERROR, tag, message, error)
}

/** Monotonic millisecond clock. Android uses SystemClock.elapsedRealtime (counts deep sleep). */
fun interface MonoClock {
    fun nowMs(): Long

    companion object {
        val SYSTEM_NANO = MonoClock { System.nanoTime() / 1_000_000L }
    }
}
