package app.mediaduet.util

import java.io.File
import java.io.PrintWriter
import java.io.StringWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Local crash capture (no upload): keeps the last [ringSize] log lines in memory and, on an
 * uncaught exception, writes the stack trace plus those lines to `dir/crash-<millis>.txt`, keeping
 * the newest [maxFiles] reports. The UI shows an unseen report once (Settings › Diagnostics).
 * Pure JVM code (unit-tested with a temp directory).
 */
class CrashLog(
    private val dir: File,
    private val wallClock: () -> Long = System::currentTimeMillis,
    private val maxFiles: Int = 5,
    private val ringSize: Int = 200,
    /** Extra lines at the top of a report (app version, device, OS). */
    private val header: () -> String = { "" },
) {
    private val ring = ArrayDeque<String>(ringSize)
    private val seenFile = File(dir, "seen")

    @Synchronized
    fun log(level: MdLog.Level, tag: String, message: String, error: Throwable?) {
        if (ring.size >= ringSize) ring.removeFirst()
        val line = "${stamp(wallClock())} ${level.name.first()}/$tag: $message" + (error?.let { " ($it)" } ?: "")
        ring.addLast(line)
    }

    @Synchronized
    fun recentLines(): List<String> = ring.toList()

    /** A caught failure: kept in the log ring with its stack trace, no report file. */
    fun recordNonFatal(what: String, e: Throwable) = log(MdLog.Level.ERROR, "MD/Crash", "non-fatal $what: ${stackTrace(e)}", null)

    /** Writes a report for a fatal [e] on [thread]; returns the file (null if it could not be written). */
    fun recordCrash(thread: String, e: Throwable): File? = try {
        dir.mkdirs()
        val now = wallClock()
        var file = File(dir, "crash-$now.txt")
        var n = 1
        while (file.exists()) file = File(dir, "crash-$now-${n++}.txt")
        file.writeText(header() + format(now, thread, e, recentLines()))
        prune()
        file
    } catch (t: Throwable) {
        null
    }

    /** Reports, newest first. */
    fun reports(): List<File> =
        dir.listFiles { f -> f.isFile && f.name.startsWith("crash-") && f.name.endsWith(".txt") }
            .orEmpty()
            .sortedWith(compareByDescending<File> { timeOf(it) }.thenByDescending { it.name })

    /** The newest report the user has not dismissed yet. */
    fun latestUnseen(): File? {
        val latest = reports().firstOrNull() ?: return null
        val seen = runCatching { seenFile.readText().trim() }.getOrNull()
        return latest.takeIf { it.name != seen }
    }

    fun markSeen() {
        val latest = reports().firstOrNull() ?: return
        runCatching {
            dir.mkdirs()
            seenFile.writeText(latest.name)
        }
    }

    private fun prune() {
        reports().drop(maxFiles).forEach { it.delete() }
    }

    companion object {
        @Volatile var instance: CrashLog? = null

        /** Report time from the file name (crash-<millis>[-n].txt). */
        fun timeOf(f: File): Long = f.name.removePrefix("crash-").removeSuffix(".txt").substringBefore('-').toLongOrNull() ?: 0L

        fun stackTrace(e: Throwable): String = StringWriter().also { e.printStackTrace(PrintWriter(it)) }.toString()

        fun format(atMs: Long, thread: String, e: Throwable, lines: List<String>): String = buildString {
            append("MediaDuet crash report\n")
            append("time: ").append(stamp(atMs)).append('\n')
            append("thread: ").append(thread).append('\n')
            append('\n').append(stackTrace(e)).append('\n')
            append("--- last ").append(lines.size).append(" log lines ---\n")
            lines.forEach { append(it).append('\n') }
        }

        private fun stamp(ms: Long): String = SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.ROOT).format(Date(ms))
    }
}
