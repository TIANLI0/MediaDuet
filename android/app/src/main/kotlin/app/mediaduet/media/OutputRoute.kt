package app.mediaduet.media

/** Kind of device the phone's media audio currently plays on. */
enum class OutputRoute { SPEAKER, WIRED, BLUETOOTH, OTHER }

/**
 * Pure mapping from android.media.AudioDeviceInfo types to an [OutputRoute] and the output
 * latency sent as `outputLatencyMs` (mirrored constants so it is unit-testable on the JVM).
 */
object OutputRoutes {
    // android.media.AudioDeviceInfo.TYPE_*
    const val TYPE_BUILTIN_EARPIECE = 1
    const val TYPE_BUILTIN_SPEAKER = 2
    const val TYPE_WIRED_HEADSET = 3
    const val TYPE_WIRED_HEADPHONES = 4
    const val TYPE_LINE_ANALOG = 5
    const val TYPE_LINE_DIGITAL = 6
    const val TYPE_BLUETOOTH_SCO = 7
    const val TYPE_BLUETOOTH_A2DP = 8
    const val TYPE_HDMI = 9
    const val TYPE_USB_DEVICE = 11
    const val TYPE_USB_ACCESSORY = 12
    const val TYPE_DOCK = 13
    const val TYPE_AUX_LINE = 19
    const val TYPE_USB_HEADSET = 22
    const val TYPE_HEARING_AID = 23
    const val TYPE_BUILTIN_SPEAKER_SAFE = 24
    const val TYPE_BLE_HEADSET = 26
    const val TYPE_BLE_SPEAKER = 27
    const val TYPE_BLE_BROADCAST = 30

    const val DEFAULT_BLUETOOTH_MS = 180
    const val MAX_BLUETOOTH_MS = 1_000

    private val BLUETOOTH = setOf(TYPE_BLUETOOTH_A2DP, TYPE_BLE_HEADSET, TYPE_BLE_SPEAKER, TYPE_BLE_BROADCAST, TYPE_HEARING_AID, TYPE_BLUETOOTH_SCO)
    private val WIRED = setOf(
        TYPE_WIRED_HEADSET, TYPE_WIRED_HEADPHONES, TYPE_LINE_ANALOG, TYPE_LINE_DIGITAL, TYPE_HDMI,
        TYPE_USB_DEVICE, TYPE_USB_ACCESSORY, TYPE_DOCK, TYPE_AUX_LINE, TYPE_USB_HEADSET,
    )
    private val SPEAKER = setOf(TYPE_BUILTIN_SPEAKER, TYPE_BUILTIN_EARPIECE, TYPE_BUILTIN_SPEAKER_SAFE)

    fun of(type: Int): OutputRoute = when (type) {
        in BLUETOOTH -> OutputRoute.BLUETOOTH
        in WIRED -> OutputRoute.WIRED
        in SPEAKER -> OutputRoute.SPEAKER
        else -> OutputRoute.OTHER
    }

    /**
     * Route of the media stream from the devices it is routed to (API 33+
     * `getAudioDevicesForAttributes`): Bluetooth wins when any routed device is Bluetooth.
     */
    fun fromRouted(types: List<Int>): OutputRoute {
        val routes = types.map(::of)
        return when {
            OutputRoute.BLUETOOTH in routes -> OutputRoute.BLUETOOTH
            OutputRoute.WIRED in routes -> OutputRoute.WIRED
            OutputRoute.SPEAKER in routes -> OutputRoute.SPEAKER
            routes.isNotEmpty() -> OutputRoute.OTHER
            else -> OutputRoute.SPEAKER
        }
    }

    /**
     * Fallback below API 33 from all connected outputs: media follows a connected A2DP / LE audio
     * / hearing-aid device (SCO alone is a call route), then wired outputs, else the speaker.
     */
    fun fromConnected(types: List<Int>, a2dpOn: Boolean): OutputRoute = when {
        a2dpOn || types.any { it in BLUETOOTH && it != TYPE_BLUETOOTH_SCO } -> OutputRoute.BLUETOOTH
        types.any { it in WIRED } -> OutputRoute.WIRED
        else -> OutputRoute.SPEAKER
    }

    /** `outputLatencyMs` for [route]: the configured Bluetooth delay, 0 for wired / speaker. */
    fun latencyMs(route: OutputRoute, bluetoothMs: Int): Int = when (route) {
        OutputRoute.BLUETOOTH -> bluetoothMs.coerceIn(0, MAX_BLUETOOTH_MS)
        else -> 0
    }
}
