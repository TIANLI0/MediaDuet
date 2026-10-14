package app.mediaduet.util

/**
 * Compares this app's version with the paired device's (`DeviceInfo.app`), the same rule as the
 * Windows host: "major.minor.patch", any `-suffix` / `+build` ignored. Different major or minor
 * versions may not understand each other's newer features (warn); a different patch is shown
 * quietly; an unknown version is shown as unknown without a warning.
 */
object VersionCompat {
    enum class Level { SAME, PATCH, MISMATCH, UNKNOWN }

    fun parse(v: String): IntArray? {
        val core = v.trim().removePrefix("v").substringBefore('-').substringBefore('+').substringBefore(' ')
        val parts = core.split('.')
        if (parts.isEmpty() || parts.size > 4) return null
        val nums = parts.map { it.toIntOrNull() ?: return null }
        return IntArray(3) { nums.getOrElse(it) { 0 } }
    }

    fun compare(local: String, peer: String): Level {
        val a = parse(local) ?: return Level.UNKNOWN
        val b = parse(peer) ?: return Level.UNKNOWN
        return when {
            a[0] != b[0] || a[1] != b[1] -> Level.MISMATCH
            a[2] != b[2] -> Level.PATCH
            else -> Level.SAME
        }
    }
}
