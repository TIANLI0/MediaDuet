package app.mediaduet.policy

import app.mediaduet.protocol.PeerPolicy
import app.mediaduet.protocol.PlayStatus
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Virtual time for the arbiter's timers. */
private class ManualTimer {
    var now = 1_000L
    private class Task(val at: Long, val block: () -> Unit, var cancelled: Boolean = false)
    private val tasks = mutableListOf<Task>()

    val timer = PolicyTimer { delayMs, block ->
        val t = Task(now + delayMs, block)
        tasks += t
        Cancellable { t.cancelled = true }
    }

    fun advance(ms: Long) {
        val end = now + ms
        while (true) {
            val next = tasks.filter { !it.cancelled && it.at <= end }.minByOrNull { it.at } ?: break
            tasks.remove(next)
            now = next.at
            next.block()
        }
        now = end
    }
}

/** One device: its arbiter, its sessions (the "player") and what it published. */
private class Device(val id: String, val clock: ManualTimer, var wall: () -> Long) {
    val actions = mutableListOf<ArbiterAction>()
    var published: PeerPolicy? = null
    val sessions = LinkedHashMap<String, String>() // key -> status
    val arbiter = PeerArbiter(id, clock.timer, { clock.now }, { wall() }, { actions += it }, { published = it })

    fun set(key: String, status: String) {
        sessions[key] = status
        arbiter.onLocalSessions(sessions.map { (k, s) -> ArbiterSession(k, "app.$k", "track", s) })
    }

    /** The player follows the arbiter's last command. */
    fun applyActions() {
        val pending = actions.toList()
        actions.clear()
        for (a in pending) set(a.key, if (a.kind == ArbiterAction.Kind.PAUSE) PlayStatus.PAUSED else PlayStatus.PLAYING)
    }
}

class PeerArbiterTest {
    private val clock = ManualTimer()
    private var wall = 1_700_000_000_000L
    private val phone = Device("1".repeat(32), clock) { wall }
    private val tablet = Device("2".repeat(32), clock) { wall }

    /** Delivers each device's current `policy.peer` to the other one. */
    private fun exchange() {
        phone.published?.let { tablet.arbiter.onPeer(phone.id, it) }
        tablet.published?.let { phone.arbiter.onPeer(tablet.id, it) }
    }

    private fun tick(ms: Long = 10) {
        wall += ms
        clock.advance(ms)
    }

    private fun connect() {
        phone.set("music", PlayStatus.PAUSED)
        tablet.set("video", PlayStatus.PAUSED)
        exchange() // baseline
    }

    @Test
    fun latestWinsPausesTheOlderPlaybackAndResumesItAfterwards() {
        connect()
        tick(); phone.set("music", PlayStatus.PLAYING); exchange()
        tick(); tablet.set("video", PlayStatus.PLAYING); exchange()
        assertEquals(listOf(ArbiterAction(ArbiterAction.Kind.PAUSE, "music", PeerArbiter.REASON_PEER_PLAYING)), phone.actions)
        assertTrue(tablet.actions.isEmpty())
        phone.applyActions(); exchange()
        assertEquals(ArbiterState.POLICY_PAUSED, phone.arbiter.stateOf("music"))
        assertTrue(phone.published!!.pausedAt > 0)

        // The tablet's video stops: after the resume delay the phone's music continues.
        tick(); tablet.set("video", PlayStatus.PAUSED); exchange()
        tick(2_000)
        assertTrue(phone.actions.isEmpty())
        tick(1_100)
        assertEquals(ArbiterAction.Kind.RESUME, phone.actions.single().kind)
        phone.applyActions(); exchange()
        assertEquals(ArbiterState.PLAYING, phone.arbiter.stateOf("music"))
        // An automatic resume is not a new playback: the tablet does not react to it.
        assertEquals(PeerPolicy.CAUSE_POLICY, phone.published!!.cause)
        tick(); tablet.set("video", PlayStatus.PAUSED); exchange()
        assertTrue(tablet.actions.isEmpty())
    }

    @Test
    fun aUserPauseIsNeverUndone() {
        connect()
        tick(); phone.set("music", PlayStatus.PLAYING); exchange()
        tick(); tablet.set("video", PlayStatus.PLAYING); exchange()
        phone.applyActions(); exchange()
        // The user now pauses and resumes nothing; the tablet stops; then the user pauses the phone's
        // music explicitly (track skip while paused counts as a decision).
        phone.arbiter.onLocalCommand("music", "pause", policy = false)
        assertEquals(ArbiterState.USER_PAUSED, phone.arbiter.stateOf("music"))
        tick(); tablet.set("video", PlayStatus.PAUSED); exchange()
        tick(10_000)
        assertTrue(phone.actions.isEmpty())
    }

    @Test
    fun crossingStartsOnlyPauseTheOlderOne() {
        connect()
        tick(); phone.set("music", PlayStatus.PLAYING)
        tick(); tablet.set("video", PlayStatus.PLAYING)
        exchange() // both messages cross
        assertEquals(1, phone.actions.size)
        assertTrue(tablet.actions.isEmpty())
    }

    @Test
    fun priorityModeKeepsTheHigherDevicePlaying() {
        phone.arbiter.setConfig(PeerArbiterConfig(mode = PeerPolicy.MODE_PRIORITY, priority = 2))
        tablet.arbiter.setConfig(PeerArbiterConfig(mode = PeerPolicy.MODE_PRIORITY, priority = 1))
        connect()
        tick(); phone.set("music", PlayStatus.PLAYING); exchange()
        tick(); tablet.set("video", PlayStatus.PLAYING); exchange()
        assertTrue(phone.actions.isEmpty())
        // The other way round the tablet yields.
        tick(); tablet.set("video", PlayStatus.PAUSED); exchange()
        tick(); tablet.set("video", PlayStatus.PLAYING); exchange()
        tick(); phone.set("music", PlayStatus.PAUSED); exchange()
        tick(); phone.set("music", PlayStatus.PLAYING); exchange()
        assertEquals(ArbiterAction.Kind.PAUSE, tablet.actions.single().kind)
    }

    @Test
    fun aCallPausesPeersEvenWhenTheyWouldNotYield() {
        phone.arbiter.setConfig(PeerArbiterConfig(mode = PeerPolicy.MODE_PRIORITY, priority = 2))
        connect()
        tick(); phone.set("music", PlayStatus.PLAYING); exchange()
        tick(); tablet.arbiter.onLocalCall(true); exchange()
        assertEquals(PeerArbiter.REASON_PEER_CALL, phone.actions.single().reason)
        phone.applyActions(); exchange()
        tick(); tablet.arbiter.onLocalCall(false); exchange()
        tick(3_100)
        assertEquals(ArbiterAction.Kind.RESUME, phone.actions.single().kind)
    }

    @Test
    fun offNeverActs() {
        phone.arbiter.setConfig(PeerArbiterConfig(mode = PeerPolicy.MODE_OFF))
        connect()
        tick(); phone.set("music", PlayStatus.PLAYING); exchange()
        tick(); tablet.set("video", PlayStatus.PLAYING); exchange()
        assertTrue(phone.actions.isEmpty())
    }

    @Test
    fun theFirstReportOfAPeerIsABaseline() {
        phone.set("music", PlayStatus.PLAYING)
        tablet.set("video", PlayStatus.PLAYING)
        tick(); exchange()
        assertTrue(phone.actions.isEmpty())
        assertTrue(tablet.actions.isEmpty())
    }

    @Test
    fun theDevicePausedLastResumesFirst() {
        val third = Device("3".repeat(32), clock) { wall }
        fun all() {
            val ds = listOf(phone, tablet, third)
            for (a in ds) for (b in ds) if (a !== b) a.published?.let { b.arbiter.onPeer(a.id, it) }
        }
        phone.set("m", PlayStatus.PAUSED); tablet.set("m", PlayStatus.PAUSED); third.set("m", PlayStatus.PAUSED); all()
        tick(); phone.set("m", PlayStatus.PLAYING); all()
        tick(); tablet.set("m", PlayStatus.PLAYING); all(); phone.applyActions(); all()
        tick(); third.set("m", PlayStatus.PLAYING); all(); tablet.applyActions(); all()
        assertEquals(ArbiterState.POLICY_PAUSED, phone.arbiter.stateOf("m"))
        assertEquals(ArbiterState.POLICY_PAUSED, tablet.arbiter.stateOf("m"))
        // The third device stops: only the tablet (paused last) resumes; the phone keeps waiting.
        tick(); third.set("m", PlayStatus.PAUSED); all()
        tick(3_100)
        assertTrue(phone.actions.isEmpty())
        assertEquals(ArbiterAction.Kind.RESUME, tablet.actions.single().kind)
        tablet.applyActions(); all()
        // Then the tablet stops (user): the phone resumes.
        tick(); tablet.set("m", PlayStatus.PAUSED); all()
        tick(3_100)
        assertEquals(ArbiterAction.Kind.RESUME, phone.actions.single().kind)
    }

    @Test
    fun aResumeThatDoesNotHappenIsNotRetried() {
        connect()
        tick(); phone.set("music", PlayStatus.PLAYING); exchange()
        tick(); tablet.set("video", PlayStatus.PLAYING); exchange()
        phone.applyActions(); exchange()
        tick(); tablet.set("video", PlayStatus.PAUSED); exchange()
        tick(3_100)
        phone.actions.clear() // the player ignores the resume
        tick(5_000)
        assertEquals(ArbiterState.USER_PAUSED, phone.arbiter.stateOf("music"))
        assertEquals(0L, phone.published!!.pausedAt)
        assertTrue(phone.arbiter.lastAction!!.failed)
        tick(10_000)
        assertTrue(phone.actions.isEmpty())
        assertFalse(phone.published!!.playing)
    }
}
