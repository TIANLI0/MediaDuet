package app.mediaduet.link

import kotlin.math.abs
import kotlin.math.ceil

/**
 * Peer clock estimate for position sync (§11.1), same rules as the PC (`windows/core` ClockSync).
 * Each pong carries the peer's monotonic clock at the moment it answered (`rt`); with our send and
 * receive times that is one NTP-style sample: offset = rt − (t0 + t1) / 2, wrong by at most half the
 * round trip. The sample with the smallest error bound (half its RTT plus worst-case clock drift
 * since it was taken) wins, so one slow ping (Wi-Fi power save, a queue behind an artwork transfer)
 * never moves the estimate.
 *
 * Snapshots carry the sender's clock at which their positions hold (`at`); converting it makes the
 * anchor independent of how long the snapshot itself took to arrive. Not thread-safe: used on the
 * LinkManager's dispatcher.
 */
class ClockSync {
    private class Sample(val at: Long, val rtt: Long, val offset: Long)

    private val samples = ArrayDeque<Sample>()

    /** [t0]: our clock when the ping left, [peerMs]: the peer's clock when it answered, [t1]: ours at the pong. */
    fun addSample(t0: Long, peerMs: Long, t1: Long) {
        val rtt = t1 - t0
        if (rtt !in 0..120_000) return
        val offset = peerMs - (t0 + rtt / 2)
        best(t1)?.let { b ->
            // Both are within their RTT/2 of the truth; further apart means a clock restarted.
            if (abs(offset - b.offset) > (rtt + b.rtt) / 2.0 + 1_000) samples.clear()
        }
        samples.addLast(Sample(t1, rtt, offset))
        if (samples.size > MAX_SAMPLES) samples.removeFirst()
    }

    fun reset() = samples.clear()

    /** Peer clock − our clock, from the best fresh sample (null without one). */
    fun offsetMs(nowMs: Long): Long? = best(nowMs)?.offset

    /** Error bound of [offsetMs] (ms), null without a sample. */
    fun uncertaintyMs(nowMs: Long): Long? = best(nowMs)?.let { ceil(errorBound(it, nowMs)).toLong() }

    /**
     * Our clock at which a snapshot's positions hold: the sender's [peerAt] converted with the clock
     * estimate (never later than [nowMs]); otherwise the legacy estimate nowMs − [fallbackLatencyMs].
     */
    fun anchor(peerAt: Long?, nowMs: Long, fallbackLatencyMs: Long): Long {
        if (peerAt != null) {
            val off = offsetMs(nowMs)
            if (off != null) {
                val local = peerAt - off
                if (local >= nowMs - MAX_TRANSIT_MS) return minOf(local, nowMs)
            }
        }
        return nowMs - fallbackLatencyMs.coerceAtLeast(0)
    }

    private fun errorBound(s: Sample, nowMs: Long) = s.rtt / 2.0 + (nowMs - s.at).coerceAtLeast(0) * DRIFT_PER_MS

    private fun best(nowMs: Long): Sample? =
        samples.filter { nowMs - it.at <= MAX_AGE_MS }.minByOrNull { errorBound(it, nowMs) }

    companion object {
        const val MAX_SAMPLES = 16
        const val MAX_AGE_MS = 10 * 60_000L
        /** Worst-case drift between two device clocks (100 ppm). */
        const val DRIFT_PER_MS = 100e-6
        /** A converted `at` further in the past is not trusted (clock discontinuity). */
        const val MAX_TRANSIT_MS = 10_000L
        /** Extra pings after the primary changes and when media starts, 600 ms apart. */
        const val BURST_PINGS = 5
        const val BURST_GAP_MS = 600L
    }
}
