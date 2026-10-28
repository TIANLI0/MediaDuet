package app.mediaduet.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class NavStateTest {
    @Test
    fun opensOnHome() {
        assertEquals(Tab.HOME, NavState().tab)
        assertFalse(NavState().playerOpen)
    }

    @Test
    fun backStackOrder() {
        // The player in lyrics mode with the sheet open, over a page of the Settings tab.
        var s: NavState? = NavState().open(SettingsRoute.ABOUT).showPlayer().copy(lyricsMode = true, sheetOpen = true)
        assertFalse(s!!.backPopsSettingsPage)
        s = s.back()
        assertEquals(NavState(Tab.SETTINGS, SettingsRoute.ABOUT, playerOpen = true, lyricsMode = true), s) // sheet first
        s = s!!.back()
        assertEquals(NavState(Tab.SETTINGS, SettingsRoute.ABOUT, playerOpen = true), s) // lyrics -> player
        s = s!!.back()
        assertEquals(NavState(Tab.SETTINGS, SettingsRoute.ABOUT), s) // player closes
        assertTrue(s!!.backPopsSettingsPage)
        s = s.back()
        assertEquals(NavState(Tab.SETTINGS), s) // page -> tab root
        s = s!!.back()
        assertEquals(NavState(), s) // other tab -> Home
        assertNull(s!!.back()) // Home -> leave the app
    }

    @Test
    fun pagesOpenOnTheirTab() {
        assertEquals(NavState(Tab.HOME, SettingsRoute.UNLOCK), NavState(Tab.SETTINGS).open(SettingsRoute.UNLOCK))
        assertEquals(NavState(Tab.SETTINGS, SettingsRoute.PERMISSIONS), NavState().open(SettingsRoute.PERMISSIONS))
        // Connection is the Devices tab itself.
        assertEquals(NavState(Tab.DEVICES), NavState(Tab.HOME, SettingsRoute.AUDIO).open(SettingsRoute.CONNECTION))
        assertEquals(NavState(Tab.SETTINGS), NavState().open(null))
        // Opening a page from the player closes it (lyrics mode is kept for next time).
        assertEquals(NavState(Tab.DEVICES, lyricsMode = true), NavState(playerOpen = true, lyricsMode = true, sheetOpen = true).open(SettingsRoute.CONNECTION))
    }

    @Test
    fun selectingTabs() {
        assertEquals(NavState(Tab.DEVICES), NavState(Tab.HOME, SettingsRoute.AUDIO).select(Tab.DEVICES))
        assertEquals(NavState(Tab.HOME), NavState(Tab.HOME, SettingsRoute.AUDIO).select(Tab.HOME)) // re-tap pops the page
        assertEquals(NavState(playerOpen = true), NavState().showPlayer())
    }

    @Test
    fun encodingSurvivesProcessDeath() {
        for (s in listOf(
            NavState(),
            NavState(Tab.SETTINGS, SettingsRoute.AUDIO),
            NavState(Tab.DEVICES, playerOpen = true, lyricsMode = true, sheetOpen = true),
        )) {
            assertEquals(s, NavState.decode(s.encode()))
        }
        assertEquals(NavState(), NavState.decode("garbage"))
        assertEquals(NavState(), NavState.decode(null))
        assertEquals(NavState(), NavState.decode("v3|NOPE|-|false|false|false"))
    }

    @Test
    fun olderEncodingsDecode() {
        // v2 (Now Playing was home): the app now opens on Home; a Settings page opens on its tab.
        assertEquals(NavState(lyricsMode = true), NavState.decode("v2|NOW|-|true|false"))
        assertEquals(NavState(Tab.SETTINGS, SettingsRoute.LYRICS), NavState.decode("v2|SETTINGS|LYRICS|false|false"))
        assertEquals(NavState(Tab.DEVICES), NavState.decode("v2|SETTINGS|CONNECTION|false|false"))
        assertEquals(NavState(Tab.SETTINGS), NavState.decode("v2|SETTINGS|-|false|false"))
        assertEquals(NavState(), NavState.decode("SETTINGS|CONNECTION|false|false"))
    }
}
