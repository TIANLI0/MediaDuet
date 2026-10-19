package app.mediaduet.update

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** GitHub release parsing, version order, checksums. Mirrors windows/tests/test_update.cpp. */
class UpdateInfoTest {
    private val sample = Json.parseToJsonElement(
        """{"tag_name":"v1.5.0","draft":false,"prerelease":false,
            "html_url":"https://github.com/TIANLI0/MediaDuet/releases/tag/v1.5.0","body":"notes",
            "assets":[
              {"name":"MediaDuet-1.5.0-android.apk","size":2860798,"browser_download_url":"https://github.com/TIANLI0/MediaDuet/releases/download/v1.5.0/MediaDuet-1.5.0-android.apk"},
              {"name":"evil.apk","browser_download_url":"http://example.com/evil.apk"},
              {"name":"SHA256SUMS.txt","size":284,"browser_download_url":"https://github.com/TIANLI0/MediaDuet/releases/download/v1.5.0/SHA256SUMS.txt"}]}""",
    ).jsonObject

    private fun with(key: String, value: kotlinx.serialization.json.JsonElement) = JsonObject(sample + (key to value))

    @Test
    fun parsesTheLatestRelease() {
        val r = UpdateInfo.parseRelease(sample)!!
        assertEquals("1.5.0", r.version)
        assertEquals("https://github.com/TIANLI0/MediaDuet/releases/tag/v1.5.0", r.page)
        assertEquals(2, r.assets.size) // the http link is dropped
        assertEquals(2860798L, r.find(UpdateInfo.androidAsset("1.5.0"))?.size)
        assertNotNull(r.find(UpdateInfo.CHECKSUMS_ASSET))
        assertNull(UpdateInfo.parseRelease(with("draft", JsonPrimitive(true))))
        assertNull(UpdateInfo.parseRelease(with("prerelease", JsonPrimitive(true))))
        assertNull(UpdateInfo.parseRelease(with("tag_name", JsonPrimitive("nightly"))))
        assertNull(UpdateInfo.parseRelease(null))
    }

    @Test
    fun versionOrder() {
        assertTrue(UpdateInfo.isNewer("1.5.0", "1.4.1"))
        assertTrue(UpdateInfo.isNewer("v1.4.2", "1.4.1"))
        assertTrue(UpdateInfo.isNewer("2.0.0", "1.99.99"))
        assertTrue(UpdateInfo.isNewer("1.5", "1.4.9"))
        assertFalse(UpdateInfo.isNewer("1.4.1", "1.4.1"))
        assertFalse(UpdateInfo.isNewer("1.4.1", "1.4.1-debug"))
        assertFalse(UpdateInfo.isNewer("1.4.0", "1.4.1"))
        assertFalse(UpdateInfo.isNewer("garbage", "1.4.1"))
        assertEquals("MediaDuet-1.5.0-android.apk", UpdateInfo.androidAsset("1.5.0"))
    }

    @Test
    fun checksums() {
        val sums = "8e4f1a63b7c1d0f2a9e5b6c7d8e9f0a1b2c3d4e5f60718293a4b5c6d7e8f9a0b  MediaDuet-1.5.0-android.apk\n" +
            "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789 *MediaDuet-1.5.0-x64-setup.exe\r\n" +
            "1234  short.txt\n"
        assertEquals("8e4f1a63b7c1d0f2a9e5b6c7d8e9f0a1b2c3d4e5f60718293a4b5c6d7e8f9a0b", UpdateInfo.checksumFor(sums, "MediaDuet-1.5.0-android.apk"))
        assertEquals("abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789", UpdateInfo.checksumFor(sums, "MediaDuet-1.5.0-x64-setup.exe"))
        assertNull(UpdateInfo.checksumFor(sums, "short.txt"))
        assertNull(UpdateInfo.checksumFor("", "x"))
    }
}
