package app.mediaduet.ui

/** The bottom tabs. */
enum class Tab { HOME, DEVICES, SETTINGS }

/** Pages pushed on a tab. [CONNECTION] is the Devices tab itself (it has no root list). */
enum class SettingsRoute(val key: String, val tab: Tab) {
    CONNECTION("connection", Tab.DEVICES),
    UNLOCK("unlock", Tab.HOME),
    ARBITRATION("arbitration", Tab.HOME),
    AUDIO("audio", Tab.HOME),
    PERMISSIONS("permissions", Tab.SETTINGS),
    LYRICS("lyrics", Tab.SETTINGS),
    LANGUAGE("language", Tab.SETTINGS),
    DIAGNOSTICS("diagnostics", Tab.SETTINGS),
    ABOUT("about", Tab.SETTINGS),
}

/**
 * The app's navigation state and back-stack rules (pure, unit-tested). Three bottom tabs (Home,
 * Devices, Settings; the app opens on Home), each with at most one pushed page, and the full
 * player as a layer over all of them (opened from the mini player above the tab bar).
 *
 * Back order: sheet → lyrics mode → player → pushed page → Home tab → leave the app (null; the
 * activity moves to the back so its state survives). Lyrics mode belongs to the player and is
 * kept while it is closed.
 */
data class NavState(
    val tab: Tab = Tab.HOME,
    val route: SettingsRoute? = null,
    val playerOpen: Boolean = false,
    val lyricsMode: Boolean = false,
    val sheetOpen: Boolean = false,
) {
    /** The state after a system back, or null when back should leave the app. */
    fun back(): NavState? = when {
        playerOpen && sheetOpen -> copy(sheetOpen = false)
        playerOpen && lyricsMode -> copy(lyricsMode = false)
        playerOpen -> copy(playerOpen = false)
        route != null -> copy(route = null)
        tab != Tab.HOME -> copy(tab = Tab.HOME)
        else -> null
    }

    /** True when back pops a pushed page (predictive back animates that page). */
    val backPopsSettingsPage: Boolean get() = !playerOpen && route != null

    /** Opens page [r] on its tab ([SettingsRoute.CONNECTION]: the Devices tab), or the Settings tab (null); closes the player. */
    fun open(r: SettingsRoute?) = when (r) {
        null -> copy(tab = Tab.SETTINGS, route = null, playerOpen = false, sheetOpen = false)
        SettingsRoute.CONNECTION -> copy(tab = Tab.DEVICES, route = null, playerOpen = false, sheetOpen = false)
        else -> copy(tab = r.tab, route = r, playerOpen = false, sheetOpen = false)
    }

    /** Tapping a tab: switches to it at its root (tapping the current tab pops its page). */
    fun select(t: Tab) = copy(tab = t, route = null)

    /** Opens the full player over the tabs. */
    fun showPlayer() = copy(playerOpen = true, sheetOpen = false)

    fun encode(): String = listOf(VERSION, tab.name, route?.name ?: "-", playerOpen, lyricsMode, sheetOpen).joinToString("|")

    companion object {
        private const val VERSION = "v3"

        fun decode(s: String?): NavState {
            val p = s?.split('|') ?: return NavState()
            return runCatching {
                when {
                    p.size == 6 && p[0] == VERSION -> NavState(
                        Tab.valueOf(p[1]),
                        p[2].takeIf { it != "-" }?.let { SettingsRoute.valueOf(it) },
                        p[3].toBooleanStrict(),
                        p[4].toBooleanStrict(),
                        p[5].toBooleanStrict(),
                    )
                    // v2 (no tabs: Now Playing was the home screen): open on Home, a Settings page where it belongs.
                    p.size == 5 && p[0] == "v2" -> when (p[1]) {
                        "SETTINGS" -> NavState().open(p[2].takeIf { it != "-" }?.let { SettingsRoute.valueOf(it) })
                        else -> NavState(lyricsMode = p[3].toBooleanStrict())
                    }
                    else -> NavState()
                }
            }.getOrDefault(NavState())
        }
    }
}
