package app.mediaduet.remote

import app.mediaduet.lyrics.LyricsRepository
import app.mediaduet.lyrics.SongMatch
import app.mediaduet.media.Side
import app.mediaduet.protocol.MediaSessionInfo
import app.mediaduet.protocol.TrackId
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.file.Files

/**
 * The QQ Music flicker: the player re-reports the same song with another duration / album, the
 * track id changes, and the lyrics must stay (no lookup, no gap) instead of disappearing.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class LyricsRepositoryTest {
    private fun session(title: String, artist: String, album: String, durationMs: Long?) = MediaSessionInfo(
        key = "qq", appId = "com.tencent.qqmusic", appName = "QQ音乐",
        title = title, artist = artist, album = album, albumArtist = "",
        trackId = TrackId.of(title, artist, album, durationMs),
        durationMs = durationMs, positionMs = 0, rate = 1.0, status = "playing",
        actions = emptyList(), artwork = null, kind = "music",
    )

    private val lyrics = Lyrics(listOf(LyricLine(0, "故事的小黄花"), LyricLine(4_000, "从出生那年就飘着")))

    private fun repo(scope: TestScope) =
        LyricsRepository(scope, Files.createTempDirectory("lyrics").toFile(), MutableStateFlow(false), StandardTestDispatcher(scope.testScheduler))

    @Test fun reReportedSongKeepsItsLyrics() = runTest {
        val repo = repo(this)
        val first = session("晴天", "周杰伦", "叶惠美", 269_000)
        repo.onPcLyrics(first.trackId, LyricsState(first.trackId, lyrics, emptyList(), "qqmusic"), LyricsRepository.songOf(first))

        // Same song, duration reported 0.6 s differently and the album dropped: a new track id.
        val again = session("晴天", "周杰伦", "", 269_600)
        assertTrue(again.trackId != first.trackId)
        assertNotNull("resolved by same-song match before any request", repo.resolve(again))

        repo.request(again, Side.PC, pcConnected = true)
        advanceUntilIdle()
        assertEquals(lyrics, repo.entries.value[again.trackId]?.lyrics)
        assertTrue(repo.pending.value.isEmpty())
    }

    @Test fun differentSongIsNotMatched() = runTest {
        val repo = repo(this)
        val first = session("晴天", "周杰伦", "叶惠美", 269_000)
        repo.onPcLyrics(first.trackId, LyricsState(first.trackId, lyrics, emptyList(), "qqmusic"), LyricsRepository.songOf(first))
        assertNull(repo.resolve(session("七里香", "周杰伦", "七里香", 299_000)))
        assertNull(repo.resolve(session("晴天", "周杰伦", "叶惠美", 300_000)))
    }

    @Test fun phoneLooksUpWhenThePcHasNone() = runTest {
        val repo = repo(this)
        val s = session("Some Song", "Someone", "", 200_000)
        repo.onPcNone(s.trackId, LyricsRepository.songOf(s))
        repo.request(s, Side.PC, pcConnected = true)
        advanceUntilIdle()
        // Online lookup is off in this test: the phone answered "none" (not stuck pending).
        assertEquals(LyricsRepository.SOURCE_NONE, repo.entries.value[s.trackId]?.source)
        assertTrue(repo.pending.value.isEmpty())
        assertEquals(SongMatch.Candidate("Some Song", "Someone", "", 200_000), repo.entries.value[s.trackId]?.song)
    }
}
