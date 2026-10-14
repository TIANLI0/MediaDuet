package app.mediaduet.util

import app.mediaduet.util.VersionCompat.Level
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class VersionCompatTest {
    @Test fun same() = assertEquals(Level.SAME, VersionCompat.compare("1.2.1", "1.2.1"))
    @Test fun suffixesIgnored() = assertEquals(Level.SAME, VersionCompat.compare("1.2.1-beta", "v1.2.1+42"))
    @Test fun twoParts() = assertEquals(Level.SAME, VersionCompat.compare("1.2", "1.2.0"))
    @Test fun patchOnly() = assertEquals(Level.PATCH, VersionCompat.compare("1.2.1", "1.2.3"))
    @Test fun minor() = assertEquals(Level.MISMATCH, VersionCompat.compare("1.2.1", "1.1.9"))
    @Test fun major() = assertEquals(Level.MISMATCH, VersionCompat.compare("2.0.0", "1.2.1"))
    @Test fun unknown() {
        assertEquals(Level.UNKNOWN, VersionCompat.compare("1.2.1", ""))
        assertEquals(Level.UNKNOWN, VersionCompat.compare("1.2.1", "dev"))
        assertNull(VersionCompat.parse("1.x"))
    }
}
