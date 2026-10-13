package app.mediaduet.remote

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import app.mediaduet.crypto.sha256
import app.mediaduet.link.LinkManager
import app.mediaduet.protocol.Action
import app.mediaduet.protocol.ArtworkRef
import app.mediaduet.protocol.BlobChunk
import app.mediaduet.protocol.CommandResult
import app.mediaduet.protocol.ErrorCode
import app.mediaduet.protocol.Hex
import app.mediaduet.protocol.LyricsPush
import app.mediaduet.protocol.MediaCommand
import app.mediaduet.protocol.MediaSessionInfo
import app.mediaduet.protocol.MediaSnapshot
import app.mediaduet.protocol.PolicyStatus
import app.mediaduet.protocol.VolumeState
import app.mediaduet.util.MdLog
import app.mediaduet.util.MonoClock
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.io.File

data class RemoteState(
    val peerId: String,
    val snapshot: MediaSnapshot,
    /** Monotonic time the snapshot was received. */
    val receivedAtMs: Long,
    val rttMs: Long?,
    val connected: Boolean,
) {
    fun session(key: String?): MediaSessionInfo? =
        snapshot.sessions.firstOrNull { it.key == key } ?: snapshot.activeSession
}

/** [lyrics] = synced lines; [plain] = unsynced text lines; both empty when the peer found nothing. */
data class LyricsState(val trackId: String, val lyrics: Lyrics?, val plain: List<String>, val source: String) {
    val synced: Boolean get() = lyrics != null && !lyrics.isEmpty
    val available: Boolean get() = synced || plain.isNotEmpty()
}

/**
 * The Windows side as seen from the phone: the latest peer snapshot (with local position
 * interpolation), content-addressed artwork fetched via `artwork.get`, lyrics and policy status.
 */
class RemoteMediaStore(
    private val scope: CoroutineScope,
    private val clock: MonoClock,
    private val link: LinkManager,
    cacheDir: File,
) {
    private val _state = MutableStateFlow<RemoteState?>(null)
    val state: StateFlow<RemoteState?> = _state

    /** Session chosen in the UI switcher (null = follow the peer's active session). */
    private val _selectedKey = MutableStateFlow<String?>(null)
    val selectedKey: StateFlow<String?> = _selectedKey

    private val _artwork = MutableStateFlow<Map<String, Bitmap>>(emptyMap())
    val artwork: StateFlow<Map<String, Bitmap>> = _artwork

    private val _lyrics = MutableStateFlow<Map<String, LyricsState>>(emptyMap())
    val lyrics: StateFlow<Map<String, LyricsState>> = _lyrics

    private val _policy = MutableStateFlow<PolicyStatus?>(null)
    val policy: StateFlow<PolicyStatus?> = _policy

    /** The PC's media volume (`volume.state`), null until reported. */
    private val _volume = MutableStateFlow<VolumeState?>(null)
    val volume: StateFlow<VolumeState?> = _volume

    fun onPeerVolume(state: VolumeState) {
        _volume.value = state
    }

    private val artDir = File(cacheDir, "remote-artwork").apply { mkdirs() }

    private class Assembly(val total: Int, val startedMs: Long) {
        val data = ByteArray(total)
        val seen = java.util.BitSet()
        var received = 0
    }

    private val assemblies = HashMap<String, Assembly>()
    private val requestedAt = HashMap<String, Long>()
    private val missing = HashSet<String>()

    fun onSessionStarted(peerId: String, resumed: Boolean) {
        val current = _state.value
        if (!resumed || current?.peerId != peerId) {
            // §7: a new session discards peer state; the peer re-sends a full snapshot.
            _state.value = null
            _policy.value = null
            _volume.value = null
            synchronized(this) { assemblies.clear(); requestedAt.clear(); missing.clear() }
        } else {
            _state.value = current.copy(connected = true)
            fetchArtworkFor(current)
        }
    }

    fun onDisconnected(peerId: String) {
        _state.update { if (it?.peerId == peerId) it.copy(connected = false) else it }
    }

    fun onSnapshot(peerId: String, snapshot: MediaSnapshot, rttMs: Long?) {
        val st = RemoteState(peerId, snapshot, clock.nowMs(), rttMs, connected = true)
        _state.value = st
        if (_selectedKey.value != null && snapshot.sessions.none { it.key == _selectedKey.value }) _selectedKey.value = null
        fetchArtworkFor(st)
        pruneArtwork(snapshot)
    }

    fun select(key: String?) {
        _selectedKey.value = key
    }

    /** The session the UI shows. */
    fun focused(): MediaSessionInfo? = _state.value?.session(_selectedKey.value)

    fun positionNow(session: MediaSessionInfo): Long? {
        val st = _state.value ?: return null
        return RemotePosition.at(session, st.receivedAtMs, st.rttMs, clock.nowMs())
    }

    private fun fetchArtworkFor(st: RemoteState) {
        val wanted = st.snapshot.sessions.mapNotNull { it.artwork }.distinctBy { it.hash }
        for (ref in wanted) ensureArtwork(st.peerId, ref)
    }

    private fun ensureArtwork(peerId: String, ref: ArtworkRef) {
        if (_artwork.value.containsKey(ref.hash)) return
        val file = File(artDir, "${ref.hash}.jpg")
        if (file.isFile) {
            scope.launch(Dispatchers.IO) { decodeAndPublish(ref.hash, file.readBytes()) }
            return
        }
        val now = clock.nowMs()
        synchronized(this) {
            if (ref.hash in missing) return
            val last = requestedAt[ref.hash]
            if (last != null && now - last < REQUEST_RETRY_MS) return
            requestedAt[ref.hash] = now
        }
        link.requestArtwork(peerId, ref)
    }

    fun onArtworkNotFound(hash: String) {
        synchronized(this) {
            missing += hash
            assemblies.remove(hash)
        }
    }

    /** Reassembles BLOB chunks (§8.6); verifies SHA-256 before caching. */
    fun onChunk(chunk: BlobChunk) {
        val hash = Hex.encode(chunk.sha256)
        val complete: ByteArray? = synchronized(this) {
            if (_artwork.value.containsKey(hash)) return
            val a = assemblies.getOrPut(hash) { Assembly(chunk.total, clock.nowMs()) }
            if (a.total != chunk.total) {
                assemblies.remove(hash)
                return
            }
            val index = chunk.offset / app.mediaduet.protocol.Limits.MAX_BLOB_PAYLOAD
            if (!a.seen.get(index)) {
                a.seen.set(index)
                System.arraycopy(chunk.bytes, 0, a.data, chunk.offset, chunk.bytes.size)
                a.received += chunk.bytes.size
            }
            if (a.received >= a.total) {
                assemblies.remove(hash)
                a.data
            } else {
                // Drop stale partial transfers.
                val now = clock.nowMs()
                assemblies.entries.removeAll { now - it.value.startedMs > REQUEST_RETRY_MS }
                null
            }
        }
        if (complete != null) {
            if (Hex.encode(sha256(complete)) != hash) {
                MdLog.w(TAG, "artwork hash mismatch, discarding")
                return
            }
            scope.launch(Dispatchers.IO) {
                runCatching { File(artDir, "$hash.jpg").writeBytes(complete) }
                trimDiskCache()
                decodeAndPublish(hash, complete)
            }
        }
    }

    private fun decodeAndPublish(hash: String, bytes: ByteArray) {
        val bmp = BitmapFactory.decodeByteArray(bytes, 0, bytes.size) ?: return
        _artwork.update { it + (hash to bmp) }
    }

    private fun pruneArtwork(snapshot: MediaSnapshot) {
        val keep = snapshot.sessions.mapNotNull { it.artwork?.hash }.toSet()
        _artwork.update { m -> if (m.keys.all { it in keep }) m else m.filterKeys { it in keep } }
    }

    private fun trimDiskCache() {
        val files = artDir.listFiles()?.sortedByDescending { it.lastModified() } ?: return
        var total = 0L
        for (f in files) {
            total += f.length()
            if (total > DISK_CACHE_BYTES) f.delete()
        }
    }

    fun onLyrics(push: LyricsPush) {
        val parsed = if (push.format == "lrc") {
            LrcParser.parse(push.text).let { l -> push.translation?.let { TranslationMerge.merge(l, it) } ?: l }
        } else null
        val plain = if (parsed == null || parsed.isEmpty) LrcParser.plainLines(push.text) else emptyList()
        _lyrics.update { m -> trim(m + (push.trackId to LyricsState(push.trackId, parsed?.takeIf { !it.isEmpty }, plain, push.source))) }
    }

    fun onLyricsNone(trackId: String) {
        _lyrics.update { m -> trim(m + (trackId to LyricsState(trackId, null, emptyList(), ""))) }
    }

    private fun trim(m: Map<String, LyricsState>): Map<String, LyricsState> =
        if (m.size <= 8) m else m.entries.toList().takeLast(8).associate { it.key to it.value }

    fun onPolicyStatus(status: PolicyStatus) {
        _policy.value = status
    }

    /** Sends a user command for the focused session (or the PC session [key]). */
    suspend fun command(action: String, positionMs: Long? = null, key: String? = null): CommandResult {
        val session = (key?.let { k -> _state.value?.snapshot?.sessions?.firstOrNull { it.key == k } } ?: focused())
            ?: return CommandResult.failure(ErrorCode.NO_SESSION)
        if (action !in session.actions && !(action == Action.TOGGLE && (Action.PLAY in session.actions || Action.PAUSE in session.actions))) {
            return CommandResult.failure(ErrorCode.ACTION_UNSUPPORTED)
        }
        val effective = if (action == Action.TOGGLE && Action.TOGGLE !in session.actions) {
            if (session.isPlaying) Action.PAUSE else Action.PLAY
        } else action
        return link.sendCommand(MediaCommand(session.key, effective, positionMs, MediaCommand.ORIGIN_USER, "android_ui"))
    }

    companion object {
        private const val TAG = "MD/Remote"
        const val REQUEST_RETRY_MS = 20_000L
        const val DISK_CACHE_BYTES = 16L * 1024 * 1024
    }
}
