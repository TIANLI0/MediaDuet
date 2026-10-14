package app.mediaduet.testplayer

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Shader
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.media.MediaMetadata
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.os.IBinder
import android.os.SystemClock
import android.util.Log
import kotlin.math.PI
import kotlin.math.sin

/**
 * TEST FIXTURE ONLY. A tiny player with a real MediaSession that plays a generated sine tone.
 * It lets the MediaSessionManager path of MediaDuet be verified on an emulator.
 *
 * Intents: action "play" | "pause" | "next" | "previous" | "stop".
 */
class PlayerService : Service() {

    /** [scene]: a text-free dusk cover (screenshots for the website / README). */
    private data class Track(
        val title: String,
        val artist: String,
        val album: String,
        val hz: Double,
        val durationMs: Long,
        val color: Int,
        val scene: Boolean = false,
    )

    private val tracks = listOf(
        Track("Test Tone A4", "MediaDuet Fixture", "Sine Collection", 440.0, 180_000, 0xFF5A7DFF.toInt()),
        Track("Test Tone C5", "MediaDuet Fixture", "Sine Collection", 523.25, 150_000, 0xFFE0559B.toInt()),
        Track("测试音 E5", "MediaDuet 测试", "正弦合集", 659.25, 200_000, 0xFF2DBE8C.toInt()),
        // A real recording's metadata, for the phone's own online lyrics lookup (the tone is still a tone).
        Track("晴天", "周杰伦", "叶惠美", 392.0, 269_000, 0xFF3B6FB6.toInt()),
        // An invented song (original lyrics, see windows/ui/tools/fake_host.py --showcase) for screenshots.
        Track("晚风来信", "林岸", "夜车", 349.23, 212_000, 0xFF3A2C78.toInt(), scene = true),
    )

    private lateinit var session: MediaSession
    private lateinit var audioManager: AudioManager
    private var focusRequest: AudioFocusRequest? = null
    private var index = 0
    @Volatile private var positionMs = 0L
    @Volatile private var playing = false
    private var engine: Thread? = null
    private var foreground = false
    private val liked = HashSet<Int>() // custom action "like" per track (fixture for MediaDuet's player buttons)

    override fun onCreate() {
        super.onCreate()
        audioManager = getSystemService(AudioManager::class.java)
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL, "Test player", NotificationManager.IMPORTANCE_LOW),
        )
        session = MediaSession(this, "MediaDuetTestPlayer").apply {
            setCallback(object : MediaSession.Callback() {
                override fun onPlay() = play()
                override fun onPause() = pause()
                override fun onSkipToNext() = skip(1)
                override fun onSkipToPrevious() = skip(-1)
                override fun onSeekTo(pos: Long) = seek(pos)
                override fun onStop() = stopPlayback()
                override fun onSkipToQueueItem(id: Long) {
                    val wasPlaying = playing
                    pause()
                    index = id.toInt().coerceIn(0, tracks.size - 1)
                    positionMs = 0
                    updateMetadata()
                    if (wasPlaying) play() else updateState()
                    Log.i(TAG, "queue item $id")
                }
                override fun onCustomAction(action: String, extras: android.os.Bundle?) {
                    if (action == ACTION_LIKE) {
                        if (!liked.add(index)) liked.remove(index)
                        Log.i(TAG, "like ${tracks[index].title}: ${index in liked}")
                        updateState()
                    }
                }
            })
            // The play queue for MediaDuet's "Up next": every track twice with the same queue id,
            // like real players that repeat ids (duplicate ids once crashed the queue sheet).
            setQueue((tracks + tracks).mapIndexed { i, t ->
                MediaSession.QueueItem(
                    android.media.MediaDescription.Builder().setMediaId("t$i").setTitle(t.title).setSubtitle(t.artist).build(),
                    (i % tracks.size).toLong(),
                )
            })
            setQueueTitle("Fixture queue")
            isActive = true
        }
        updateMetadata()
        updateState()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        ensureForeground()
        when (intent?.action) {
            "play", null -> play()
            "pause" -> pause()
            "next" -> skip(1)
            "previous" -> skip(-1)
            "stop" -> stopPlayback()
        }
        return START_NOT_STICKY
    }

    private fun ensureForeground() {
        if (foreground) return
        startForeground(1, notification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        foreground = true
    }

    private fun notification(): Notification {
        val t = tracks[index]
        val open = PendingIntent.getActivity(this, 0, Intent(this, PlayerActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_tone)
            .setContentTitle(t.title)
            .setContentText("${t.artist} · test fixture")
            .setContentIntent(open)
            .setStyle(Notification.MediaStyle().setMediaSession(session.sessionToken))
            .setOngoing(playing)
            .build()
    }

    private fun play() {
        if (playing) return
        val req = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(attributes)
            .setOnAudioFocusChangeListener { change ->
                if (change == AudioManager.AUDIOFOCUS_LOSS || change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT) pause()
            }
            .build()
        focusRequest = req
        audioManager.requestAudioFocus(req)
        playing = true
        engine = Thread(::render, "tone").apply { start() }
        updateState()
        Log.i(TAG, "play ${tracks[index].title} at $positionMs ms")
    }

    private fun pause() {
        if (!playing) return
        playing = false
        engine?.join(500)
        engine = null
        focusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
        updateState()
        Log.i(TAG, "pause at $positionMs ms")
    }

    private fun stopPlayback() {
        pause()
        positionMs = 0
        updateState(PlaybackState.STATE_STOPPED)
    }

    private fun skip(delta: Int) {
        val wasPlaying = playing
        pause()
        index = (index + delta + tracks.size) % tracks.size
        positionMs = 0
        updateMetadata()
        if (wasPlaying) play() else updateState()
        Log.i(TAG, "skip to ${tracks[index].title}")
    }

    private fun seek(pos: Long) {
        positionMs = pos.coerceIn(0, tracks[index].durationMs)
        updateState()
        Log.i(TAG, "seek to $positionMs ms")
    }

    private val attributes = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_MEDIA)
        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
        .build()

    /** Writes a quiet sine wave; position advances with the frames actually written. */
    private fun render() {
        val rate = 48_000
        // Devices/emulators without an audio output cannot create an AudioTrack: keep the playback
        // clock running silently so the MediaSession still behaves like a playing player.
        val track = try {
            AudioTrack.Builder()
                .setAudioAttributes(attributes)
                .setAudioFormat(AudioFormat.Builder().setSampleRate(rate).setEncoding(AudioFormat.ENCODING_PCM_16BIT).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
                .setBufferSizeInBytes(rate / 5 * 2)
                .build()
        } catch (e: UnsupportedOperationException) {
            Log.w(TAG, "no audio output; playing silently")
            null
        }
        val chunk = ShortArray(rate / 50) // 20 ms
        var phase = 0.0
        val hz = tracks[index].hz
        track?.play()
        while (playing) {
            if (track != null) {
                for (i in chunk.indices) {
                    chunk[i] = (sin(phase) * 3000).toInt().toShort()
                    phase += 2 * PI * hz / rate
                    if (phase > 2 * PI) phase -= 2 * PI
                }
                track.write(chunk, 0, chunk.size)
            } else {
                Thread.sleep(20)
            }
            positionMs += 20
            if (positionMs >= tracks[index].durationMs) {
                Thread { skip(1) }.start()
                break
            }
        }
        track?.stop()
        track?.release()
    }

    private fun updateMetadata() {
        val t = tracks[index]
        session.setMetadata(
            MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, t.title)
                .putString(MediaMetadata.METADATA_KEY_ARTIST, t.artist)
                .putString(MediaMetadata.METADATA_KEY_ALBUM, t.album)
                .putLong(MediaMetadata.METADATA_KEY_DURATION, t.durationMs)
                .putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, artwork(t))
                .build(),
        )
    }

    private fun artwork(t: Track): Bitmap {
        val size = 640
        val bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        val p = Paint(Paint.ANTI_ALIAS_FLAG)
        if (t.scene) {
            val s = size.toFloat()
            p.shader = LinearGradient(0f, 0f, 0f, s, 0xFF3A2C78.toInt(), 0xFFEC8C78.toInt(), Shader.TileMode.CLAMP)
            c.drawRect(0f, 0f, s, s, p)
            p.shader = null
            p.color = 0xCCFFD6AA.toInt()
            c.drawCircle(s * 0.38f, s * 0.53f, s * 0.195f, p)
            p.color = 0xAAFFF0DC.toInt()
            c.drawCircle(s * 0.61f, s * 0.49f, s * 0.195f, p)
            p.color = 0xFF2C2254.toInt()
            val hills = android.graphics.Path().apply {
                moveTo(0f, s * 0.78f); lineTo(s * 0.27f, s * 0.66f); lineTo(s * 0.59f, s * 0.76f)
                lineTo(s * 0.82f, s * 0.68f); lineTo(s, s * 0.74f); lineTo(s, s); lineTo(0f, s); close()
            }
            c.drawPath(hills, p)
            return bmp
        }
        p.shader = LinearGradient(0f, 0f, size.toFloat(), size.toFloat(), t.color, 0xFF101421.toInt(), Shader.TileMode.CLAMP)
        c.drawRect(0f, 0f, size.toFloat(), size.toFloat(), p)
        p.shader = null
        p.color = 0x55FFFFFF
        c.drawCircle(size * 0.38f, size * 0.5f, size * 0.22f, p)
        c.drawCircle(size * 0.62f, size * 0.5f, size * 0.22f, p)
        p.color = 0xFFFFFFFF.toInt()
        p.textSize = 54f
        c.drawText(t.title, 40f, size - 60f, p)
        return bmp
    }

    private fun updateState(forced: Int? = null) {
        val state = forced ?: if (playing) PlaybackState.STATE_PLAYING else PlaybackState.STATE_PAUSED
        session.setPlaybackState(
            PlaybackState.Builder()
                .setActions(
                    PlaybackState.ACTION_PLAY or PlaybackState.ACTION_PAUSE or PlaybackState.ACTION_PLAY_PAUSE or
                        PlaybackState.ACTION_SKIP_TO_NEXT or PlaybackState.ACTION_SKIP_TO_PREVIOUS or
                        PlaybackState.ACTION_SEEK_TO or PlaybackState.ACTION_STOP,
                )
                .setState(state, positionMs, if (playing) 1f else 0f, SystemClock.elapsedRealtime())
                .setActiveQueueItemId(index.toLong())
                .addCustomAction(
                    PlaybackState.CustomAction.Builder(
                        ACTION_LIKE, if (index in liked) "Liked" else "Like",
                        if (index in liked) R.drawable.ic_heart else R.drawable.ic_heart_outline,
                    ).build(),
                )
                .build(),
        )
        if (foreground) getSystemService(NotificationManager::class.java).notify(1, notification())
        PlayerActivity.onStateChanged?.invoke("${tracks[index].title} — ${if (playing) "playing" else "paused"} @ ${positionMs / 1000}s")
    }

    override fun onDestroy() {
        playing = false
        engine?.join(500)
        session.release()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        private const val TAG = "TestPlayer"
        private const val CHANNEL = "player"
        private const val ACTION_LIKE = "app.mediaduet.testplayer.LIKE"

        fun send(context: Context, action: String) {
            context.startForegroundService(Intent(context, PlayerService::class.java).setAction(action))
        }
    }
}
