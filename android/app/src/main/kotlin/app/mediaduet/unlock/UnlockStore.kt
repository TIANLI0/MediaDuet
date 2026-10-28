package app.mediaduet.unlock

import android.app.KeyguardManager
import android.content.Context
import android.os.Build
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyPermanentlyInvalidatedException
import android.security.keystore.KeyProperties
import android.util.AtomicFile
import app.mediaduet.protocol.B64
import app.mediaduet.util.MdLog
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import kotlinx.serialization.json.put
import java.io.File
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** A PC this phone can unlock. */
data class UnlockPc(val enrollment: String, val peerId: String, val pcName: String, val enrolledAtMs: Long, val lastUsedMs: Long)

/**
 * The PCs' unlock keys K (docs/unlock.md). Each K is wrapped (AES-256-GCM) with its own Android
 * Keystore key that needs a fresh user authentication (strong biometric or the device credential)
 * for every use, so K only comes out after the user confirmed in the system prompt. Only the
 * wrapped form is stored (app-private file); the Keystore key dies with new fingerprints or when
 * the lock screen is removed, which makes the enrollment unusable ([Failure.INVALIDATED]).
 */
class UnlockStore(private val context: Context) {
    private val file = AtomicFile(File(context.filesDir, "unlock_pcs.json"))

    private class Entry(val pc: UnlockPc, val iv: ByteArray, val wrapped: ByteArray)

    private val entries = LinkedHashMap<String, Entry>()
    private val _pcs = MutableStateFlow<List<UnlockPc>>(emptyList())
    val pcs: StateFlow<List<UnlockPc>> = _pcs

    enum class Failure(val code: String) {
        NOT_ENROLLED(UnlockErrors.NOT_ENROLLED),
        INVALIDATED(UnlockErrors.KEY_INVALIDATED),
        NO_AUTH(UnlockErrors.NO_AUTH),
        /** The enrollment id belongs to another PC. */
        CONFLICT(UnlockErrors.BAD_MESSAGE),
    }

    class CipherResult(val cipher: Cipher? = null, val failure: Failure? = null)

    init {
        load()
    }

    /** The phone has a secure lock screen (a PIN / pattern / password): required for the keys. */
    fun deviceSecure(): Boolean = context.getSystemService(KeyguardManager::class.java)?.isDeviceSecure == true

    /** A fresh Keystore key for [enrollment] and a cipher to wrap K with, to be authorized by the prompt. */
    @Synchronized
    fun enrollCipher(peerId: String, enrollment: String): CipherResult {
        if (entries[enrollment]?.let { it.pc.peerId != peerId } == true) return CipherResult(failure = Failure.CONFLICT)
        if (!deviceSecure()) return CipherResult(failure = Failure.NO_AUTH)
        return try {
            val ks = keyStore()
            ks.deleteEntry(alias(enrollment))
            val spec = KeyGenParameterSpec.Builder(alias(enrollment), KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .setRandomizedEncryptionRequired(true)
                .setUserAuthenticationRequired(true)
                .setInvalidatedByBiometricEnrollment(true)
                .apply {
                    if (Build.VERSION.SDK_INT >= 30) {
                        setUserAuthenticationParameters(0, KeyProperties.AUTH_BIOMETRIC_STRONG or KeyProperties.AUTH_DEVICE_CREDENTIAL)
                    } else {
                        // Android 10: every use needs a strong biometric (no device-credential fallback with a crypto object).
                        @Suppress("DEPRECATION")
                        setUserAuthenticationValidityDurationSeconds(-1)
                    }
                }
                .build()
            val key = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, ANDROID_KEYSTORE).run {
                init(spec)
                generateKey()
            }
            CipherResult(Cipher.getInstance(TRANSFORMATION).apply { init(Cipher.ENCRYPT_MODE, key) })
        } catch (e: Exception) {
            // No biometric enrolled (Android 10) or no secure lock screen.
            MdLog.w(TAG, "cannot create the unlock key: ${e.javaClass.simpleName} ${e.message}")
            runCatching { keyStore().deleteEntry(alias(enrollment)) }
            CipherResult(failure = Failure.NO_AUTH)
        }
    }

    /**
     * Wraps K with the cipher the user authorized and stores it for [peerId], replacing any
     * earlier enrollment of that PC. False when the cipher was not usable.
     */
    @Synchronized
    fun completeEnroll(cipher: Cipher, enrollment: String, peerId: String, pcName: String, key: ByteArray, nowMs: Long): Boolean {
        val wrapped = try {
            cipher.updateAAD(aad(peerId, enrollment))
            cipher.doFinal(key)
        } catch (e: Exception) {
            MdLog.e(TAG, "cannot wrap the unlock key", e)
            runCatching { keyStore().deleteEntry(alias(enrollment)) }
            return false
        }
        entries.values.filter { it.pc.peerId == peerId && it.pc.enrollment != enrollment }.forEach { drop(it.pc.enrollment) }
        entries[enrollment] = Entry(UnlockPc(enrollment, peerId, pcName, nowMs, 0), cipher.iv, wrapped)
        persist()
        return true
    }

    /**
     * A cipher that unwraps K of [enrollment] once the prompt authorized it; only for the PC that
     * enrolled it. An enrollment whose Keystore key is gone is forgotten ([Failure.INVALIDATED]).
     */
    @Synchronized
    fun unlockCipher(peerId: String, enrollment: String): CipherResult {
        val e = entries[enrollment]?.takeIf { it.pc.peerId == peerId } ?: return CipherResult(failure = Failure.NOT_ENROLLED)
        val cipher = try {
            (keyStore().getKey(alias(enrollment), null) as? SecretKey)?.takeIf { deviceSecure() }?.let { key ->
                Cipher.getInstance(TRANSFORMATION).apply { init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(128, e.iv)) }
            }
        } catch (ex: KeyPermanentlyInvalidatedException) {
            null
        } catch (ex: Exception) {
            MdLog.e(TAG, "cannot use the unlock key of ${e.pc.pcName}", ex)
            null
        }
        if (cipher != null) return CipherResult(cipher)
        MdLog.w(TAG, "unlock key of ${e.pc.pcName} is gone (biometrics or lock screen changed)")
        drop(enrollment)
        persist()
        return CipherResult(failure = Failure.INVALIDATED)
    }

    /** K, unwrapped with the cipher the user authorized; null when that failed. */
    @Synchronized
    fun unwrap(cipher: Cipher, peerId: String, enrollment: String, nowMs: Long): ByteArray? {
        val e = entries[enrollment]?.takeIf { it.pc.peerId == peerId } ?: return null
        return try {
            cipher.updateAAD(aad(peerId, enrollment))
            cipher.doFinal(e.wrapped).also {
                entries[enrollment] = Entry(e.pc.copy(lastUsedMs = nowMs), e.iv, e.wrapped)
                persist()
            }
        } catch (ex: Exception) {
            MdLog.e(TAG, "cannot unwrap the unlock key of ${e.pc.pcName}", ex)
            null
        }
    }

    /** Forgets [enrollment] (any PC); returns the PC it belonged to. */
    @Synchronized
    fun remove(enrollment: String): UnlockPc? {
        val pc = entries[enrollment]?.pc ?: return null
        drop(enrollment)
        persist()
        return pc
    }

    /** Forgets [enrollment] only if it belongs to [peerId] (`unlock.revoke` from that PC). */
    @Synchronized
    fun remove(peerId: String, enrollment: String): Boolean {
        if (entries[enrollment]?.pc?.peerId != peerId) return false
        drop(enrollment)
        persist()
        return true
    }

    /** An enrollment of [peerId] that did not complete: drops its Keystore key (and any earlier entry under that id). */
    @Synchronized
    fun discard(peerId: String, enrollment: String) {
        if (entries[enrollment]?.let { it.pc.peerId != peerId } == true) return
        val had = entries.containsKey(enrollment)
        drop(enrollment)
        if (had) persist()
    }

    /** Forgets the enrollments of PCs that are no longer paired. */
    @Synchronized
    fun retainPeers(peerIds: Set<String>) {
        val gone = entries.values.filter { it.pc.peerId !in peerIds }.map { it.pc.enrollment }
        if (gone.isEmpty()) return
        MdLog.i(TAG, "removing ${gone.size} unlock enrollment(s) of unpaired PCs")
        gone.forEach { drop(it) }
        persist()
    }

    private fun drop(enrollment: String) {
        entries.remove(enrollment)
        runCatching { keyStore().deleteEntry(alias(enrollment)) }
    }

    private fun keyStore(): KeyStore = KeyStore.getInstance(ANDROID_KEYSTORE).apply { load(null) }

    private fun alias(enrollment: String) = "$ALIAS_PREFIX$enrollment"

    private fun aad(peerId: String, enrollment: String) = "$peerId|$enrollment".toByteArray(Charsets.US_ASCII)

    private fun load() {
        try {
            if (!file.baseFile.exists()) return
            val root = Json.parseToJsonElement(String(file.readFully(), Charsets.UTF_8)).jsonObject
            for (item in root["pcs"]?.jsonArray.orEmpty()) {
                val o = item.jsonObject
                val pc = UnlockPc(
                    enrollment = o["enrollment"]!!.jsonPrimitive.content,
                    peerId = o["peer"]!!.jsonPrimitive.content,
                    pcName = o["name"]?.jsonPrimitive?.content ?: "",
                    enrolledAtMs = o["enrolledAt"]?.jsonPrimitive?.long ?: 0,
                    lastUsedMs = o["lastUsed"]?.jsonPrimitive?.long ?: 0,
                )
                entries[pc.enrollment] = Entry(pc, B64.decode(o["iv"]!!.jsonPrimitive.content), B64.decode(o["key"]!!.jsonPrimitive.content))
            }
        } catch (e: Exception) {
            MdLog.e(TAG, "unlock store unreadable; starting empty", e)
            entries.clear()
        }
        _pcs.value = entries.values.map { it.pc }
    }

    private fun persist() {
        val root = buildJsonObject {
            put("version", 1)
            put("pcs", JsonArray(entries.values.map { e ->
                buildJsonObject {
                    put("enrollment", e.pc.enrollment)
                    put("peer", e.pc.peerId)
                    put("name", e.pc.pcName)
                    put("enrolledAt", e.pc.enrolledAtMs)
                    put("lastUsed", e.pc.lastUsedMs)
                    put("iv", B64.encode(e.iv))
                    put("key", B64.encode(e.wrapped))
                }
            }))
        }
        val out = file.startWrite()
        try {
            out.write(Json.encodeToString(JsonObject.serializer(), root).toByteArray(Charsets.UTF_8))
            file.finishWrite(out)
        } catch (e: Exception) {
            file.failWrite(out)
            MdLog.e(TAG, "cannot persist the unlock store", e)
        }
        _pcs.value = entries.values.map { it.pc }
    }

    companion object {
        private const val TAG = "MD/Unlock"
        private const val ANDROID_KEYSTORE = "AndroidKeyStore"
        private const val TRANSFORMATION = "AES/GCM/NoPadding"
        private const val ALIAS_PREFIX = "mediaduet.unlock.v1."
    }
}
