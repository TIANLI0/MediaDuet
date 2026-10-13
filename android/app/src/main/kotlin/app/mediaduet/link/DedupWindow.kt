package app.mediaduet.link

/**
 * Receiver de-duplication (§7): remembers the highest peer id and which of the last [size] ids
 * (by value) were seen. Ids older than the window are treated as duplicates - they can only be
 * re-deliveries because senders use strictly increasing ids.
 */
class DedupWindow(private val size: Int = 1024) {
    private val bits = BooleanArray(size)
    var highest: Long = 0
        private set

    /** Returns true if [id] is new (and records it), false if it is a duplicate / too old. */
    fun accept(id: Long): Boolean {
        require(id > 0)
        if (id > highest) {
            val shift = id - highest
            if (shift >= size) {
                bits.fill(false)
            } else {
                var x = highest + 1
                while (x < id) {
                    bits[slot(x)] = false
                    x++
                }
            }
            bits[slot(id)] = true
            highest = id
            return true
        }
        if (highest - id >= size) return false
        val s = slot(id)
        if (bits[s]) return false
        bits[s] = true
        return true
    }

    private fun slot(id: Long): Int = (id % size).toInt()
}
