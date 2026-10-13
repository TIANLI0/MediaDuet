package app.mediaduet.media

import android.content.ContentResolver
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.core.graphics.scale
import android.media.MediaMetadata
import android.net.Uri
import android.util.LruCache
import app.mediaduet.crypto.sha256
import app.mediaduet.protocol.ArtworkRef
import app.mediaduet.protocol.Hex
import app.mediaduet.protocol.Limits
import app.mediaduet.util.MdLog
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import java.io.ByteArrayOutputStream

/**
 * Turns session artwork (bitmap or URI from MediaMetadata) into a ≤512 px JPEG (q85), addressed
 * by SHA-256 (§8.6). Encoded images live in an LRU cache and are served for `artwork.get`.
 */
class ArtworkProcessor(private val context: Context, private val scope: CoroutineScope) {

    private val cache = object : LruCache<String, ByteArray>(8 * 1024 * 1024) {
        override fun sizeOf(key: String, value: ByteArray) = value.size
    }

    private class SessionArt(val fingerprint: String, val ref: ArtworkRef?)

    private val perSession = HashMap<String, SessionArt>()
    private val jobs = HashMap<String, Job>()

    /** Encoded JPEG for [hash], or null when unknown / evicted. */
    fun bytes(hash: String): ByteArray? = cache.get(hash)

    @Synchronized
    fun refFor(sessionKey: String): ArtworkRef? = perSession[sessionKey]?.ref?.takeIf { cache.get(it.hash) != null }

    @Synchronized
    fun forget(sessionKey: String) {
        perSession.remove(sessionKey)
        jobs.remove(sessionKey)?.cancel()
    }

    /**
     * Processes the artwork of [metadata] for [sessionKey] in the background; [onReady] is invoked
     * when the session's artwork reference changed.
     */
    @Synchronized
    fun update(sessionKey: String, metadata: MediaMetadata?, onReady: () -> Unit) {
        val bitmap = metadata?.let {
            runCatching {
                it.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART)
                    ?: it.getBitmap(MediaMetadata.METADATA_KEY_ART)
                    ?: it.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON)
            }.getOrNull()?.takeIf { b -> !b.isRecycled && b.width > 0 && b.height > 0 }
        }
        val uri = if (bitmap == null) metadata?.let {
            (it.getString(MediaMetadata.METADATA_KEY_ALBUM_ART_URI)
                ?: it.getString(MediaMetadata.METADATA_KEY_ART_URI)
                ?: it.getString(MediaMetadata.METADATA_KEY_DISPLAY_ICON_URI))?.takeIf { s -> s.isNotBlank() }
        } else null

        if (bitmap == null && uri == null) {
            if (perSession[sessionKey]?.ref != null) {
                perSession[sessionKey] = SessionArt("none", null)
                onReady()
            } else {
                perSession[sessionKey] = SessionArt("none", null)
            }
            return
        }
        jobs.remove(sessionKey)?.cancel()
        jobs[sessionKey] = scope.launch(Dispatchers.Default) {
            val fingerprint = try {
                if (bitmap != null) fingerprint(bitmap) else "uri:$uri"
            } catch (e: Exception) {
                return@launch
            } catch (e: OutOfMemoryError) {
                MdLog.w(TAG, "artwork for $sessionKey too large")
                return@launch
            }
            synchronized(this@ArtworkProcessor) {
                val current = perSession[sessionKey]
                if (current?.fingerprint == fingerprint && (current.ref == null || cache.get(current.ref.hash) != null)) return@launch
            }
            val ref = try {
                val source = bitmap ?: loadUri(Uri.parse(uri))
                source?.let { encode(it) }
            } catch (e: Exception) {
                MdLog.w(TAG, "artwork for $sessionKey unavailable: ${e.javaClass.simpleName}")
                null
            } catch (e: OutOfMemoryError) {
                MdLog.w(TAG, "artwork for $sessionKey too large")
                null
            }
            val changed = synchronized(this@ArtworkProcessor) {
                val previous = perSession[sessionKey]?.ref
                perSession[sessionKey] = SessionArt(fingerprint, ref)
                previous != ref
            }
            if (changed) onReady()
        }
    }

    private fun fingerprint(bitmap: Bitmap): String {
        val small = softwareCopy(bitmap).scale(16, 16, true)
        val pixels = IntArray(256)
        small.getPixels(pixels, 0, 16, 0, 0, 16, 16)
        val bytes = ByteArray(pixels.size * 4)
        for (i in pixels.indices) {
            val p = pixels[i]
            bytes[i * 4] = (p ushr 24).toByte()
            bytes[i * 4 + 1] = (p ushr 16).toByte()
            bytes[i * 4 + 2] = (p ushr 8).toByte()
            bytes[i * 4 + 3] = p.toByte()
        }
        return "bmp:${bitmap.width}x${bitmap.height}:" + Hex.encode(sha256(bytes)).take(24)
    }

    private fun softwareCopy(bitmap: Bitmap): Bitmap =
        if (bitmap.config == Bitmap.Config.HARDWARE) bitmap.copy(Bitmap.Config.ARGB_8888, false) else bitmap

    private fun loadUri(uri: Uri): Bitmap? {
        // Only local content is read; remote http(s) artwork URLs are not fetched.
        if (uri.scheme !in setOf(ContentResolver.SCHEME_CONTENT, ContentResolver.SCHEME_ANDROID_RESOURCE, ContentResolver.SCHEME_FILE)) {
            return null
        }
        val resolver = context.contentResolver
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        resolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, bounds) } ?: return null
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
        var sample = 1
        while (maxOf(bounds.outWidth, bounds.outHeight) / (sample * 2) >= MAX_EDGE) sample *= 2
        val opts = BitmapFactory.Options().apply { inSampleSize = sample }
        return resolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, opts) }
    }

    private fun encode(source: Bitmap): ArtworkRef? {
        val bitmap = softwareCopy(source)
        if (bitmap.width <= 0 || bitmap.height <= 0) return null
        val scale = minOf(1f, MAX_EDGE.toFloat() / maxOf(bitmap.width, bitmap.height))
        val scaled = if (scale < 1f) {
            bitmap.scale((bitmap.width * scale).toInt().coerceAtLeast(1), (bitmap.height * scale).toInt().coerceAtLeast(1), true)
        } else bitmap
        var quality = 85
        var bytes: ByteArray
        do {
            val out = ByteArrayOutputStream(64 * 1024)
            scaled.compress(Bitmap.CompressFormat.JPEG, quality, out)
            bytes = out.toByteArray()
            quality -= 15
        } while (bytes.size > Limits.MAX_ARTWORK_SIZE && quality > 20)
        if (bytes.size > Limits.MAX_ARTWORK_SIZE) return null
        val hash = Hex.encode(sha256(bytes))
        cache.put(hash, bytes)
        return ArtworkRef(hash, "image/jpeg", bytes.size)
    }

    companion object {
        private const val TAG = "MD/Artwork"
        const val MAX_EDGE = 512
    }
}
