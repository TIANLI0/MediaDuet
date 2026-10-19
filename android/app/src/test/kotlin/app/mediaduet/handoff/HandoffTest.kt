package app.mediaduet.handoff

import app.mediaduet.media.LocalSessionState
import app.mediaduet.media.PlaybackConst
import app.mediaduet.protocol.CatalogIds
import app.mediaduet.protocol.MediaKind
import app.mediaduet.protocol.MediaSessionInfo
import app.mediaduet.protocol.PlayStatus
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.net.URLDecoder

/** Handoff (§8.7): apps, deep links, catalogue ids and the target match. Mirrors windows/tests/test_handoff.cpp. */
class HandoffTest {
    private fun obj(text: String) = Json.parseToJsonElement(text).jsonObject

    private fun state(key: String, pkg: String, title: String, playing: Boolean = true, duration: Long? = 325_000) = LocalSessionState(
        key, pkg, "app", title, "买辣椒也用券", "", "", duration, true,
        if (playing) PlaybackConst.STATE_PLAYING else PlaybackConst.STATE_PAUSED, 0, 0, 1f, 0, 0, null, 0,
    )

    private val want = HandoffSong("起风了", "买辣椒也用券", "起风了", 325_000)

    @Test
    fun appsOfSessions() {
        assertEquals(HandoffApp.NETEASE, HandoffApp.ofSession("com.netease.cloudmusic"))
        assertEquals(HandoffApp.NETEASE, HandoffApp.ofSession("cloudmusic.exe"))
        assertEquals(HandoffApp.QQMUSIC, HandoffApp.ofSession("QQMusic.exe"))
        assertEquals(HandoffApp.SPOTIFY, HandoffApp.ofSession("SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify"))
        assertNull(HandoffApp.ofSession("com.android.chrome"))
        assertEquals(
            listOf(HandoffApp.SPOTIFY, HandoffApp.NETEASE),
            HandoffApp.listFrom(Json.parseToJsonElement("""["spotify","bogus","netease","spotify",3]""")),
        )
    }

    @Test
    fun deepLinks() {
        assertEquals("orpheus://song/1330348068?switch=true", Handoff.neteaseUri("1330348068"))
        val qq = Handoff.qqMusicUri("003hKob536i6FA")
        assertTrue(qq.startsWith("qqmusic://qq.com/media/playSonglist?p="))
        val p = URLDecoder.decode(qq.substringAfter("?p=").substringBefore("&source="), "UTF-8")
        val json = obj(p)
        assertEquals("play", json["action"]!!.toString().trim('"'))
        assertTrue(p.contains("\"songmid\":\"003hKob536i6FA\""))
        assertTrue(qq.endsWith("&source=https%3A%2F%2Fi.y.qq.com%2Fv8%2Fplaysong.html"))
        assertEquals("spotify:track:4uLU6hMCjMI75M1A2tKUQC", Handoff.spotifyTrackUri("4uLU6hMCjMI75M1A2tKUQC"))
    }

    @Test
    fun catalogueIdsFromMediaIds() {
        assertEquals(CatalogIds(netease = "1330348068"), Handoff.idsFrom("com.netease.cloudmusic", "1330348068"))
        assertNull(Handoff.idsFrom("com.netease.cloudmusic", "local:/sdcard/a.mp3"))
        assertEquals(CatalogIds(qqmusic = "003hKob536i6FA"), Handoff.idsFrom("com.tencent.qqmusic", "003hKob536i6FA"))
        assertNull(Handoff.idsFrom("com.tencent.qqmusic", "293700875")) // a numeric id is not a songmid
        assertEquals(CatalogIds(spotify = "4uLU6hMCjMI75M1A2tKUQC"), Handoff.idsFrom("com.spotify.music", "spotify:track:4uLU6hMCjMI75M1A2tKUQC"))
        assertNull(Handoff.idsFrom("com.spotify.music", "spotify:album:4uLU6hMCjMI75M1A2tKUQC"))
        assertNull(Handoff.idsFrom("com.example.player", "12345"))
        assertNull(Handoff.idsFrom("com.netease.cloudmusic", ""))
    }

    @Test
    fun idsInTheSnapshot() {
        val info = state("com.netease.cloudmusic#1", "com.netease.cloudmusic", "起风了").copy(mediaId = "1330348068").toSessionInfo(0)
        assertEquals(CatalogIds(netease = "1330348068"), info.ids)
        val back = MediaSessionInfo.parse(info.toJson())
        assertEquals(info.ids, back.ids)
        val plain = MediaSessionInfo.parse(state("k", "com.example", "t").toSessionInfo(0).toJson())
        assertNull(plain.ids)
        assertFalse(state("k", "com.example", "t").toSessionInfo(0).toJson().containsKey("ids"))
        // Invalid entries are dropped, an object without valid entries is absent.
        assertEquals(CatalogIds(qqmusic = "003hKob536i6FA"), CatalogIds.parseLenient(obj("""{"netease":"12 34","qqmusic":"003hKob536i6FA"}""")))
        assertNull(CatalogIds.parseLenient(obj("""{"spotify":""}""")))
        assertEquals(MediaKind.MUSIC, info.kind)
    }

    @Test
    fun targetMatch() {
        assertTrue(Handoff.shows("起风了 (原曲：《ヤキモチ》—高桥优)", 325_000, want))
        assertTrue(Handoff.shows("起风了", null, want))
        assertFalse(Handoff.shows("起风了", 200_000, want))
        assertFalse(Handoff.shows("晴天", 325_000, want))
        assertFalse(Handoff.shows("", 325_000, want))
        assertFalse(Handoff.shows("A Song", null, HandoffSong("A", "", "", null)))
        val sessions = listOf(
            state("com.tencent.qqmusic#1", "com.tencent.qqmusic", "起风了", playing = false),
            state("com.netease.cloudmusic#1", "com.netease.cloudmusic", "起风了", playing = false),
            state("com.netease.cloudmusic#2", "com.netease.cloudmusic", "起风了", playing = true),
        )
        assertEquals("com.netease.cloudmusic#2", Handoff.findTarget(sessions, HandoffApp.NETEASE, want)?.key)
        assertNull(Handoff.findTarget(sessions, HandoffApp.SPOTIFY, want))
    }

    @Test
    fun messages() {
        val play = HandoffPlay.parse(obj("""{"app":"qqmusic","song":{"title":"起风了","artist":"买辣椒也用券","album":"","durationMs":325000},"ids":{"qqmusic":"003hKob536i6FA"}}"""))!!
        assertEquals(HandoffApp.QQMUSIC, play.app)
        assertEquals(325_000L, play.song.durationMs)
        assertEquals("003hKob536i6FA", play.ids?.qqmusic)
        assertNull(HandoffPlay.parse(obj("""{"app":"winamp","song":{"title":"x"}}""")))
        assertNull(HandoffPlay.parse(obj("""{"app":"netease","song":{"title":" "}}""")))

        assertEquals("""{"ok":true,"state":"playing","key":"com.tencent.qqmusic#1"}""", PlayResult.playing("com.tencent.qqmusic#1").toJson().toString())
        assertEquals("""{"ok":false,"error":{"code":"not_found"}}""", PlayResult.failed(HandoffErrors.NOT_FOUND).toJson().toString())

        val done = HandoffOutcome.parse(obj("""{"ok":true,"to":"pc","title":"起风了","app":"netease","state":"playing"}"""))
        assertTrue(done.ok)
        assertEquals(HandoffApp.NETEASE, done.app)
        assertEquals("pc", done.to)
        val failed = HandoffOutcome.parse(obj("""{"ok":false,"to":"phone","title":"","app":null,"error":{"code":"not_found"}}"""))
        assertFalse(failed.ok)
        assertEquals(HandoffErrors.NOT_FOUND, failed.error)
        assertEquals(HandoffErrors.TIMEOUT, HandoffOutcome.parse(null).error)
        assertEquals(PlayStatus.PLAYING, state("k", "p", "t").status)
    }
}
