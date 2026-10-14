package app.mediaduet.crypto

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.AtomicFile
import app.mediaduet.link.TrustStore
import app.mediaduet.link.TrustedPeer
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

/**
 * Pairing keys K, each wrapped (AES-256-GCM) with a non-exportable Android Keystore key. Only the
 * wrapped form is written to app-private storage; plaintext keys exist in memory only while a
 * handshake needs them.
 */
class KeystoreTrustStore(context: Context) : TrustStore {
    private val file = AtomicFile(File(context.filesDir, "trusted_peers.json"))

    private class Entry(val peer: TrustedPeer, val iv: ByteArray, val wrapped: ByteArray)

    private val entries = LinkedHashMap<String, Entry>()
    private val _peers = MutableStateFlow<List<TrustedPeer>>(emptyList())
    override val peers: StateFlow<List<TrustedPeer>> = _peers

    init {
        load()
    }

    private fun wrappingKey(): SecretKey {
        val ks = KeyStore.getInstance(ANDROID_KEYSTORE).apply { load(null) }
        (ks.getKey(ALIAS, null) as? SecretKey)?.let { return it }
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, ANDROID_KEYSTORE)
        generator.init(
            KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .setRandomizedEncryptionRequired(true)
                .build(),
        )
        return generator.generateKey()
    }

    @Synchronized
    override fun key(peerId: String): ByteArray? {
        val e = entries[peerId] ?: return null
        return try {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, wrappingKey(), GCMParameterSpec(128, e.iv))
            cipher.updateAAD(peerId.toByteArray(Charsets.US_ASCII))
            cipher.doFinal(e.wrapped)
        } catch (ex: Exception) {
            // Keystore key lost (backup restored to another device, keystore reset): the pairing is
            // unusable and the peer will receive not_paired.
            MdLog.e(TAG, "cannot unwrap key for $peerId", ex)
            null
        }
    }

    @Synchronized
    override fun add(peer: TrustedPeer, key: ByteArray) {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, wrappingKey())
        cipher.updateAAD(peer.id.toByteArray(Charsets.US_ASCII))
        val wrapped = cipher.doFinal(key)
        entries[peer.id] = Entry(peer, cipher.iv, wrapped)
        persist()
    }

    @Synchronized
    override fun remove(peerId: String) {
        if (entries.remove(peerId) != null) persist()
    }

    @Synchronized
    override fun updateSeen(peerId: String, name: String, seenAtMs: Long, appVersion: String) {
        val e = entries[peerId] ?: return
        val version = appVersion.ifEmpty { e.peer.appVersion }
        entries[peerId] = Entry(e.peer.copy(name = name, lastSeenMs = seenAtMs, appVersion = version), e.iv, e.wrapped)
        persist()
    }

    private fun load() {
        try {
            if (!file.baseFile.exists()) return
            val root = Json.parseToJsonElement(String(file.readFully(), Charsets.UTF_8)).jsonObject
            for (item in root["peers"]?.jsonArray.orEmpty()) {
                val o = item.jsonObject
                val peer = TrustedPeer(
                    id = o["id"]!!.jsonPrimitive.content,
                    name = o["name"]!!.jsonPrimitive.content,
                    platform = o["platform"]?.jsonPrimitive?.content ?: "",
                    pairedAtMs = o["pairedAt"]?.jsonPrimitive?.long ?: 0,
                    lastSeenMs = o["lastSeen"]?.jsonPrimitive?.long ?: 0,
                    appVersion = o["app"]?.jsonPrimitive?.content ?: "",
                )
                entries[peer.id] = Entry(peer, B64.decode(o["iv"]!!.jsonPrimitive.content), B64.decode(o["key"]!!.jsonPrimitive.content))
            }
        } catch (e: Exception) {
            MdLog.e(TAG, "trust store unreadable; starting empty", e)
            entries.clear()
        }
        _peers.value = entries.values.map { it.peer }
    }

    private fun persist() {
        val root = buildJsonObject {
            put("version", 1)
            put("peers", JsonArray(entries.values.map { e ->
                buildJsonObject {
                    put("id", e.peer.id)
                    put("name", e.peer.name)
                    put("platform", e.peer.platform)
                    put("pairedAt", e.peer.pairedAtMs)
                    put("lastSeen", e.peer.lastSeenMs)
                    put("app", e.peer.appVersion)
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
            MdLog.e(TAG, "cannot persist trust store", e)
        }
        _peers.value = entries.values.map { it.peer }
    }

    companion object {
        private const val TAG = "MD/Trust"
        private const val ANDROID_KEYSTORE = "AndroidKeyStore"
        private const val ALIAS = "mediaduet.pairing.wrap.v1"
    }
}
