package app.mediaduet.media

import app.mediaduet.protocol.MediaSessionInfo
import app.mediaduet.protocol.MediaSnapshot
import app.mediaduet.protocol.PlayStatus
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class TargetResolverTest {
    private fun pcSession(key: String, status: String) = MediaSessionInfo(
        key, key, key, "t", "a", "", "", "", 1000, 0, 1.0, status, emptyList(), null, "music",
    )

    private fun phone(key: String, state: Int, changed: Long = 0) = LocalSessionState(
        key, key.substringBefore('#'), "App", "t", "a", "", "", 1000, true, state, 0, 0, 1f, 0, 0, null, changed,
    )

    private fun r(choice: TargetChoice, pc: MediaSnapshot?, connected: Boolean, phone: List<LocalSessionState>, prev: AutoState? = null) =
        TargetResolver.resolve(choice, pc, connected, phone, prev).target

    private val pcPlaying = MediaSnapshot(
        1, "spotify.exe",
        listOf(pcSession("spotify.exe", PlayStatus.PLAYING), pcSession("chrome.exe", PlayStatus.PAUSED)),
    )
    private val pcPaused = MediaSnapshot(2, "spotify.exe", listOf(pcSession("spotify.exe", PlayStatus.PAUSED)))
    private val phonePlaying = listOf(
        phone("com.netease.cloudmusic#1", PlaybackConst.STATE_PLAYING),
        phone("com.spotify.music#1", PlaybackConst.STATE_PAUSED),
    )
    private val phonePaused = listOf(phone("com.netease.cloudmusic#1", PlaybackConst.STATE_PAUSED))

    @Test
    fun automaticPrefersPlayingPcThenPhone() {
        assertEquals(ResolvedTarget(Side.PC, "spotify.exe", true), r(TargetChoice.AUTO, pcPlaying, true, phonePlaying))
        assertEquals(ResolvedTarget(Side.PHONE, "com.netease.cloudmusic#1", true), r(TargetChoice.AUTO, pcPaused, true, phonePlaying))
        // A playing PC that is disconnected (stale state) does not win.
        assertEquals(Side.PHONE, r(TargetChoice.AUTO, pcPlaying, false, phonePlaying)?.side)
        // Nothing on the phone: fall back to the (paused) PC session.
        assertEquals(ResolvedTarget(Side.PC, "spotify.exe", true), r(TargetChoice.AUTO, pcPaused, true, emptyList()))
        assertNull(r(TargetChoice.AUTO, null, false, emptyList()))
        // Stale (disconnected) PC state is never shown automatically.
        assertNull(r(TargetChoice.AUTO, pcPaused, false, emptyList()))
    }

    // ---- Auto (round 4): most recently started source, with holds -------------------------------

    private val pcKey = "spotify.exe"
    private val phKey = "com.netease.cloudmusic#1"

    private fun pcSnap(playing: Boolean?) =
        if (playing == null) MediaSnapshot(9, null, emptyList())
        else MediaSnapshot(9, pcKey, listOf(pcSession(pcKey, if (playing) PlayStatus.PLAYING else PlayStatus.PAUSED)))

    private fun phoneList(playing: Boolean?, changed: Long = 0) =
        if (playing == null) emptyList() else listOf(phone(phKey, if (playing) PlaybackConst.STATE_PLAYING else PlaybackConst.STATE_PAUSED, changed))

    /** Small driver: feeds states with times like the engine does. */
    private inner class Auto(var state: AutoState? = null) {
        var uc: UserControl? = null
        fun at(t: Long, pc: Boolean?, phone: Boolean?, connected: Boolean = true, phoneChanged: Long = 0): Side? {
            state = TargetResolver.resolve(TargetChoice.AUTO, pcSnap(pc), connected, phoneList(phone, phoneChanged), state, t, uc)
            return state!!.target?.side
        }
    }

    @Test
    fun newlyStartedSourceWinsImmediatelyEvenWhileTheTargetPlays() {
        val a = Auto()
        assertEquals(Side.PHONE, a.at(1_000, pc = false, phone = true))
        // The PC starts while the phone still plays: the most recent start wins at once.
        assertEquals(Side.PC, a.at(5_000, pc = true, phone = true))
        // ... and the phone starting again later takes it back (after the switch guard).
        a.at(6_000, pc = true, phone = false)
        assertEquals(Side.PC, a.at(9_000, pc = true, phone = false))
        assertEquals(Side.PHONE, a.at(12_000, pc = true, phone = true))
    }

    @Test
    fun alreadyPlayingWhenAutoFirstEvaluates() {
        // The phone started long ago, the PC is seen for the first time: the PC is "newer".
        assertEquals(Side.PC, Auto().at(100_000, pc = true, phone = true, phoneChanged = 40_000))
        // Only the phone plays (e.g. started while the app was closed): it is picked at once.
        assertEquals(Side.PHONE, Auto().at(100_000, pc = false, phone = true, phoneChanged = 99_000))
    }

    @Test
    fun targetPausedByItsOwnAppSwitchesAfterThePauseHold() {
        // Round-3 bug: phone target, PC started meanwhile (edge consumed), phone paused -> stuck.
        val a = Auto()
        a.at(1_000, pc = false, phone = true)
        a.at(2_000, pc = true, phone = true)
        a.at(20_000, pc = true, phone = true) // guard over, PC is the target now
        assertEquals(Side.PC, a.state!!.target?.side)
        // PC paused by its own app while the phone plays: hold 3 s, then the phone.
        assertEquals(Side.PC, a.at(30_000, pc = false, phone = true))
        assertEquals(30_000L + TargetResolver.PAUSE_HOLD_MS, a.state!!.recheckAtMs) // engine re-evaluates then
        assertEquals(Side.PC, a.at(32_900, pc = false, phone = true))
        assertEquals(Side.PHONE, a.at(33_000, pc = false, phone = true))
    }

    @Test
    fun userControlInMediaDuetHoldsTheTarget() {
        val a = Auto()
        a.at(1_000, pc = false, phone = true)
        a.at(2_000, pc = true, phone = true)
        a.at(20_000, pc = true, phone = true)
        assertEquals(Side.PC, a.state!!.target?.side)
        // The user pauses the PC in MediaDuet while the phone plays: stays during the hold.
        a.uc = UserControl(Side.PC, pcKey, 30_000)
        assertEquals(Side.PC, a.at(30_100, pc = false, phone = true))
        assertEquals(30_000L + TargetResolver.USER_HOLD_MS, a.state!!.recheckAtMs)
        assertEquals(Side.PC, a.at(37_000, pc = false, phone = true))
        assertEquals(Side.PHONE, a.at(38_000, pc = false, phone = true))
        // A control on another source does not hold this one.
        val b = Auto()
        b.at(1_000, pc = false, phone = true)
        b.at(2_000, pc = true, phone = true)
        b.at(20_000, pc = true, phone = true)
        b.uc = UserControl(Side.PHONE, phKey, 30_000)
        b.at(30_000, pc = false, phone = true)
        assertEquals(Side.PHONE, b.at(33_100, pc = false, phone = true))
    }

    @Test
    fun longPausedTargetFollowsThePlayingSource() {
        val a = Auto()
        a.at(1_000, pc = true, phone = false)
        a.at(2_000, pc = false, phone = false) // PC paused, nothing plays: sticky
        assertEquals(Side.PC, a.at(60_000, pc = false, phone = false))
        // Minutes later the phone plays (it started while the app was in the background).
        assertEquals(Side.PHONE, a.at(200_000, pc = false, phone = true))
    }

    @Test
    fun nothingPlayingKeepsTheLastTargetUnlessItDisappears() {
        val a = Auto()
        a.at(1_000, pc = false, phone = true)
        a.at(2_000, pc = false, phone = false)
        assertEquals(Side.PHONE, a.at(100_000, pc = false, phone = false))
        // The phone session disappears: the most recently active remaining source (the PC).
        assertEquals(Side.PC, a.at(101_000, pc = false, phone = null))
        assertNull(Auto().at(1, pc = null, phone = null))
    }

    @Test
    fun disconnectedPcIsNeverSelectedAndReconnectIsNotANewStart() {
        val a = Auto()
        a.at(1_000, pc = true, phone = false)
        a.at(2_000, pc = true, phone = true) // the phone starts later -> phone
        assertEquals(Side.PHONE, a.state!!.target?.side)
        assertEquals(Side.PHONE, a.at(3_000, pc = true, phone = true, connected = false))
        assertNull(Auto().at(1_000, pc = true, phone = null, connected = false))
        // The PC reconnects still playing: it keeps its old start time, the phone (newer) stays.
        assertEquals(Side.PHONE, a.at(10_000, pc = true, phone = true))
        // A source that stopped and starts again *is* a new start.
        a.at(11_000, pc = false, phone = true)
        assertEquals(Side.PC, a.at(15_000, pc = true, phone = true))
    }

    @Test
    fun flappingSourceSwitchesAtMostOncePerWindow() {
        val a = Auto()
        a.at(1_000, pc = true, phone = false)
        a.at(2_000, pc = true, phone = true)
        a.at(10_000, pc = true, phone = true)
        assertEquals(Side.PHONE, a.state!!.target?.side)
        a.at(20_000, pc = true, phone = true) // settle
        // The PC bursts play / pause every 300 ms for 3 s while the phone keeps playing.
        var switches = 0
        var last = a.state!!.target?.side
        var t = 30_000L
        var pcOn = false
        while (t < 33_000) {
            pcOn = !pcOn
            val s = a.at(t, pc = pcOn, phone = true)
            if (s != last) switches++
            last = s
            t += 300
        }
        assertTrue("switched $switches times", switches <= 1)
    }

    @Test
    fun backgroundReopenKeepsEngineState() {
        // The engine keeps resolving while the UI is gone; a reopened UI just reads the result.
        val a = Auto()
        a.at(1_000, pc = false, phone = true)
        assertEquals(Side.PC, a.at(50_000, pc = true, phone = true)) // PC started while the app was not visible
        assertEquals(Side.PC, a.at(80_000, pc = true, phone = true)) // reopened later: no flip back
        // Process restart: the phone started 20 s before, the PC is paused -> the phone at once.
        assertEquals(Side.PHONE, Auto().at(100_000, pc = false, phone = true, phoneChanged = 80_000))
    }

    @Test
    fun explicitChoicesFallBackToTheSideActiveSession() {
        assertEquals("chrome.exe", r(TargetChoice(Side.PC, "chrome.exe"), pcPlaying, true, phonePlaying)?.key)
        assertEquals("spotify.exe", r(TargetChoice(Side.PC, "gone.exe"), pcPlaying, true, phonePlaying)?.key)
        assertEquals("com.spotify.music#1", r(TargetChoice(Side.PHONE, "com.spotify.music#1"), pcPlaying, true, phonePlaying)?.key)
        assertEquals(ResolvedTarget(Side.PHONE, "com.netease.cloudmusic#1", false), r(TargetChoice(Side.PHONE), pcPlaying, true, phonePlaying))
        assertNull(r(TargetChoice(Side.PHONE), pcPlaying, true, emptyList()))
        // A chosen PC that is not connected shows the phone's media instead (flagged as fallback).
        assertEquals(
            ResolvedTarget(Side.PHONE, "com.netease.cloudmusic#1", automatic = false, fallback = true),
            r(TargetChoice(Side.PC), pcPlaying, false, phonePlaying),
        )
        assertNull(r(TargetChoice(Side.PC), pcPlaying, false, emptyList()))
    }

    @Test
    fun choiceEncoding() {
        for (c in listOf(TargetChoice.AUTO, TargetChoice(Side.PC), TargetChoice(Side.PC, "a:b.exe"), TargetChoice(Side.PHONE, "com.x#2"))) {
            assertEquals(c, TargetChoice.decode(c.encode()))
        }
        assertEquals(TargetChoice.AUTO, TargetChoice.decode("garbage"))
        assertEquals(TargetChoice.AUTO, TargetChoice.decode(null))
    }
}
