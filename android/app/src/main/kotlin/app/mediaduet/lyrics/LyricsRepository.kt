package app.mediaduet.lyrics

import app.mediaduet.media.Side
import app.mediaduet.protocol.MediaSessionInfo
import app.mediaduet.remote.LrcParser
import app.mediaduet.remote.LyricsState
import app.mediaduet.remote.TranslationMerge
import app.mediaduet.util.MdLog
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/**
 * All lyrics the UI can show, keyed by protocol track id: lyrics pushed by the PC (`lyrics.push`)
 * and the phone's own lookup (cache → online, see [OnlineLyrics]), so lyrics work for phone media
 * without a PC and for PC media the PC has no lyrics for.
 *
 * Players re-report the same song with a slightly different duration or album (QQ Music does this
 * constantly), which changes the track id. [resolve] therefore falls back to an entry for the same
 * song ([SongMatch.sameSong]), and [request] reuses it instead of looking up again: the lyrics stay
 * on screen instead of flickering away and back.
 */
class LyricsRepository(
    private val scope: CoroutineScope,
    filesDir: File,
    private val onlineEnabled: StateFlow<Boolean>,
    private val io: CoroutineDispatcher = Dispatchers.IO,
    /**
     * Lyrics the phone found itself (its cache or its own online lookup), as LRC (+ translation
     * LRC), so the PC can show them too when its own lookup came up empty (`lyrics.push`).
     */
    private val onFound: (trackId: String, lrc: String, translation: String?, source: String) -> Unit = { _, _, _, _ -> },
) {
    private val _entries = MutableStateFlow<Map<String, LyricsState>>(emptyMap())
    val entries: StateFlow<Map<String, LyricsState>> = _entries

    private val _pending = MutableStateFlow<Set<String>>(emptySet())
    /** Track ids the phone is still looking up (the UI shows "looking for lyrics"). */
    val pending: StateFlow<Set<String>> = _pending

    private val cacheDir = File(filesDir, "lyrics").apply { mkdirs() }
    private val inFlight = HashMap<String, Job>()

    /** The lyrics to show for [info]: its own entry, else one for the same song. */
    fun resolve(info: MediaSessionInfo, map: Map<String, LyricsState> = _entries.value): LyricsState? =
        resolve(info.trackId, songOf(info), map)

    /**
     * Makes sure lyrics for the shown track get found. Phone tracks are looked up right away; for
     * PC tracks the PC's push is awaited briefly ([pcConnected]) before the phone looks itself.
     */
    fun request(info: MediaSessionInfo, side: Side, pcConnected: Boolean) {
        val id = info.trackId
        if (id.isEmpty() || info.title.isBlank()) return
        val song = songOf(info)
        val existing = _entries.value[id]
        if (existing?.available == true) return
        // Same song under another id (metadata re-reported): alias it, no lookup, no flicker.
        resolve(id, song, _entries.value)?.let { alias ->
            put(alias.copy(trackId = id, song = song))
            return
        }
        if (existing != null && existing.source != SOURCE_PC_NONE) return // phone already answered
        synchronized(inFlight) {
            if (inFlight[id]?.isActive == true) return
            _pending.update { it + id }
            inFlight[id] = scope.launch {
                try {
                    if (side == Side.PC && pcConnected && existing == null) delay(PC_GRACE_MS)
                    val now = _entries.value
                    if (now[id]?.available == true || resolve(id, song, now) != null) {
                        resolve(id, song, now)?.takeIf { now[id]?.available != true }?.let { put(it.copy(trackId = id, song = song)) }
                        return@launch
                    }
                    val found = withContext(io) { lookUp(id, song) }
                    // A PC push that arrived meanwhile wins.
                    if (_entries.value[id]?.available != true) {
                        put(found)
                        if (found.available) withContext(io) { share(id, found.source) }
                    }
                } finally {
                    synchronized(inFlight) { inFlight.remove(id) }
                    _pending.update { it - id }
                }
            }
        }
    }

    /** `lyrics.push` from the PC. [song] is the PC session it belongs to, when known. */
    fun onPcLyrics(trackId: String, state: LyricsState, song: SongMatch.Candidate?) {
        val current = _entries.value[trackId]
        // Keep synced lyrics the phone found if the PC only has plain text.
        if (current?.synced == true && !state.synced) return
        put(state.copy(song = song ?: current?.song))
    }

    /** `lyrics.none` from the PC: the phone may still find some (unless it already answered). */
    fun onPcNone(trackId: String, song: SongMatch.Candidate?) {
        if (_entries.value[trackId] != null) return
        put(LyricsState(trackId, null, emptyList(), SOURCE_PC_NONE, song))
    }

    private fun put(state: LyricsState) {
        _entries.update { m ->
            val next = LinkedHashMap(m)
            next.remove(state.trackId)
            next[state.trackId] = state
            while (next.size > MAX_ENTRIES) next.remove(next.keys.first())
            next
        }
    }

    private fun resolve(trackId: String, song: SongMatch.Candidate?, map: Map<String, LyricsState>): LyricsState? {
        map[trackId]?.takeIf { it.available }?.let { return it }
        if (song == null) return null
        return map.values.lastOrNull { it.available && it.song != null && SongMatch.sameSong(it.song, song) }
    }

    // ---- lookup (IO) -------------------------------------------------------------------------

    private fun lookUp(trackId: String, song: SongMatch.Candidate?): LyricsState {
        val none = LyricsState(trackId, null, emptyList(), SOURCE_NONE, song)
        if (song == null) return none
        val stem = safe(trackId)
        val lrcFile = File(cacheDir, "$stem.lrc")
        val trFile = File(cacheDir, "$stem.tlrc")
        val missFile = File(cacheDir, "$stem.none")
        readCached(lrcFile, trFile)?.let { return it.copy(trackId = trackId, song = song) }
        if (missFile.isFile && System.currentTimeMillis() - missFile.lastModified() < NEGATIVE_TTL_MS) return none
        if (!onlineEnabled.value) return none

        var definitive = true
        var plain: LyricsState? = null
        for (provider in OnlineLyrics.DEFAULT_PROVIDERS) {
            val r = OnlineLyrics.query(provider, song)
            if (r == null) {
                definitive = false
                continue
            }
            if (r.text.isBlank()) continue
            if (!r.synced) {
                if (plain == null) plain = LyricsState(trackId, null, LrcParser.plainLines(r.text), provider, song)
                continue // prefer synced lyrics: keep looking
            }
            val base = if (r.yrc) LyricFormats.parseYrc(r.text) else LrcParser.parse(r.text)
            if (base.isEmpty) continue
            val lrc = if (r.yrc) LyricFormats.toLrc(base) else r.text
            val merged = if (r.translation.isNotBlank()) TranslationMerge.merge(base, r.translation) else base
            write(lrcFile, lrc)
            if (merged.hasTranslations) write(trFile, LyricFormats.translationLrc(merged))
            MdLog.i(TAG, "lyrics for $trackId from $provider (${merged.lines.size} lines)")
            trim()
            return LyricsState(trackId, merged, emptyList(), provider, song)
        }
        if (plain != null) {
            write(lrcFile, plain.plain.joinToString("\n"))
            trim()
            return plain
        }
        if (definitive) write(missFile, "")
        return none
    }

    /** Hands the cached files of [trackId] (only ever the phone's own finds) to [onFound]. */
    private fun share(trackId: String, source: String) {
        val stem = safe(trackId)
        val lrc = runCatching { File(cacheDir, "$stem.lrc").readText() }.getOrNull()?.takeIf { it.isNotBlank() } ?: return
        val tr = runCatching { File(cacheDir, "$stem.tlrc").takeIf { it.isFile }?.readText() }.getOrNull()?.takeIf { it.isNotBlank() }
        try {
            onFound(trackId, lrc, tr, source)
        } catch (e: Exception) {
            MdLog.w(TAG, "sharing lyrics failed", e)
        }
    }

    private fun readCached(lrcFile: File, trFile: File): LyricsState? {
        if (!lrcFile.isFile) return null
        val text = runCatching { lrcFile.readText() }.getOrNull() ?: return null
        lrcFile.setLastModified(System.currentTimeMillis()) // LRU
        val parsed = LrcParser.parse(text)
        if (parsed.isEmpty) {
            val plain = LrcParser.plainLines(text)
            return if (plain.isEmpty()) null else LyricsState("", null, plain, SOURCE_CACHE)
        }
        val tr = if (trFile.isFile) runCatching { trFile.readText() }.getOrNull() else null
        val merged = if (!tr.isNullOrBlank() && !parsed.hasTranslations) TranslationMerge.merge(parsed, tr) else parsed
        return LyricsState("", merged, emptyList(), SOURCE_CACHE)
    }

    private fun write(f: File, s: String) {
        runCatching { f.writeText(s) }.onFailure { MdLog.w(TAG, "lyrics cache write failed", it) }
    }

    private fun trim() {
        val files = cacheDir.listFiles()?.sortedByDescending { it.lastModified() } ?: return
        for (f in files.drop(MAX_CACHE_FILES)) f.delete()
    }

    private fun safe(id: String) = id.filter { it.isLetterOrDigit() }.take(64)

    companion object {
        private const val TAG = "MD/Lyrics"
        const val SOURCE_NONE = ""
        const val SOURCE_CACHE = "cache"
        /** The PC said "no lyrics"; the phone may still look. */
        const val SOURCE_PC_NONE = "pc-none"
        /** How long the phone waits for the PC's lyrics before looking itself. */
        const val PC_GRACE_MS = 2_500L
        const val NEGATIVE_TTL_MS = 72L * 3600 * 1000
        private const val MAX_ENTRIES = 24
        private const val MAX_CACHE_FILES = 600

        fun songOf(info: MediaSessionInfo): SongMatch.Candidate? =
            if (info.title.isBlank()) null else SongMatch.Candidate(info.title, info.artist, info.album, info.durationMs)
    }
}
