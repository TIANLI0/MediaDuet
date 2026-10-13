package app.mediaduet.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class NavStateTest {
    @Test
    fun backStackOrder() {
        // Settings pushed over Now Playing in lyrics mode with the sheet open.
        var s: NavState? = NavState(lyricsMode = true, sheetOpen = true).open(SettingsRoute.CONNECTION)
        assertTrue(s!!.backPopsSettingsPage)
        assertFalse(s.sheetOpen) // pushing Settings closes the sheet
        s = s.back()
        assertEquals(NavState(Section.SETTINGS, null, lyricsMode = true), s) // sub-page -> settings root
        s = s!!.back()
        assertEquals(NavState(lyricsMode = true), s) // settings root -> Now Playing (lyrics kept)
        s = s!!.back()
        assertEquals(NavState(), s) // lyrics -> player
        assertNull(s!!.back()) // Now Playing -> leave the app
        val sheet = NavState(lyricsMode = true, sheetOpen = true)
        assertEquals(NavState(lyricsMode = true), sheet.back()) // sheet first
    }

    @Test
    fun openAndShowPlayer() {
        val sub = NavState().open(SettingsRoute.PERMISSIONS)
        assertEquals(NavState(Section.SETTINGS, SettingsRoute.PERMISSIONS), sub)
        assertEquals(NavState(), sub.showPlayer()) // the whole Settings stack closes
        assertEquals(NavState(Section.SETTINGS), NavState(sheetOpen = true).open(null))
        assertEquals(NavState(lyricsMode = true), NavState(Section.SETTINGS, SettingsRoute.ABOUT, lyricsMode = true).showPlayer())
    }

    @Test
    fun encodingSurvivesProcessDeath() {
        for (s in listOf(NavState(), NavState(Section.SETTINGS, SettingsRoute.AUDIO), NavState(lyricsMode = true, sheetOpen = true))) {
            assertEquals(s, NavState.decode(s.encode()))
        }
        assertEquals(NavState(), NavState.decode("garbage"))
        assertEquals(NavState(), NavState.decode(null))
        assertEquals(NavState(), NavState.decode("v2|NOPE|-|false|false"))
    }

    @Test
    fun roundThreeEncodingsDecode() {
        // Old bottom-tab encodings: the Settings tab (any page) opens the Settings root.
        assertEquals(NavState(Section.SETTINGS), NavState.decode("SETTINGS|CONNECTION|false|false"))
        assertEquals(NavState(Section.SETTINGS), NavState.decode("SETTINGS|-|true|false"))
        assertEquals(NavState(lyricsMode = true), NavState.decode("NOW|-|true|false"))
        assertEquals(NavState(), NavState.decode("NOW|-|maybe|false"))
    }
}
