package app.mediaduet.media

import app.mediaduet.protocol.VolumeSet
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** Output route → latency, and the "volume change silently ignored" detection. */
class RobustnessTest {
    @Test
    fun routeClassification() {
        assertEquals(OutputRoute.BLUETOOTH, OutputRoutes.fromRouted(listOf(OutputRoutes.TYPE_BLUETOOTH_A2DP)))
        assertEquals(OutputRoute.BLUETOOTH, OutputRoutes.fromRouted(listOf(OutputRoutes.TYPE_BUILTIN_SPEAKER, OutputRoutes.TYPE_BLE_HEADSET)))
        assertEquals(OutputRoute.BLUETOOTH, OutputRoutes.fromRouted(listOf(OutputRoutes.TYPE_HEARING_AID)))
        assertEquals(OutputRoute.WIRED, OutputRoutes.fromRouted(listOf(OutputRoutes.TYPE_USB_HEADSET)))
        assertEquals(OutputRoute.SPEAKER, OutputRoutes.fromRouted(listOf(OutputRoutes.TYPE_BUILTIN_SPEAKER)))
        assertEquals(OutputRoute.SPEAKER, OutputRoutes.fromRouted(emptyList()))
        assertEquals(OutputRoute.OTHER, OutputRoutes.fromRouted(listOf(9_999)))
        // Pre-33 fallback: SCO alone is a call route, A2DP (or the A2DP flag) wins over wired.
        assertEquals(OutputRoute.SPEAKER, OutputRoutes.fromConnected(listOf(OutputRoutes.TYPE_BUILTIN_SPEAKER, OutputRoutes.TYPE_BLUETOOTH_SCO), a2dpOn = false))
        assertEquals(OutputRoute.BLUETOOTH, OutputRoutes.fromConnected(listOf(OutputRoutes.TYPE_WIRED_HEADPHONES, OutputRoutes.TYPE_BLUETOOTH_A2DP), false))
        assertEquals(OutputRoute.BLUETOOTH, OutputRoutes.fromConnected(listOf(OutputRoutes.TYPE_BUILTIN_SPEAKER), a2dpOn = true))
        assertEquals(OutputRoute.WIRED, OutputRoutes.fromConnected(listOf(OutputRoutes.TYPE_BUILTIN_SPEAKER, OutputRoutes.TYPE_WIRED_HEADSET), false))
    }

    @Test
    fun routeLatency() {
        assertEquals(180, OutputRoutes.latencyMs(OutputRoute.BLUETOOTH, 180))
        assertEquals(1_000, OutputRoutes.latencyMs(OutputRoute.BLUETOOTH, 5_000))
        assertEquals(0, OutputRoutes.latencyMs(OutputRoute.WIRED, 180))
        assertEquals(0, OutputRoutes.latencyMs(OutputRoute.SPEAKER, 180))
    }

    @Test
    fun snapshotCarriesThePhoneOutputLatency() {
        val s = LocalSessionState(
            "p#1", "p", "P", "t", "a", "", "", 1_000, true, PlaybackConst.STATE_PLAYING, 0, 0, 1f, 0, 0, null, 0,
        )
        assertEquals(180, SnapshotBuilder.build(1, listOf(s), 0, 180).sessions.single().outputLatencyMs)
        assertNull(SnapshotBuilder.build(1, listOf(s), 0).sessions.single().outputLatencyMs)
        // A latency change (Bluetooth connected) is a meaningful snapshot change.
        val sup = SnapshotSuppressor()
        assertTrue(sup.shouldSend(SnapshotBuilder.build(1, listOf(s), 0, 0), 0))
        assertTrue(sup.shouldSend(SnapshotBuilder.build(2, listOf(s), 10, 180), 10))
    }

    private val min = 0
    private val max = 15

    @Test
    fun ignoredVolumeChangeIsDetected() {
        val before = RawVolume(5, muted = false)
        // Level asked to change, nothing happened.
        assertTrue(VolumeRestriction.ignored(VolumeSet(level = 0.9), before, before, min, max))
        // Same index requested: nothing to change, not a restriction.
        assertFalse(VolumeRestriction.ignored(VolumeSet(level = 5 / 15.0), before, before, min, max))
        // It worked.
        assertFalse(VolumeRestriction.ignored(VolumeSet(level = 0.9), before, RawVolume(14, false), min, max))
        // Delta at the limit is not a restriction; otherwise it is.
        assertFalse(VolumeRestriction.ignored(VolumeSet(delta = 1), RawVolume(15, false), RawVolume(15, false), min, max))
        assertTrue(VolumeRestriction.ignored(VolumeSet(delta = -2), before, before, min, max))
        // Mute requests.
        assertTrue(VolumeRestriction.ignored(VolumeSet(muted = true), before, before, min, max))
        assertFalse(VolumeRestriction.ignored(VolumeSet(muted = false), before, before, min, max))
        // Raising the level of a muted stream should un-mute it.
        val muted = RawVolume(5, muted = true)
        assertTrue(VolumeRestriction.ignored(VolumeSet(level = 5 / 15.0), muted, muted, min, max))
        assertFalse(VolumeRestriction.ignored(VolumeSet(level = 5 / 15.0), muted, RawVolume(5, false), min, max))
    }

    @Test
    fun expectedIndex() {
        val before = RawVolume(5, false)
        assertEquals(14, VolumeRestriction.expectedIndex(VolumeSet(level = 0.9), before, min, max))
        assertEquals(7, VolumeRestriction.expectedIndex(VolumeSet(delta = 2), before, min, max))
        assertEquals(15, VolumeRestriction.expectedIndex(VolumeSet(delta = 40), before, min, max))
        assertNull(VolumeRestriction.expectedIndex(VolumeSet(muted = true), before, min, max))
    }
}
