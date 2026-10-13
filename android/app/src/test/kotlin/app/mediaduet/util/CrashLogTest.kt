package app.mediaduet.util

import app.mediaduet.service.decodeOffsets
import app.mediaduet.service.encodeOffsets
import app.mediaduet.transport.LanAddressPolicy
import app.mediaduet.transport.NetCandidate
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** Crash capture, LAN endpoint choice and the persisted per-app lyric offsets. */
class CrashLogTest {
    @get:Rule val tmp = TemporaryFolder()

    @Test
    fun keepsTheLastLinesAndWritesReports() {
        var now = 1_000L
        val log = CrashLog(tmp.root, wallClock = { now }, maxFiles = 5, ringSize = 3, header = { "hdr\n" })
        for (i in 1..5) log.log(MdLog.Level.INFO, "T", "line $i", null)
        assertEquals(listOf("line 3", "line 4", "line 5"), log.recentLines().map { it.substringAfter("I/T: ") })
        val f = log.recordCrash("main", IllegalStateException("boom"))
        assertNotNull(f)
        val text = f!!.readText()
        assertTrue(text.startsWith("hdr\nMediaDuet crash report"))
        assertTrue("java.lang.IllegalStateException: boom" in text)
        assertTrue("line 5" in text && "line 2" !in text)
        assertEquals(1_000L, CrashLog.timeOf(f))
    }

    @Test
    fun rotatesToFiveAndTracksSeen() {
        var now = 1_000L
        val log = CrashLog(tmp.root, wallClock = { now })
        assertNull(log.latestUnseen())
        repeat(7) {
            now += 1_000
            log.recordCrash("main", RuntimeException("crash $it"))
        }
        val reports = log.reports()
        assertEquals(5, reports.size)
        assertEquals(8_000L, CrashLog.timeOf(reports.first()))
        assertEquals(reports.first(), log.latestUnseen())
        log.markSeen()
        assertNull(log.latestUnseen())
        now += 1_000
        log.recordCrash("main", RuntimeException("again"))
        assertEquals(9_000L, CrashLog.timeOf(log.latestUnseen()!!))
        // Two crashes in the same millisecond get distinct files.
        log.recordCrash("main", RuntimeException("same ms"))
        assertEquals(5, log.reports().size)
    }

    @Test
    fun lanAddressesOnlyFromWifiAndEthernetDefaultWifiFirst() {
        val nets = listOf(
            NetCandidate(wifi = false, ethernet = false, vpn = false, cellular = true, isDefault = false, addresses = listOf("10.64.0.9")),
            NetCandidate(wifi = true, ethernet = false, vpn = true, cellular = false, isDefault = true, addresses = listOf("10.8.0.2")),
            NetCandidate(wifi = false, ethernet = true, vpn = false, cellular = false, isDefault = false, addresses = listOf("192.168.2.5")),
            NetCandidate(wifi = true, ethernet = false, vpn = false, cellular = false, isDefault = false, addresses = listOf("169.254.1.1", "192.168.1.20")),
        )
        // VPN is the default here; Wi-Fi still ranks before Ethernet; link-local is dropped.
        assertEquals(listOf("192.168.1.20", "192.168.2.5"), LanAddressPolicy.pick(nets))
        val wifiDefault = nets.map { if (it.addresses == listOf("192.168.2.5")) it.copy(isDefault = false) else it }
            .map { if ("192.168.1.20" in it.addresses) it.copy(isDefault = true) else it }
        assertEquals("192.168.1.20", LanAddressPolicy.pick(wifiDefault).first())
        // The phone's own hotspot interface comes last; duplicates are removed.
        assertEquals(listOf("192.168.1.20", "192.168.2.5", "192.168.43.1"), LanAddressPolicy.pick(nets, listOf("192.168.43.1", "192.168.1.20")))
        assertTrue(LanAddressPolicy.isHotspotInterface("ap0"))
        assertTrue(LanAddressPolicy.isHotspotInterface("swlan0"))
        assertTrue(!LanAddressPolicy.isHotspotInterface("rmnet_data0") && !LanAddressPolicy.isHotspotInterface("wlan0"))
        for (bad in listOf("127.0.0.1", "0.0.0.0", "224.0.0.251", "1.2.3", "a.b.c.d", "300.1.1.1")) {
            assertTrue(bad, !LanAddressPolicy.isUsableIpv4(bad))
        }
    }

    @Test
    fun lyricOffsetsRoundTrip() {
        val m = mapOf("Spotify.exe" to 300, "com.netease.cloudmusic" to -1_200, "zero" to 0)
        val decoded = decodeOffsets(encodeOffsets(m))
        assertEquals(mapOf("Spotify.exe" to 300, "com.netease.cloudmusic" to -1_200), decoded)
        assertEquals(mapOf("a" to 5_000), decodeOffsets("""{"a":99999,"b":"x","":5,"c":1.5}"""))
        assertEquals(emptyMap<String, Int>(), decodeOffsets("not json"))
        assertEquals(emptyMap<String, Int>(), decodeOffsets(null))
    }
}
