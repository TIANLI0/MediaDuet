package app.mediaduet.service

import android.content.Context
import android.os.Build
import android.provider.Settings
import androidx.core.content.edit
import app.mediaduet.crypto.Rand
import app.mediaduet.media.OutputRoutes
import app.mediaduet.media.TargetChoice
import app.mediaduet.remote.LyricsDelay
import app.mediaduet.protocol.Hex
import app.mediaduet.protocol.StrLimit
import app.mediaduet.protocol.truncateCodePoints
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import app.mediaduet.protocol.LanEndpoint
import app.mediaduet.protocol.PeerPolicy
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive

/** Persistent, non-secret settings and the device identity (§3.1). */
class AppSettings(context: Context) {
    private val prefs = context.getSharedPreferences("settings", Context.MODE_PRIVATE)

    /** 16 random bytes, lowercase hex, generated once. Not secret. */
    val deviceId: String = prefs.getString(KEY_DEVICE_ID, null)?.takeIf { Hex.isLowerHex(it, 32) }
        ?: Hex.encode(Rand.bytes(16)).also { prefs.edit { putString(KEY_DEVICE_ID, it) } }

    private val defaultName: String =
        (Settings.Global.getString(context.contentResolver, Settings.Global.DEVICE_NAME)?.takeIf { it.isNotBlank() }
            ?: "${Build.MANUFACTURER} ${Build.MODEL}").truncateCodePoints(StrLimit.DEVICE_NAME)

    private val _deviceName = MutableStateFlow(prefs.getString(KEY_NAME, null) ?: defaultName)
    val deviceName: StateFlow<String> = _deviceName

    private val _foregroundService = MutableStateFlow(prefs.getBoolean(KEY_FGS, true))
    val foregroundService: StateFlow<Boolean> = _foregroundService

    private val _mirror = MutableStateFlow(prefs.getBoolean(KEY_MIRROR, false))
    val mirrorEnabled: StateFlow<Boolean> = _mirror

    private val _audioReceive = MutableStateFlow(prefs.getBoolean(KEY_AUDIO, false))
    val audioReceiveEnabled: StateFlow<Boolean> = _audioReceive

    private val _translations = MutableStateFlow(prefs.getBoolean(KEY_TRANSLATIONS, true))
    /** Show translated lyric lines (e.g. Chinese translations under English lyrics). */
    val showTranslations: StateFlow<Boolean> = _translations

    fun setShowTranslations(enabled: Boolean) {
        prefs.edit { putBoolean(KEY_TRANSLATIONS, enabled) }
        _translations.value = enabled
    }

    private val _lyricsSize = MutableStateFlow(LyricsSize.fromKey(prefs.getString(KEY_LYRICS_SIZE, null)))
    /** Text size of the synced lyrics (Now Playing lyrics mode). */
    val lyricsSize: StateFlow<LyricsSize> = _lyricsSize

    fun setLyricsSize(size: LyricsSize) {
        prefs.edit { putString(KEY_LYRICS_SIZE, size.key) }
        _lyricsSize.value = size
    }

    private val _lyricsNotification = MutableStateFlow(prefs.getBoolean(KEY_LYRICS_NOTIFICATION, false))
    /** The current lyric line as a notification (lock screen; Live Update / Fluid Cloud on Android 16+). */
    val lyricsNotification: StateFlow<Boolean> = _lyricsNotification

    fun setLyricsNotification(enabled: Boolean) {
        prefs.edit { putBoolean(KEY_LYRICS_NOTIFICATION, enabled) }
        _lyricsNotification.value = enabled
    }

    private val _autoLatency = MutableStateFlow(prefs.getBoolean(KEY_LATENCY_AUTO, true))
    /** Delay lyrics by the playing device's output latency (Bluetooth) automatically. */
    val lyricsAutoLatency: StateFlow<Boolean> = _autoLatency

    fun setLyricsAutoLatency(enabled: Boolean) {
        prefs.edit { putBoolean(KEY_LATENCY_AUTO, enabled) }
        _autoLatency.value = enabled
    }

    private val _bluetoothLatency = MutableStateFlow(
        prefs.getInt(KEY_LATENCY_BT, OutputRoutes.DEFAULT_BLUETOOTH_MS).coerceIn(0, OutputRoutes.MAX_BLUETOOTH_MS),
    )
    /** Assumed delay of Bluetooth audio (`lyrics.latency.bluetoothMs`, 0..1000 ms). */
    val bluetoothLatencyMs: StateFlow<Int> = _bluetoothLatency

    fun setBluetoothLatencyMs(ms: Int) {
        val v = ms.coerceIn(0, OutputRoutes.MAX_BLUETOOTH_MS)
        prefs.edit { putInt(KEY_LATENCY_BT, v) }
        _bluetoothLatency.value = v
    }

    private val _lyricOffsets = MutableStateFlow(decodeOffsets(prefs.getString(KEY_OFFSETS, null)))
    /** Manual lyric offsets per app id (ms, positive = lyrics later); zero entries are not stored. */
    val lyricOffsets: StateFlow<Map<String, Int>> = _lyricOffsets

    fun setLyricOffset(appId: String, ms: Int) {
        if (appId.isEmpty()) return
        val v = ms.coerceIn(LyricsDelay.MIN_OFFSET_MS, LyricsDelay.MAX_OFFSET_MS)
        val next = if (v == 0) _lyricOffsets.value - appId else _lyricOffsets.value + (appId to v)
        prefs.edit { putString(KEY_OFFSETS, encodeOffsets(next)) }
        _lyricOffsets.value = next
    }

    private val _onlineLyrics = MutableStateFlow(prefs.getBoolean(KEY_ONLINE_LYRICS, true))
    /** Look up lyrics online on this phone (NetEase / QQ Music / LRCLIB) when the PC has none. */
    val onlineLyrics: StateFlow<Boolean> = _onlineLyrics

    fun setOnlineLyrics(enabled: Boolean) {
        prefs.edit { putBoolean(KEY_ONLINE_LYRICS, enabled) }
        _onlineLyrics.value = enabled
    }

    private val _mode = MutableStateFlow(WorkMode.decode(prefs.getString(KEY_MODE, null)))
    /**
     * How the app is used: with a PC ([WorkMode.LINKED]) or on this phone only
     * ([WorkMode.STANDALONE]: no listening for PCs, no pairing prompts). [WorkMode.UNSET] until the
     * user chose (first run); it behaves like LINKED.
     */
    val mode: StateFlow<WorkMode> = _mode

    fun setMode(mode: WorkMode) {
        prefs.edit { putString(KEY_MODE, mode.key) }
        _mode.value = mode
    }

    private val _dismissedVersionWarning = MutableStateFlow(prefs.getString(KEY_VERSION_DISMISSED, "") ?: "")
    /** "local|peer" version pair whose mismatch warning the user dismissed on Now Playing. */
    val dismissedVersionWarning: StateFlow<String> = _dismissedVersionWarning

    fun dismissVersionWarning(pair: String) {
        prefs.edit { putString(KEY_VERSION_DISMISSED, pair) }
        _dismissedVersionWarning.value = pair
    }

    private val _target = MutableStateFlow(TargetChoice.decode(prefs.getString(KEY_TARGET, null)))
    /** Player control target (auto / phone / PC, optionally a specific session). */
    val target: StateFlow<TargetChoice> = _target

    fun setTarget(choice: TargetChoice) {
        prefs.edit { putString(KEY_TARGET, choice.encode()) }
        _target.value = choice
    }

    fun setDeviceName(name: String) {
        val clean = name.trim().truncateCodePoints(StrLimit.DEVICE_NAME).ifEmpty { defaultName }
        prefs.edit { putString(KEY_NAME, clean) }
        _deviceName.value = clean
    }

    fun setForegroundService(enabled: Boolean) {
        prefs.edit { putBoolean(KEY_FGS, enabled) }
        _foregroundService.value = enabled
    }

    fun setMirror(enabled: Boolean) {
        prefs.edit { putBoolean(KEY_MIRROR, enabled) }
        _mirror.value = enabled
    }

    fun setAudioReceive(enabled: Boolean) {
        prefs.edit { putBoolean(KEY_AUDIO, enabled) }
        _audioReceive.value = enabled
    }

    private val _peerPolicy = MutableStateFlow(
        PeerPolicySettings(
            mode = prefs.getString(KEY_PEER_MODE, null)?.takeIf { it in PeerPolicy.MODES } ?: PeerPolicy.MODE_LATEST,
            priority = prefs.getInt(KEY_PEER_PRIORITY, 1).coerceIn(PeerPolicy.MIN_PRIORITY, PeerPolicy.MAX_PRIORITY),
            autoResume = prefs.getBoolean(KEY_PEER_RESUME, true),
            pauseForCalls = prefs.getBoolean(KEY_PEER_CALLS, true),
        ),
    )
    /** Arbitration with other Android devices (§13): how this device yields to their playback. */
    val peerPolicy: StateFlow<PeerPolicySettings> = _peerPolicy

    fun setPeerPolicy(p: PeerPolicySettings) {
        prefs.edit {
            putString(KEY_PEER_MODE, p.mode)
            putInt(KEY_PEER_PRIORITY, p.priority)
            putBoolean(KEY_PEER_RESUME, p.autoResume)
            putBoolean(KEY_PEER_CALLS, p.pauseForCalls)
        }
        _peerPolicy.value = p
    }

    /** Last known LAN endpoints of paired Android devices we dial (by device id). */
    @Synchronized
    fun peerEndpoints(peerId: String): List<LanEndpoint> = decodeEndpoints(prefs.getString(KEY_PEER_ENDPOINTS, null))[peerId].orEmpty()

    /** Puts [endpoints] first in [peerId]'s list (at most 6 kept). */
    @Synchronized
    fun rememberPeerEndpoints(peerId: String, endpoints: List<LanEndpoint>) {
        val all = decodeEndpoints(prefs.getString(KEY_PEER_ENDPOINTS, null)).toMutableMap()
        val next = (endpoints + all[peerId].orEmpty()).distinct().take(6)
        if (next == all[peerId]) return
        all[peerId] = next
        prefs.edit { putString(KEY_PEER_ENDPOINTS, encodeEndpoints(all)) }
    }

    @Synchronized
    fun forgetPeerEndpoints(keep: Set<String>) {
        val all = decodeEndpoints(prefs.getString(KEY_PEER_ENDPOINTS, null))
        if (all.keys.all { it in keep }) return
        prefs.edit { putString(KEY_PEER_ENDPOINTS, encodeEndpoints(all.filterKeys { it in keep })) }
    }

    private val _autoUpdateCheck =MutableStateFlow(prefs.getBoolean(KEY_UPDATE_AUTO, true))
    /** Look for a new MediaDuet version on GitHub once a day (Settings > About). */
    val autoUpdateCheck: StateFlow<Boolean> = _autoUpdateCheck

    fun setAutoUpdateCheck(enabled: Boolean) {
        prefs.edit { putBoolean(KEY_UPDATE_AUTO, enabled) }
        _autoUpdateCheck.value = enabled
    }

    /** Wall time (ms) of the last update check that got an answer. */
    var lastUpdateCheckMs: Long
        get() = prefs.getLong(KEY_UPDATE_CHECKED, 0)
        set(v) = prefs.edit { putLong(KEY_UPDATE_CHECKED, v) }

    /** The newest version the user was already told about (one notification per version). */
    var updateNotified: String
        get() = prefs.getString(KEY_UPDATE_NOTIFIED, "") ?: ""
        set(v) = prefs.edit { putString(KEY_UPDATE_NOTIFIED, v) }

    private companion object {
        const val KEY_DEVICE_ID = "device_id"
        const val KEY_NAME = "device_name"
        const val KEY_FGS = "foreground_service"
        const val KEY_MIRROR = "mirror_session"
        const val KEY_LYRICS_NOTIFICATION = "lyrics_notification"
        const val KEY_AUDIO = "audio_receive"
        const val KEY_TRANSLATIONS = "lyrics_translations"
        const val KEY_LYRICS_SIZE = "lyrics.size"
        const val KEY_TARGET = "player_target"
        const val KEY_LATENCY_AUTO = "lyrics.latency.auto"
        const val KEY_LATENCY_BT = "lyrics.latency.bluetoothMs"
        const val KEY_OFFSETS = "lyrics.offsets"
        const val KEY_ONLINE_LYRICS = "lyrics.online"
        const val KEY_MODE = "work_mode"
        const val KEY_VERSION_DISMISSED = "version_warning_dismissed"
        const val KEY_UPDATE_AUTO = "update.auto"
        const val KEY_UPDATE_CHECKED = "update.checkedAt"
        const val KEY_UPDATE_NOTIFIED = "update.notified"
        const val KEY_PEER_MODE = "peer.policy.mode"
        const val KEY_PEER_PRIORITY = "peer.policy.priority"
        const val KEY_PEER_RESUME = "peer.policy.autoResume"
        const val KEY_PEER_CALLS = "peer.policy.calls"
        const val KEY_PEER_ENDPOINTS = "peer.endpoints"
    }
}

/** Settings › Arbitration › Android devices (see [app.mediaduet.policy.PeerArbiter]). */
data class PeerPolicySettings(
    val mode: String = PeerPolicy.MODE_LATEST,
    val priority: Int = 1,
    val autoResume: Boolean = true,
    val pauseForCalls: Boolean = true,
)

/** Endpoints map <-> JSON `{"<id>":["host:port", …]}` (invalid entries are dropped). */
internal fun encodeEndpoints(map: Map<String, List<LanEndpoint>>): String =
    JsonObject(map.mapValues { (_, list) -> JsonArray(list.map { JsonPrimitive("${it.host}|${it.port}") }) }).toString()

internal fun decodeEndpoints(s: String?): Map<String, List<LanEndpoint>> {
    if (s.isNullOrEmpty()) return emptyMap()
    val o = runCatching { Json.parseToJsonElement(s) as? JsonObject }.getOrNull() ?: return emptyMap()
    return o.mapValues { (_, v) ->
        (v as? JsonArray).orEmpty().mapNotNull { e ->
            val parts = (e as? JsonPrimitive)?.takeIf { it.isString }?.content?.split('|') ?: return@mapNotNull null
            val port = parts.getOrNull(1)?.toIntOrNull()?.takeIf { it in 1..65535 } ?: return@mapNotNull null
            parts[0].takeIf { it.isNotBlank() }?.let { LanEndpoint(it, port) }
        }
    }.filterValues { it.isNotEmpty() }
}

/** Synced lyrics text size: a scale on the lyrics style (translations scale along). */
enum class LyricsSize(val key: String, val scale: Float) {
    SMALL("small", 0.84f),
    STANDARD("standard", 1f),
    LARGE("large", 1.18f);

    companion object {
        fun fromKey(key: String?) = entries.firstOrNull { it.key == key } ?: STANDARD
    }
}

/** How MediaDuet is used on this phone. */
enum class WorkMode(val key: String) {
    UNSET("unset"),
    LINKED("linked"),
    STANDALONE("standalone");

    /** Talks to a PC (listens, pairs, shows PC media). */
    val linked: Boolean get() = this != STANDALONE

    companion object {
        fun decode(s: String?): WorkMode = entries.firstOrNull { it.key == s } ?: UNSET
    }
}

/** Offsets map <-> JSON object string (unknown / invalid entries are dropped). */
internal fun encodeOffsets(map: Map<String, Int>): String =
    JsonObject(map.filterValues { it != 0 }.mapValues { JsonPrimitive(it.value) }).toString()

internal fun decodeOffsets(s: String?): Map<String, Int> {
    if (s.isNullOrEmpty()) return emptyMap()
    val o = runCatching { Json.parseToJsonElement(s) as? JsonObject }.getOrNull() ?: return emptyMap()
    return o.mapNotNull { (k, v) ->
        val ms = (v as? JsonPrimitive)?.takeIf { !it.isString }?.content?.toIntOrNull() ?: return@mapNotNull null
        if (k.isEmpty() || ms == 0) null else k to ms.coerceIn(LyricsDelay.MIN_OFFSET_MS, LyricsDelay.MAX_OFFSET_MS)
    }.toMap()
}
