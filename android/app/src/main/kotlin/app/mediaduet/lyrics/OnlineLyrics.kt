package app.mediaduet.lyrics

import app.mediaduet.BuildConfig
import app.mediaduet.util.MdLog
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

/**
 * The phone's own online lyrics lookup, the same providers and matching rules as the Windows host
 * (`windows/platform/lyrics/LyricsService.cpp`): NetEase Cloud Music (word-timed yrc when
 * available), QQ Music and LRCLIB. Only title / artist / album / duration are sent. Results are
 * matched by [SongMatch] and never invented. Blocking: call from Dispatchers.IO.
 */
object OnlineLyrics {
    /** [text] is LRC or yrc (see [yrc]); empty [text] = the provider answered "no lyrics". */
    /**
     * [text] is LRC or yrc (see [yrc]); empty [text] = the provider answered "no lyrics".
     * [translationTiming]: the LRC [translation] is timed by when [text] is not (NetEase yrc).
     */
    data class Result(
        val text: String,
        val synced: Boolean,
        val yrc: Boolean,
        val provider: String,
        val translation: String = "",
        val translationTiming: String = "",
    )

    val DEFAULT_PROVIDERS = listOf("netease", "qqmusic", "lrclib")

    private const val TAG = "MD/Lyrics"
    private const val TIMEOUT_MS = 10_000
    private const val BROWSER_UA = "Mozilla/5.0 (Linux; Android 15) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Mobile Safari/537.36"
    private val json = Json { ignoreUnknownKeys = true }

    /** null = network / server failure (try again later); a Result with empty text = definitive "none". */
    fun query(provider: String, want: SongMatch.Candidate): Result? = try {
        when (provider) {
            "netease" -> netease(want)
            "qqmusic" -> qq(want)
            "lrclib" -> lrclib(want)
            else -> null
        }
    } catch (e: IOException) {
        MdLog.i(TAG, "$provider lookup failed: ${e.javaClass.simpleName}")
        null
    } catch (e: RuntimeException) {
        MdLog.w(TAG, "$provider lookup failed", e)
        null
    }

    // ---- HTTP ------------------------------------------------------------------------------

    private class Http(val code: Int, val body: String)

    private fun request(url: String, referer: String?, ua: String, postJson: String? = null): Http {
        val c = URL(url).openConnection() as HttpURLConnection
        try {
            c.connectTimeout = TIMEOUT_MS
            c.readTimeout = TIMEOUT_MS
            c.useCaches = false
            c.instanceFollowRedirects = true
            c.setRequestProperty("User-Agent", ua)
            c.setRequestProperty("Accept", "application/json, text/plain, */*")
            if (referer != null) c.setRequestProperty("Referer", referer)
            if (postJson != null) {
                c.requestMethod = "POST"
                c.doOutput = true
                c.setRequestProperty("Content-Type", "application/json")
                c.outputStream.use { it.write(postJson.toByteArray(Charsets.UTF_8)) }
            }
            val code = c.responseCode
            val stream = if (code in 200..299) c.inputStream else c.errorStream
            val body = stream?.use { s -> s.readBytes().let { b -> if (b.size > MAX_BODY) "" else String(b, Charsets.UTF_8) } } ?: ""
            return Http(code, body)
        } finally {
            c.disconnect()
        }
    }

    private const val MAX_BODY = 2 * 1024 * 1024

    /** GET; null on server errors, "" on 404. */
    private fun get(url: String, referer: String?, ua: String = BROWSER_UA): String? {
        val r = request(url, referer, ua)
        return when {
            r.code == 404 -> ""
            r.code in 200..299 -> r.body
            else -> null
        }
    }

    private fun post(url: String, referer: String, body: String): String? {
        val r = request(url, referer, BROWSER_UA, body)
        return if (r.code in 200..299) r.body else null
    }

    private fun enc(s: String): String = URLEncoder.encode(s, "UTF-8").replace("+", "%20")

    private fun parse(s: String?): JsonElement? = s?.let { runCatching { json.parseToJsonElement(it) }.getOrNull() }

    private fun JsonElement?.obj(key: String): JsonObject? = (this as? JsonObject)?.get(key) as? JsonObject
    private fun JsonElement?.arr(key: String): JsonArray? = (this as? JsonObject)?.get(key) as? JsonArray
    private fun JsonElement?.str(key: String): String? = ((this as? JsonObject)?.get(key) as? JsonPrimitive)?.contentOrNull
    private fun JsonElement?.num(key: String): Double? = ((this as? JsonObject)?.get(key) as? JsonPrimitive)?.doubleOrNull
    private fun JsonElement?.long(key: String): Long? = ((this as? JsonObject)?.get(key) as? JsonPrimitive)?.longOrNull
    private fun JsonElement?.bool(key: String): Boolean? = ((this as? JsonObject)?.get(key) as? JsonPrimitive)?.booleanOrNull

    private fun artists(arr: JsonArray?): String = arr.orEmpty().mapNotNull { it.str("name") }.joinToString("/")

    // ---- NetEase -----------------------------------------------------------------------------

    // ---- Catalogue lookups (also used by handoff, §8.7) -----------------------------------------

    /** [answered] false: network / server failure (try again later); [hit] null: no match. */
    data class Found<T>(val answered: Boolean, val hit: T?)

    data class QqSong(val mid: String, val id: Long)

    /** The NetEase song id of [want] (score >= 0.6). Blocking. */
    fun findNetease(want: SongMatch.Candidate): Found<Long> = try {
        val body = get("https://music.163.com/api/cloudsearch/pc?type=1&offset=0&limit=10&s=" + enc(SongMatch.searchQuery(want.title, want.artist)), "https://music.163.com/")
        if (body == null) {
            Found(false, null)
        } else {
            var best = 0.0
            var bestId = 0L
            for (s in parse(body).obj("result").arr("songs").orEmpty()) {
                val have = SongMatch.Candidate(s.str("name").orEmpty(), artists(s.arr("ar")), s.obj("al").str("name").orEmpty(), s.long("dt"))
                val score = SongMatch.score(want, have)
                if (score > best) {
                    best = score
                    bestId = s.long("id") ?: 0
                }
            }
            Found(true, bestId.takeIf { best >= 0.6 && it != 0L })
        }
    } catch (e: IOException) {
        MdLog.i(TAG, "NetEase search failed: ${e.javaClass.simpleName}")
        Found(false, null)
    }

    /**
     * The QQ Music song of [want] (score >= 0.6, or the hit whose songmid is [mid]). An empty result
     * list counts as not answered: the desktop search is rate limited per IP. Blocking.
     */
    fun findQq(want: SongMatch.Candidate, mid: String? = null): Found<QqSong> = try {
        qqSearch(want, mid)
    } catch (e: IOException) {
        MdLog.i(TAG, "QQ Music search failed: ${e.javaClass.simpleName}")
        Found(false, null)
    }

    private fun qqSearch(want: SongMatch.Candidate, wantMid: String?): Found<QqSong> {
        val ref = "https://y.qq.com/"
        data class Hit(val song: QqSong, val have: SongMatch.Candidate)
        val hits = ArrayList<Hit>()
        var transportOk = false
        val search = JsonObject(
            qqComm() + ("req_1" to buildJsonObject {
                put("method", "DoSearchForQQMusicDesktop")
                put("module", "music.search.SearchCgiService")
                putJsonObject("param") {
                    put("num_per_page", 10)
                    put("page_num", 1)
                    put("query", SongMatch.searchQuery(want.title, want.artist))
                    put("search_type", 0)
                }
            }),
        )
        post("https://u.y.qq.com/cgi-bin/musicu.fcg", ref, search.toString())?.let { body ->
            transportOk = true
            val list = parse(body).obj("req_1").obj("data").obj("body").obj("song").arr("list")
            for (s in list.orEmpty()) {
                val mid = s.str("mid") ?: continue
                hits += Hit(QqSong(mid, s.long("id") ?: 0), SongMatch.Candidate(s.str("name").orEmpty(), artists(s.arr("singer")), s.obj("album").str("name").orEmpty(), s.long("interval")?.times(1000)))
            }
        }
        if (hits.isEmpty()) {
            // The desktop search is rate limited per IP; the legacy endpoint usually still answers.
            get("https://c.y.qq.com/soso/fcgi-bin/client_search_cp?format=json&p=1&n=10&cr=1&w=" + enc(SongMatch.searchQuery(want.title, want.artist)), ref)?.let { body ->
                transportOk = true
                for (s in parse(body).obj("data").obj("song").arr("list").orEmpty()) {
                    val mid = s.str("songmid") ?: continue
                    hits += Hit(QqSong(mid, s.long("songid") ?: 0), SongMatch.Candidate(s.str("songname").orEmpty(), artists(s.arr("singer")), s.str("albumname").orEmpty(), s.long("interval")?.times(1000)))
                }
            }
        }
        if (!transportOk) return Found(false, null)
        wantMid?.let { m -> hits.firstOrNull { it.song.mid == m }?.let { return Found(true, it.song) } }
        val best = hits.maxByOrNull { SongMatch.score(want, it.have) }
        val hit = best?.takeIf { SongMatch.score(want, it.have) >= 0.6 }?.song
        return Found(hits.isNotEmpty(), hit)
    }

    private fun netease(want: SongMatch.Candidate): Result? {
        val ref = "https://music.163.com/"
        val found = findNetease(want)
        if (!found.answered) return null
        val bestId = found.hit ?: return Result("", false, false, "netease")
        val ly = get("https://music.163.com/api/song/lyric/v1?lv=1&yv=1&tv=-1&id=$bestId", ref) ?: return null
        val j = parse(ly) ?: return null
        if (j.bool("pureMusic") == true) return Result("", false, false, "netease")
        val trans = j.obj("tlyric").str("lyric").orEmpty()
        val yrc = j.obj("yrc").str("lyric").orEmpty()
        // tlyric follows the plain LRC's timestamps, which often differ from the yrc's.
        if (yrc.isNotBlank() && !LyricFormats.parseYrc(yrc).isEmpty) return Result(yrc, true, true, "netease", trans, j.obj("lrc").str("lyric").orEmpty())
        val lrc = j.obj("lrc").str("lyric").orEmpty()
        val parsed = LyricFormats.parseSynced(lrc)
        if (parsed != null && !LyricFormats.isInstrumentalNotice(parsed)) return Result(lrc, true, false, "netease", trans)
        return Result("", false, false, "netease")
    }

    // ---- QQ Music ----------------------------------------------------------------------------

    private fun qqComm() = buildJsonObject {
        putJsonObject("comm") {
            put("ct", "19")
            put("cv", "1859")
            put("uin", "0")
        }
    }

    private fun qq(want: SongMatch.Candidate): Result? {
        val ref = "https://y.qq.com/"
        val found = qqSearch(want, null)
        val best = found.hit ?: return if (found.answered) Result("", false, false, "qqmusic") else null

        var lrc = ""
        var trans = ""
        val lyricReq = JsonObject(
            qqComm() + ("req_1" to buildJsonObject {
                put("module", "music.musichallSong.PlayLyricInfo")
                put("method", "GetPlayLyricInfo")
                putJsonObject("param") {
                    put("songMID", best.mid)
                    put("songID", 0)
                    put("trans", 1)
                    put("roma", 0)
                    put("crypt", 0)
                    put("type", -1)
                }
            }),
        )
        post("https://u.y.qq.com/cgi-bin/musicu.fcg", ref, lyricReq.toString())?.let { body ->
            val data = parse(body).obj("req_1").obj("data")
            fun field(k: String): String = data.str(k)?.takeIf { it.isNotEmpty() }?.let { b64 ->
                runCatching { LyricFormats.decodeEntities(String(java.util.Base64.getDecoder().decode(b64), Charsets.UTF_8)) }.getOrNull()
            }.orEmpty()
            lrc = field("lyric")
            if (lrc.isNotEmpty()) trans = field("trans")
        }
        if (lrc.isEmpty()) {
            val ly = get("https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?format=json&nobase64=1&g_tk=5381&songmid=" + enc(best.mid), ref)
            if (ly.isNullOrEmpty()) return null
            val j = parse(ly) ?: return null
            if (j.long("retcode") != 0L) return null
            lrc = LyricFormats.decodeEntities(j.str("lyric").orEmpty())
            trans = LyricFormats.decodeEntities(j.str("trans").orEmpty())
        }
        val parsed = LyricFormats.parseSynced(lrc)
        if (parsed == null || LyricFormats.isInstrumentalNotice(parsed)) return Result("", false, false, "qqmusic")
        return Result(lrc, true, false, "qqmusic", trans)
    }

    // ---- LRCLIB ------------------------------------------------------------------------------

    private fun lrclib(want: SongMatch.Candidate): Result? {
        val ua = "MediaDuet/${BuildConfig.VERSION_NAME} (https://github.com/mediaduet)"
        fun duration(j: JsonElement): Long? = j.num("duration")?.let { (it * 1000).toLong() }
        fun pick(j: JsonElement?): Result? {
            if (j !is JsonObject || j.bool("instrumental") == true) return null
            val have = SongMatch.Candidate(j.str("trackName").orEmpty(), j.str("artistName").orEmpty(), j.str("albumName").orEmpty(), duration(j))
            if (SongMatch.score(want, have) < 0.6) return null
            j.str("syncedLyrics")?.takeIf { it.isNotBlank() }?.let { return Result(it, true, false, "lrclib") }
            j.str("plainLyrics")?.takeIf { it.isNotBlank() }?.let { return Result(it, false, false, "lrclib") }
            return null
        }
        var url = "https://lrclib.net/api/get?track_name=${enc(want.title)}&artist_name=${enc(want.artist)}"
        if (want.album.isNotEmpty()) url += "&album_name=${enc(want.album)}"
        if (want.durationMs != null) url += "&duration=${(want.durationMs + 500) / 1000}"
        val body = get(url, null, ua) ?: return null
        if (body.isNotEmpty()) pick(parse(body))?.let { return it }
        var search = "https://lrclib.net/api/search?track_name=${enc(want.title)}"
        if (want.artist.isNotEmpty()) search += "&artist_name=${enc(want.artist)}"
        val arr = parse(get(search, null, ua) ?: return Result("", false, false, "lrclib")) as? JsonArray
            ?: return Result("", false, false, "lrclib")
        var best: Result? = null
        var bestScore = 0.0
        for (j in arr) {
            val r = pick(j) ?: continue
            val have = SongMatch.Candidate(j.str("trackName").orEmpty(), j.str("artistName").orEmpty(), "", duration(j))
            val score = SongMatch.score(want, have) + if (r.synced) 0.2 else 0.0
            if (score > bestScore) {
                bestScore = score
                best = r
            }
        }
        return best ?: Result("", false, false, "lrclib")
    }
}
