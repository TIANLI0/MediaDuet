package app.mediaduet.media

import app.mediaduet.protocol.MediaSnapshot

/** Which device the in-app player shows and controls. */
enum class Side { PHONE, PC }

/**
 * The user's choice of control target: [side] null = automatic; [key] null = that side's active
 * session. Persisted as "auto", "pc", "pc:<key>", "phone" or "phone:<key>".
 */
data class TargetChoice(val side: Side?, val key: String? = null) {
    fun encode(): String = when (side) {
        null -> "auto"
        Side.PC -> if (key == null) "pc" else "pc:$key"
        Side.PHONE -> if (key == null) "phone" else "phone:$key"
    }

    companion object {
        val AUTO = TargetChoice(null)

        fun decode(text: String?): TargetChoice {
            if (text.isNullOrEmpty() || text == "auto") return AUTO
            val side = when (text.substringBefore(':')) {
                "pc" -> Side.PC
                "phone" -> Side.PHONE
                else -> return AUTO
            }
            val key = text.substringAfter(':', "").ifEmpty { null }
            return TargetChoice(side, key)
        }
    }
}

/** [fallback]: the PC was chosen but is not connected, so the phone is shown instead. */
data class ResolvedTarget(val side: Side, val key: String, val automatic: Boolean, val fallback: Boolean = false)

/** A play / pause / seek / skip / volume action on [side]/[key] made in MediaDuet's own UI. */
data class UserControl(val side: Side, val key: String, val atMs: Long)

/**
 * Resolver state carried between updates (engine level, monotonic ms):
 * [playingSince] when each source (`pc:<key>` / `phone:<key>`) started playing - kept for a while
 * when a playing source disappears (PC disconnect), so a reconnect is not a "new start";
 * [stoppedAt] when a source was seen to stop playing; [lastActiveAt] when it was last seen playing;
 * [switchedAt] the last automatic switch; [recheckAtMs] when a pending switch must be re-evaluated
 * even without new input (a hold expires).
 */
data class AutoState(
    val target: ResolvedTarget?,
    val playingSince: Map<String, Long> = emptyMap(),
    val stoppedAt: Map<String, Long> = emptyMap(),
    val lastActiveAt: Map<String, Long> = emptyMap(),
    val switchedAt: Long = NEVER,
    val recheckAtMs: Long? = null,
) {
    companion object {
        const val NEVER = Long.MIN_VALUE / 4
    }
}

object TargetResolver {
    /** The user paused / controlled the current target in MediaDuet: keep it this long. */
    const val USER_HOLD_MS = 8_000L
    /** The current target paused by itself: keep it this long before following another source. */
    const val PAUSE_HOLD_MS = 3_000L
    /** At most one automatic switch per this window (flapping sources). */
    const val SWITCH_GUARD_MS = 3_000L
    /** A playing source that disappears (PC disconnect) keeps its start time this long. */
    const val ABSENT_KEEP_MS = 10 * 60_000L

    private fun id(side: Side, key: String) = (if (side == Side.PC) "pc:" else "phone:") + key
    private fun toTarget(id: String) =
        if (id.startsWith("pc:")) ResolvedTarget(Side.PC, id.removePrefix("pc:"), true)
        else ResolvedTarget(Side.PHONE, id.removePrefix("phone:"), true)

    /**
     * Automatic follows **the most recently started playing source**:
     * * a source that starts playing wins immediately (also when it was already playing when Auto
     *   first evaluates - phone sessions then use their own state-change time);
     * * **hold**: when the current target was paused or controlled in MediaDuet within
     *   [USER_HOLD_MS], or paused by itself less than [PAUSE_HOLD_MS] ago, it stays; afterwards a
     *   playing source takes over ([AutoState.recheckAtMs] tells the engine when to re-evaluate);
     * * at most one automatic switch per [SWITCH_GUARD_MS] (play / pause bursts don't ping-pong);
     * * nothing playing: the last target stays (sticky) unless it disappeared, then the most
     *   recently active source; a disconnected PC is never selected automatically.
     *
     * Explicit choices fall back to that side's active session when the chosen session disappeared;
     * a chosen PC that is not connected falls back to the phone's media ([ResolvedTarget.fallback]).
     */
    fun resolve(
        choice: TargetChoice,
        pc: MediaSnapshot?,
        pcConnected: Boolean,
        phone: List<LocalSessionState>,
        previous: AutoState? = null,
        nowMs: Long = 0L,
        userControl: UserControl? = null,
    ): AutoState {
        val pcSessions = if (pcConnected) pc?.sessions.orEmpty() else emptyList()
        val pcActive = if (pcConnected) pc?.activeSession else null
        val phoneActive = SnapshotBuilder.pickActive(phone)
        val present = buildSet {
            pcSessions.forEach { add(id(Side.PC, it.key)) }
            phone.forEach { add(id(Side.PHONE, it.key)) }
        }
        val playing = buildSet {
            pcSessions.filter { it.isPlaying }.forEach { add(id(Side.PC, it.key)) }
            phone.filter { it.isActivelyPlaying }.forEach { add(id(Side.PHONE, it.key)) }
        }

        // ---- per-source timestamps (independent of the choice, so Auto is always up to date) ----
        val prevSince = previous?.playingSince.orEmpty()
        val prevActive = previous?.lastActiveAt.orEmpty()
        val since = HashMap<String, Long>()
        for (p in playing) {
            since[p] = prevSince[p] ?: if (previous == null) {
                // First evaluation: a phone session knows when its state last changed.
                phone.firstOrNull { id(Side.PHONE, it.key) == p }?.lastChangeMs?.takeIf { it in 1..nowMs } ?: nowMs
            } else {
                nowMs
            }
        }
        // A playing source that disappeared (e.g. PC disconnected) keeps its start for a while.
        for ((k, v) in prevSince) {
            if (k !in present && k !in since && nowMs - (prevActive[k] ?: v) < ABSENT_KEEP_MS) since[k] = v
        }
        val stopped = HashMap(previous?.stoppedAt.orEmpty())
        for (k in prevSince.keys) if (k in present && k !in playing) stopped.putIfAbsent(k, nowMs)
        playing.forEach { stopped.remove(it) }
        stopped.keys.retainAll(present)
        val active = HashMap(prevActive)
        playing.forEach { active[it] = nowMs }
        active.entries.removeAll { nowMs - it.value >= ABSENT_KEEP_MS && it.key !in present }

        var switchedAt = previous?.switchedAt ?: AutoState.NEVER
        var recheck: Long? = null
        val target = when (choice.side) {
            null -> {
                val cur = previous?.target?.takeIf { it.automatic && !it.fallback }
                val curId = cur?.let { id(it.side, it.key) }
                val curPresent = curId != null && curId in present
                // Most recently started playing source (ties: the current target, then the PC).
                val best = playing.maxWithOrNull(
                    compareBy<String> { since[it] ?: 0L }.thenBy { it == curId }.thenBy { it.startsWith("pc:") },
                )
                val guardUntil = switchedAt + SWITCH_GUARD_MS
                val holdUntil = if (curId != null && curId !in playing) {
                    maxOf(
                        stopped[curId]?.plus(PAUSE_HOLD_MS) ?: AutoState.NEVER,
                        userControl?.takeIf { id(it.side, it.key) == curId }?.atMs?.plus(USER_HOLD_MS) ?: AutoState.NEVER,
                    )
                } else {
                    AutoState.NEVER
                }
                val chosen = when {
                    cur == null || !curPresent -> best ?: fallback(present, active, phoneActive?.key, pcActive?.key)
                    best == null || best == curId -> curId
                    curId in playing -> if ((since[best] ?: 0L) > (since[curId] ?: 0L) && nowMs >= guardUntil) best else curId
                    nowMs < holdUntil || nowMs < guardUntil -> curId
                    else -> best
                }
                if (chosen != null && curId != null && chosen != curId) switchedAt = nowMs
                // A switch is pending until a hold / the guard expires: ask to be re-evaluated then.
                if (chosen == curId && best != null && best != curId && curPresent) {
                    recheck = listOf(holdUntil, guardUntil).filter { it > nowMs }.minOrNull()
                }
                chosen?.let(::toTarget)
            }
            Side.PC -> when {
                !pcConnected -> phoneActive?.let { ResolvedTarget(Side.PHONE, it.key, automatic = false, fallback = true) }
                else -> (pcSessions.firstOrNull { it.key == choice.key } ?: pcActive)?.let { ResolvedTarget(Side.PC, it.key, false) }
            }
            Side.PHONE -> (phone.firstOrNull { it.key == choice.key } ?: phoneActive)?.let { ResolvedTarget(Side.PHONE, it.key, false) }
        }
        return AutoState(target, since, stopped, active, switchedAt, recheck)
    }

    /** Nothing plays: the most recently active present source, else the phone's, else the PC's. */
    private fun fallback(present: Set<String>, active: Map<String, Long>, phoneActive: String?, pcActive: String?): String? =
        present.filter { it in active }.maxByOrNull { active[it]!! }
            ?: phoneActive?.let { id(Side.PHONE, it) }
            ?: pcActive?.let { id(Side.PC, it) }
}
