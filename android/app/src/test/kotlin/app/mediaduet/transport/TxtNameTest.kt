package app.mediaduet.transport

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class TxtNameTest {
    @Test
    fun encodesAndTruncatesWithoutSplittingEscapes() {
        assertEquals("Tianli%27s%20PC", NsdAdvertiser.encodeTxtName("Tianli's PC"))
        val zh = NsdAdvertiser.encodeTxtName("田力的小米手机田力的小米手机")
        assertTrue(zh.length <= 40)
        // Every escape is complete: a multiple of 9 chars (3 bytes x "%XX") for CJK.
        assertEquals(0, zh.length % 9)
        assertTrue(NsdAdvertiser.encodeTxtName("x".repeat(100)).length == 40)
    }
}
