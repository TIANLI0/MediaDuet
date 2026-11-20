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
        // The player in lyrics mode with both sheets open, over a page of the Settings tab.
        var s: NavState? = NavState().open(SettingsRoute.ABOUT).showPlayer().copy(lyricsMode = true, sheetOpen = true, queueOpen = true)
        assertFalse(s!!.backPopsSettingsPage)
        s = s.back()
        assertEquals(NavState(Tab.SETTINGS, SettingsRoute.ABOUT, playerOpen = true, lyricsMode = true, queueOpen = true), s) // source sheet first
        s = s!!.back()
        assertEquals(NavState(Tab.SETTINGS, SettingsRoute.ABOUT, playerOpen = true, lyricsMode = true), s) // then the queue
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
    fun backClosesLyricsOptionsBeforeLyricsMode() {
        val s = NavState(playerOpen = true, lyricsMode = true, lyricsOptionsOpen = true)
        assertEquals(NavState(playerOpen = true, lyricsMode = true), s.back())
        assertEquals(NavState(playerOpen = true, lyricsMode = true, queueOpen = true, lyricsOptionsOpen = true).back(), s) // queue first
        // Transient: not restored after process death, and reset when the player is reopened.
        assertEquals(NavState(playerOpen = true, lyricsMode = true), NavState.decode(s.encode()))
        assertEquals(NavState(playerOpen = true, lyricsMode = true), s.showPlayer())
    }

    @Test
    fun pagesOpenOnTheirTab() {
        assertEquals(NavState(Tab.HOME, SettingsRoute.UNLOCK), NavState(Tab.SETTINGS).open(SettingsRoute.UNLOCK))
        assertEquals(NavState(Tab.SETTINGS, SettingsRoute.PERMISSIONS), NavState().open(SettingsRoute.PERMISSIONS))
        // Connection is the Devices tab itself.
        assertEquals(NavState(Tab.DEVICES), NavState(Tab.HOME, SettingsRoute.AUDIO).open(SettingsRoute.CONNECTION))
        assertEquals(NavState(Tab.SETTINGS), NavState().open(null))
        // Opening a page from the player closes it (lyrics mode is kept for next time).
        assertEquals(NavState(Tab.DEVICES, lyricsMode = true), NavState(playerOpen = true, lyricsMode = true, sheetOpen = true, queueOpen = true).open(SettingsRoute.CONNECTION))
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
            NavState(playerOpen = true, queueOpen = true),
        )) {
            assertEquals(s, NavState.decode(s.encode()))
        }
        assertEquals(NavState(), NavState.decode("garbage"))
        assertEquals(NavState(), NavState.decode(null))
        assertEquals(NavState(), NavState.decode("v4|NOPE|-|false|false|false|false"))
    }

    @Test
    fun olderEncodingsDecode() {
        // v3 (no queue sheet).
        assertEquals(NavState(Tab.DEVICES, playerOpen = true, sheetOpen = true), NavState.decode("v3|DEVICES|-|true|false|true"))
        // v2 (Now Playing was home): the app now opens on Home; a Settings page opens on its tab.
        assertEquals(NavState(lyricsMode = true), NavState.decode("v2|NOW|-|true|false"))
        assertEquals(NavState(Tab.SETTINGS, SettingsRoute.LYRICS), NavState.decode("v2|SETTINGS|LYRICS|false|false"))
        assertEquals(NavState(Tab.DEVICES), NavState.decode("v2|SETTINGS|CONNECTION|false|false"))
        assertEquals(NavState(Tab.SETTINGS), NavState.decode("v2|SETTINGS|-|false|false"))
        assertEquals(NavState(), NavState.decode("SETTINGS|CONNECTION|false|false"))
    }
}
