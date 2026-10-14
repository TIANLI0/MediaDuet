package app.mediaduet.remote

import app.mediaduet.lyrics.LyricFormats
import app.mediaduet.lyrics.SongMatch
import app.mediaduet.lyrics.SongMatch.Candidate
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SongMatchTest {
    @Test fun normalizeStripsBracketsAndFeat() {
        assertEquals("晴天", SongMatch.normalize("晴天 (Live)"))
        assertEquals("song", SongMatch.normalize("Song [Remastered 2011] feat. Someone"))
        assertEquals("abc", SongMatch.normalize("A-B C - Live"))
    }

    @Test fun scoreAcceptsExactAndRejectsDurationGap() {
        val want = Candidate("晴天", "周杰伦", "叶惠美", 269_000)
        assertTrue(SongMatch.score(want, Candidate("晴天", "周杰伦", "", 269_500)) >= 0.9)
        assertEquals(0.0, SongMatch.score(want, Candidate("晴天", "周杰伦", "", 280_000)), 0.0)
        assertTrue(SongMatch.score(want, Candidate("晴天", "别人", "", 269_000)) < 0.6)
    }

    @Test fun sameSongIgnoresAlbumAndSmallDurationChanges() {
        val a = Candidate("晴天", "周杰伦", "叶惠美", 269_400)
        assertTrue(SongMatch.sameSong(a, Candidate("晴天", "周杰伦", "", 269_600)))
        assertTrue(SongMatch.sameSong(a, Candidate("晴天", "周杰伦", "叶惠美", null)))
        assertFalse(SongMatch.sameSong(a, Candidate("七里香", "周杰伦", "", 269_400)))
        assertFalse(SongMatch.sameSong(a, Candidate("晴天", "周杰伦", "", 290_000)))
    }

    @Test fun searchQueryDropsSuffixesAndExtraArtists() {
        assertEquals("晴天 周杰伦", SongMatch.searchQuery("晴天（Live）", "周杰伦/五月天"))
    }

    @Test fun yrcWordTiming() {
        val l = LyricFormats.parseYrc(
            """{"t":0,"c":[{"tx":"作词: "}]}
              |[1000,2000](1000,500,0)Hello (1500,500,0)world
              |[4000,1000](4000,1000,0)(yes)""".trimMargin(),
        )
        assertEquals(2, l.lines.size)
        assertEquals("Hello world", l.lines[0].text)
        assertEquals(listOf(1000L, 1500L), l.lines[0].words.map { it.startMs })
        assertEquals("(yes)", l.lines[1].text)
        val lrc = LyricFormats.toLrc(l)
        assertEquals("Hello world", LrcParser.parse(lrc).lines[0].text)
    }

    @Test fun entitiesAndInstrumental() {
        assertEquals("It's <ok> & \"fine\" é", LyricFormats.decodeEntities("It&apos;s &lt;ok&gt; &amp; &quot;fine&quot; &#233;"))
        assertTrue(LyricFormats.isInstrumentalNotice(Lyrics(listOf(LyricLine(0, "纯音乐，请欣赏")))))
    }
}
