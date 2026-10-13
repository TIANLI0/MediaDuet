package app.mediaduet.ui

/** Where the user is: Now Playing (home) or the Settings stack pushed on top of it. */
enum class Section { NOW, SETTINGS }

enum class SettingsRoute(val key: String) {
    CONNECTION("connection"),
    PERMISSIONS("permissions"),
    ARBITRATION("arbitration"),
    LYRICS("lyrics"),
    AUDIO("audio"),
    LANGUAGE("language"),
    DIAGNOSTICS("diagnostics"),
    ABOUT("about"),
}

/**
 * The app's navigation state and back-stack rules (pure, unit-tested). There are no tabs: Now
 * Playing is the home screen and Settings is pushed on top of it (with its own sub-pages).
 *
 * Back order: sheet → lyrics mode → settings sub-page → settings root → Now Playing → leave the
 * app (null; the activity moves to the back so its state survives). Lyrics mode and the sheet
 * belong to Now Playing and are kept while Settings is open.
 */
data class NavState(
    val section: Section = Section.NOW,
    val route: SettingsRoute? = null,
    val lyricsMode: Boolean = false,
    val sheetOpen: Boolean = false,
) {
    val settingsOpen: Boolean get() = section == Section.SETTINGS

    /** The state after a system back, or null when back should leave the app. */
    fun back(): NavState? = when {
        settingsOpen && route != null -> copy(route = null)
        settingsOpen -> copy(section = Section.NOW)
        sheetOpen -> copy(sheetOpen = false)
        lyricsMode -> copy(lyricsMode = false)
        else -> null
    }

    /** True when back pops a page of the Settings stack (predictive back animates that page). */
    val backPopsSettingsPage: Boolean get() = settingsOpen

    /** Pushes Settings (at [r], or its root). */
    fun open(r: SettingsRoute?) = copy(section = Section.SETTINGS, route = r, sheetOpen = false)

    /** Returns to Now Playing, closing the whole Settings stack (lyrics mode is kept). */
    fun showPlayer() = copy(section = Section.NOW, route = null, sheetOpen = false)

    fun encode(): String = listOf(VERSION, section.name, route?.name ?: "-", lyricsMode, sheetOpen).joinToString("|")

    companion object {
        private const val VERSION = "v2"

        fun decode(s: String?): NavState {
            val p = s?.split('|') ?: return NavState()
            return runCatching {
                when {
                    p.size == 5 && p[0] == VERSION -> NavState(
                        Section.valueOf(p[1]),
                        p[2].takeIf { it != "-" }?.let { SettingsRoute.valueOf(it) },
                        p[3].toBooleanStrict(),
                        p[4].toBooleanStrict(),
                    )
                    // Round-3 encoding (bottom tabs): the Settings tab becomes the Settings root.
                    p.size == 4 -> when (Section.valueOf(p[0])) {
                        Section.SETTINGS -> NavState(Section.SETTINGS)
                        Section.NOW -> NavState(lyricsMode = p[2].toBooleanStrict(), sheetOpen = p[3].toBooleanStrict())
                    }
                    else -> NavState()
                }
            }.getOrDefault(NavState())
        }
    }
}
