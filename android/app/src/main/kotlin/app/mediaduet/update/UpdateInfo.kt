package app.mediaduet.update

import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.longOrNull

/**
 * Software updates from the project's GitHub releases, the pure parts: the "latest release"
 * answer, version order, the release's file names and checksums. Mirrors the host's
 * `windows/core/include/md/update/Update.h`.
 */
object UpdateInfo {
    const val LATEST_RELEASE_URL = "https://api.github.com/repos/TIANLI0/MediaDuet/releases/latest"
    const val RELEASES_PAGE = "https://github.com/TIANLI0/MediaDuet/releases/latest"
    const val CHECKSUMS_ASSET = "SHA256SUMS.txt"

    data class Asset(val name: String, val url: String, val size: Long)

    data class Release(val tag: String, val version: String, val page: String, val notes: String, val assets: List<Asset>) {
        fun find(name: String): Asset? = assets.firstOrNull { it.name == name }
    }

    private fun JsonObject.str(k: String) = (this[k] as? JsonPrimitive)?.takeIf { it.isString }?.contentOrNull

    /** The GitHub API's release object; null for drafts, prereleases, unreadable versions. https links only. */
    fun parseRelease(o: JsonObject?): Release? {
        if (o == null) return null
        if ((o["draft"] as? JsonPrimitive)?.booleanOrNull == true || (o["prerelease"] as? JsonPrimitive)?.booleanOrNull == true) return null
        val tag = o.str("tag_name")?.take(64) ?: return null
        val v = parse(tag) ?: return null
        val page = o.str("html_url")?.takeIf { it.startsWith("https://") } ?: RELEASES_PAGE
        val assets = (o["assets"] as? JsonArray).orEmpty().mapNotNull { e ->
            val a = e as? JsonObject ?: return@mapNotNull null
            val name = a.str("name")?.takeIf { it.isNotEmpty() } ?: return@mapNotNull null
            val url = a.str("browser_download_url")?.takeIf { it.startsWith("https://") } ?: return@mapNotNull null
            Asset(name, url, (a["size"] as? JsonPrimitive)?.longOrNull ?: 0)
        }
        return Release(tag, "${v[0]}.${v[1]}.${v[2]}", page, o.str("body")?.take(4000).orEmpty(), assets)
    }

    /** major.minor.patch; a leading "v" and any "-suffix" / "+build" / " (debug)" are ignored. */
    fun parse(v: String): IntArray? {
        val m = Regex("""^\s*[vV]?(\d{1,6})\.(\d{1,6})(?:\.(\d{1,6}))?""").find(v) ?: return null
        return intArrayOf(m.groupValues[1].toInt(), m.groupValues[2].toInt(), m.groupValues[3].ifEmpty { "0" }.toInt())
    }

    /** True when [latest] is a higher version than [current]. */
    fun isNewer(latest: String, current: String): Boolean {
        val a = parse(latest) ?: return false
        val b = parse(current) ?: return false
        for (i in 0..2) if (a[i] != b[i]) return a[i] > b[i]
        return false
    }

    fun androidAsset(version: String) = "MediaDuet-$version-android.apk"

    /** The lowercase SHA-256 of [file] in a `sha256sum` listing ("<hex>  <name>" or "<hex> *<name>"). */
    fun checksumFor(sums: String, file: String): String? {
        for (raw in sums.lineSequence()) {
            val line = raw.trim()
            if (line.length < 66) continue
            val hex = line.substring(0, 64).lowercase()
            if (!hex.all { it in '0'..'9' || it in 'a'..'f' } || !line[64].isWhitespace()) continue
            if (line.substring(65).trim().removePrefix("*") == file) return hex
        }
        return null
    }
}
