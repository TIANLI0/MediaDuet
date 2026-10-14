package app.mediaduet.media

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** NetEase Cloud Music publishes playlists / campaigns as custom actions; only real controls pass. */
class CustomActionFilterTest {
    @Test
    fun controlsPass() {
        for (t in listOf("喜欢", "收藏", "Like", "Favourite", "随机播放", "单曲循环", "收藏到歌单", "Repeat one", "词", "")) {
            assertTrue(t, CustomActionFilter.isControl(t, hasIcon = true))
        }
    }

    @Test
    fun promotionsAreDropped() {
        // From the user's screenshot (NetEase Cloud Music on Android).
        assertFalse(CustomActionFilter.isControl("中文乐队 | 自驾必备", hasIcon = true))
        assertFalse(CustomActionFilter.isControl("2026洛天依纯蓝演唱会", hasIcon = true))
        assertFalse(CustomActionFilter.isControl("2026", hasIcon = true))
        assertFalse(CustomActionFilter.isControl("Daily Mix · Chill", hasIcon = true))
        assertFalse(CustomActionFilter.isControl("洛天依纯蓝演唱会", hasIcon = true)) // too long for a control
        // Text-only entries are never controls.
        assertFalse(CustomActionFilter.isControl("喜欢", hasIcon = false))
    }
}
