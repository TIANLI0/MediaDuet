package app.mediaduet.policy

import app.mediaduet.protocol.PeerPolicy
import app.mediaduet.protocol.PlayStatus

/** Schedules delayed work on the arbiter's thread (coroutines in the app, a manual clock in tests). */
fun interface PolicyTimer {
    fun schedule(delayMs: Long, block: () -> Unit): Cancellable
}

fun interface Cancellable {
    fun cancel()
}

data class PeerArbiterConfig(
    /** [PeerPolicy.MODES]: off, latest (the newest playback wins) or priority (higher wins, ties: newest). */
    val mode: String = PeerPolicy.MODE_LATEST,
    val priority: Int = 1,
    val autoResume: Boolean = true,
    val pauseForCalls: Boolean = true,
    val resumeDelayMs: Long = 3_000,
    /** How long a pause / resume of ours may take to show up in the session state. */
    val confirmTimeoutMs: Long = 4_000,
    /** How long a user command explains the next transition of its session. */
    val intentWindowMs: Long = 4_000,
)

/** One local media session as the arbiter sees it. */
data class ArbiterSession(val key: String, val appId: String, val trackId: String, val status: String)

/** Arbitration state of one local session (as on Windows, docs/architecture.md). */
enum class ArbiterState { UNKNOWN, PLAYING, USER_PAUSED, POLICY_PAUSED }

data class ArbiterAction(val kind: Kind, val key: String, val reason: String) {
    enum class Kind { PAUSE, RESUME }
}

data class ArbiterLastAction(val what: String, val reason: String, val peerId: String?, val app: String, val atWallMs: Long, val failed: Boolean = false)

/**
 * Arbitration between Android devices (protocol §13). Unlike the Windows engine, which controls
 * both the PC and the phone, every Android device runs one of these and only ever pauses or
 * resumes its *own* sessions; peers learn about each other from `policy.peer` ([PeerPolicy]).
 * Since no session has two controllers, two engines can never fight over it.
 *
 * * A peer's user-started playback (a new `onsetAt`) or call makes us pause what plays here, if
 *   the mode says we yield ([shouldYield]). Its automatic resumes (`cause: policy`) never do.
 * * Messages crossing on the wire: of two playbacks started at about the same time, only the
 *   older one yields (comparing the senders' `onsetAt` wall clocks).
 * * When no peer plays (or calls) any more we resume what we paused, after [PeerArbiterConfig.resumeDelayMs].
 *   Of several devices waiting, the one that paused last resumes first (it played most
 *   recently); ties go to the larger device id. Every device computes the same order.
 * * Pauses we did not cause (the user, the app, or the PC's arbitration) are never undone, and a
 *   pause / resume that does not take effect within the confirm timeout is not retried.
 * * The first `policy.peer` of a connection is a baseline (existing state, not a new playback).
 *
 * Not thread-safe: the owner calls everything (and runs [PolicyTimer] callbacks) on one thread.
 */
class PeerArbiter(
    private val selfId: String,
    private val timer: PolicyTimer,
    private val nowMs: () -> Long,
    private val wallMs: () -> Long,
    private val execute: (ArbiterAction) -> Unit,
    private val publish: (PeerPolicy) -> Unit,
) {
    private class Tracked(var status: String, var state: ArbiterState, var appId: String, var trackId: String)
    private class Expect(val key: String, val playing: Boolean, val deadline: Long, val reason: String)
    private class Intent(val key: String, val playing: Boolean, val policy: Boolean, val at: Long)

    var config = PeerArbiterConfig()
        private set
    private val local = LinkedHashMap<String, Tracked>()
    private val peers = HashMap<String, PeerPolicy>()
    private var call = false
    private var seenLocal = false
    private var cause = PeerPolicy.CAUSE_NONE
    private var onsetAt = 0L
    private var pausedAt = 0L
    private val expects = mutableListOf<Expect>()
    private val intents = ArrayDeque<Intent>()
    private var resumeTimer: Cancellable? = null
    private var expectTimer: Cancellable? = null
    private var published: PeerPolicy? = null

    var lastAction: ArbiterLastAction? = null
        private set

    /** What we tell our peers (`policy.peer`). */
    fun state(): PeerPolicy {
        val playingKey = local.entries.firstOrNull { playingLike(it.value.status) }?.key
        return PeerPolicy(
            mode = config.mode,
            priority = config.priority,
            call = call,
            playing = playingKey != null,
            cause = if (playingKey != null) cause.takeIf { it != PeerPolicy.CAUSE_NONE } ?: PeerPolicy.CAUSE_USER else PeerPolicy.CAUSE_NONE,
            onsetAt = onsetAt,
            pausedAt = if (local.values.any { it.state == ArbiterState.POLICY_PAUSED }) pausedAt else 0,
            app = playingKey?.let { local[it]?.appId }.orEmpty(),
        )
    }

    fun stateOf(key: String): ArbiterState = local[key]?.state ?: ArbiterState.UNKNOWN

    fun setConfig(cfg: PeerArbiterConfig) {
        val off = cfg.mode == PeerPolicy.MODE_OFF && config.mode != PeerPolicy.MODE_OFF
        config = cfg.copy(priority = cfg.priority.coerceIn(PeerPolicy.MIN_PRIORITY, PeerPolicy.MAX_PRIORITY))
        if (off) {
            resumeTimer?.cancel()
            expects.clear()
            // Nothing will resume what we paused: it now counts as paused by the user.
            local.values.filter { it.state == ArbiterState.POLICY_PAUSED }.forEach { it.state = ArbiterState.USER_PAUSED }
        } else if (!cfg.autoResume) {
            resumeTimer?.cancel()
        } else {
            scheduleResume()
        }
        changed()
    }

    // ------------------------------------------------------------------------------ observations

    /** The local media sessions (MediaSessionMonitor), after every change. */
    fun onLocalSessions(sessions: List<ArbiterSession>) {
        val baseline = !seenLocal
        seenLocal = true
        val keys = sessions.mapTo(HashSet()) { it.key }
        local.keys.retainAll(keys)
        expects.removeAll { it.key !in keys }
        for (s in sessions) {
            val t = local[s.key]
            if (t == null) {
                val playing = playingLike(s.status)
                local[s.key] = Tracked(s.status, if (playing) ArbiterState.PLAYING else if (pausedLike(s.status)) ArbiterState.USER_PAUSED else ArbiterState.UNKNOWN, s.appId, s.trackId)
                if (playing) {
                    // A player that appears playing was started just now (unless we are starting up).
                    if (baseline) startEpisode(PeerPolicy.CAUSE_USER) else onTransition(s.key, local.getValue(s.key), true)
                }
                continue
            }
            val trackChanged = t.trackId != s.trackId
            t.appId = s.appId
            t.trackId = s.trackId
            // Skipping to another track while paused by us is a decision of the user.
            if (trackChanged && t.state == ArbiterState.POLICY_PAUSED && !playingLike(s.status)) t.state = ArbiterState.USER_PAUSED
            val was = playingLike(t.status)
            val wasPaused = pausedLike(t.status)
            t.status = s.status
            when {
                playingLike(s.status) && !was -> onTransition(s.key, t, true)
                pausedLike(s.status) && !wasPaused -> onTransition(s.key, t, false)
            }
        }
        if (local.values.none { playingLike(it.status) }) cause = PeerPolicy.CAUSE_NONE
        checkExpectations()
        changed()
    }

    /**
     * A command for local session [key] (MediaDuet's UI, or a peer's `media.command`). [policy]: it
     * came from another arbitration engine (the PC's): its effect is not a new user playback.
     */
    fun onLocalCommand(key: String, action: String, policy: Boolean) {
        val playing = when (action) {
            "play" -> true
            "pause", "stop" -> false
            "toggle" -> local[key]?.status?.let { !playingLike(it) } ?: return
            else -> return
        }
        intents.addLast(Intent(key, playing, policy, nowMs()))
        while (intents.size > 16) intents.removeFirst()
        // A user decision about a session we paused ends our claim on it.
        val t = local[key]
        if (!policy && !playing && t?.state == ArbiterState.POLICY_PAUSED) {
            t.state = ArbiterState.USER_PAUSED
            changed()
        }
    }

    /** A call on this device (audio mode in-call / in-communication). */
    fun onLocalCall(active: Boolean) {
        if (call == active) return
        call = active
        changed()
    }

    /** A peer's `policy.peer`. */
    fun onPeer(peerId: String, policy: PeerPolicy) {
        val prev = peers.put(peerId, policy)
        if (prev != null) {
            val onset = policy.playing && policy.cause == PeerPolicy.CAUSE_USER &&
                (!prev.playing || prev.cause != PeerPolicy.CAUSE_USER || prev.onsetAt != policy.onsetAt)
            val callOnset = policy.call && !prev.call
            if (onset || callOnset) peerOnset(peerId, policy, callOnset)
        }
        if (anyPeerBlocking()) resumeTimer?.cancel() else scheduleResume()
        changed()
    }

    /**
     * A peer disconnected. It may still be playing, so this is not a reason to resume (as on
     * Windows when the phone's link drops); the next idle report of a peer is.
     */
    fun onPeerGone(peerId: String) {
        peers.remove(peerId)
    }

    // ------------------------------------------------------------------------------ decisions

    private fun onTransition(key: String, t: Tracked, playing: Boolean) {
        if (playing) {
            if (consumeExpect(key, true)) {
                // Our own resume took effect: not a new playback for the peers.
                t.state = ArbiterState.PLAYING
                if (cause == PeerPolicy.CAUSE_NONE) cause = PeerPolicy.CAUSE_POLICY
                return
            }
            val intent = consumeIntent(key, true)
            t.state = ArbiterState.PLAYING
            if (intent?.policy == true) {
                if (cause == PeerPolicy.CAUSE_NONE) cause = PeerPolicy.CAUSE_POLICY
                return
            }
            // A person started (or resumed) playback here: peers decide whether to yield to it.
            startEpisode(PeerPolicy.CAUSE_USER)
            resumeTimer?.cancel()
            return
        }
        if (consumeExpect(key, false)) {
            t.state = ArbiterState.POLICY_PAUSED
            return
        }
        consumeIntent(key, false)
        // Paused by the user, the app or the PC's arbitration: never resumed by us.
        t.state = ArbiterState.USER_PAUSED
    }

    private fun startEpisode(c: String) {
        cause = c
        onsetAt = wallMs()
    }

    private fun peerOnset(peerId: String, p: PeerPolicy, callOnset: Boolean) {
        resumeTimer?.cancel()
        if (config.mode == PeerPolicy.MODE_OFF) return
        val playing = local.filter { playingLike(it.value.status) }
        if (playing.isEmpty()) return
        val yieldNow = (callOnset && config.pauseForCalls) || shouldYield(p)
        if (!yieldNow) return
        val reason = if (callOnset) REASON_PEER_CALL else REASON_PEER_PLAYING
        pausedAt = wallMs()
        val now = nowMs()
        for ((key, t) in playing) {
            expects += Expect(key, false, now + config.confirmTimeoutMs, reason)
            record("pause", reason, peerId, t.appId)
            execute(ArbiterAction(ArbiterAction.Kind.PAUSE, key, reason))
        }
        checkExpectations()
    }

    /**
     * Whether our playback yields to the peer's new one. Of two playbacks started at about the same
     * time (messages crossing), only the older one yields.
     */
    private fun shouldYield(p: PeerPolicy): Boolean {
        val newer = cause != PeerPolicy.CAUSE_USER || p.onsetAt >= onsetAt
        return when (config.mode) {
            PeerPolicy.MODE_LATEST -> newer
            PeerPolicy.MODE_PRIORITY -> p.priority > config.priority || (p.priority == config.priority && newer)
            else -> false
        }
    }

    private fun anyPeerBlocking(): Boolean = peers.values.any { it.playing || (it.call && config.pauseForCalls) }

    private fun scheduleResume() {
        if (!config.autoResume || config.mode == PeerPolicy.MODE_OFF) return
        if (local.values.none { it.state == ArbiterState.POLICY_PAUSED }) return
        if (anyPeerBlocking()) return
        resumeTimer?.cancel()
        resumeTimer = timer.schedule(config.resumeDelayMs) {
            resumeTimer = null
            tryResume()
            changed()
        }
    }

    private fun tryResume() {
        if (config.mode == PeerPolicy.MODE_OFF || !config.autoResume || anyPeerBlocking()) return
        // Something else plays here now (the user started it): leave the rest alone.
        if (local.values.any { playingLike(it.status) }) return
        val waiting = local.filter { it.value.state == ArbiterState.POLICY_PAUSED && pausedLike(it.value.status) }
        if (waiting.isEmpty()) return
        // The device that paused last resumes first; the others wait until it stops again.
        val first = peers.filter { it.value.pausedAt > 0 && it.value.mode != PeerPolicy.MODE_OFF }
            .any { (id, p) -> p.pausedAt > pausedAt || (p.pausedAt == pausedAt && id > selfId) }
        if (first) return
        val now = nowMs()
        for ((key, t) in waiting) {
            expects += Expect(key, true, now + config.confirmTimeoutMs, REASON_PEERS_IDLE)
            record("resume", REASON_PEERS_IDLE, null, t.appId)
            execute(ArbiterAction(ArbiterAction.Kind.RESUME, key, REASON_PEERS_IDLE))
        }
        checkExpectations()
    }

    // ------------------------------------------------------------------------------ bookkeeping

    private fun consumeExpect(key: String, playing: Boolean): Boolean {
        val i = expects.indexOfFirst { it.key == key && it.playing == playing }
        if (i < 0) return false
        expects.removeAt(i)
        return true
    }

    private fun consumeIntent(key: String, playing: Boolean): Intent? {
        val now = nowMs()
        val i = intents.indexOfLast { it.key == key && it.playing == playing && now - it.at <= config.intentWindowMs }
        if (i < 0) return null
        return intents.removeAt(i)
    }

    private fun checkExpectations() {
        val now = nowMs()
        val expired = expects.filter { it.deadline <= now }
        for (e in expired) {
            expects.remove(e)
            lastAction = lastAction?.copy(failed = true)
            // A resume that did not happen: stop waiting for it, so other devices can go first.
            if (e.playing) local[e.key]?.takeIf { it.state == ArbiterState.POLICY_PAUSED }?.state = ArbiterState.USER_PAUSED
        }
        expectTimer?.cancel()
        expectTimer = null
        val next = expects.minOfOrNull { it.deadline } ?: return
        expectTimer = timer.schedule((next - now).coerceAtLeast(0)) {
            expectTimer = null
            checkExpectations()
            changed()
        }
    }

    private fun record(what: String, reason: String, peerId: String?, app: String) {
        lastAction = ArbiterLastAction(what, reason, peerId, app, wallMs())
    }

    private fun changed() {
        val s = state()
        if (s != published) {
            published = s
            publish(s)
        }
    }

    companion object {
        const val REASON_PEER_PLAYING = "peer_playing"
        const val REASON_PEER_CALL = "peer_call"
        const val REASON_PEERS_IDLE = "peers_idle"

        fun playingLike(status: String) = status == PlayStatus.PLAYING || status == PlayStatus.BUFFERING
        fun pausedLike(status: String) = status == PlayStatus.PAUSED || status == PlayStatus.STOPPED
    }
}
