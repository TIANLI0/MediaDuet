package app.mediaduet.update

import android.app.ActivityManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.os.Build
import android.provider.Settings
import androidx.core.net.toUri
import app.mediaduet.BuildConfig
import app.mediaduet.service.AppSettings
import app.mediaduet.service.Notifications
import app.mediaduet.util.MdLog
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * Software updates (Settings > About): asks GitHub for the latest release (once a day while
 * enabled, or on request), downloads `MediaDuet-<v>-android.apk`, checks it against the release's
 * SHA256SUMS.txt and that it is this app in a newer version, then installs it through
 * [PackageInstaller] (Android asks the user to confirm; "install unknown apps" must be allowed for
 * MediaDuet once). Only the version request and the download leave the phone.
 */
class Updater(private val context: Context, private val scope: CoroutineScope, private val settings: AppSettings) {

    sealed interface State {
        data object Idle : State
        data object Checking : State
        data class Current(val release: UpdateInfo.Release) : State
        data class Available(val release: UpdateInfo.Release) : State
        data class Downloading(val release: UpdateInfo.Release, val progress: Float) : State
        /** Downloaded and verified; Android must first allow MediaDuet to install apps. */
        data class NeedsPermission(val release: UpdateInfo.Release) : State
        data class Installing(val release: UpdateInfo.Release) : State
        data class Error(val code: String, val release: UpdateInfo.Release?) : State
    }

    private val _state = MutableStateFlow<State>(State.Idle)
    val state: StateFlow<State> = _state

    private val json = Json { ignoreUnknownKeys = true }
    private val dir = File(context.cacheDir, "updates")
    private var job: Job? = null
    private var verified: File? = null

    /** Automatic checks: shortly after start, then every 6 h whether a day has passed. */
    fun start() {
        // A new version is running now: the old download is no longer needed.
        dir.listFiles()?.forEach { it.delete() }
        scope.launch {
            delay(FIRST_CHECK_MS)
            while (true) {
                val due = System.currentTimeMillis() - settings.lastUpdateCheckMs >= INTERVAL_MS
                if (settings.autoUpdateCheck.value && due && _state.value.let { it is State.Idle || it is State.Current || it is State.Error }) check(manual = false)
                delay(RECHECK_MS)
            }
        }
    }

    /** [manual]: the user asked (failures are shown; an automatic check stays quiet). */
    fun checkNow() {
        scope.launch { check(manual = true) }
    }

    private suspend fun check(manual: Boolean) {
        val before = _state.value
        if (before is State.Checking || before is State.Downloading || before is State.Installing) return
        _state.value = State.Checking
        val body = withContext(Dispatchers.IO) { get(UpdateInfo.LATEST_RELEASE_URL, github = true) }
        val release = body?.let { runCatching { UpdateInfo.parseRelease(json.parseToJsonElement(it) as? JsonObject) }.getOrNull() }
        if (release == null) {
            _state.value = if (manual) State.Error(if (body == null) ERR_NETWORK else ERR_NO_RELEASE, null)
            else before.takeUnless { it is State.Checking } ?: State.Idle
            return
        }
        settings.lastUpdateCheckMs = System.currentTimeMillis()
        if (UpdateInfo.isNewer(release.version, BuildConfig.VERSION_NAME)) {
            MdLog.i(TAG, "MediaDuet ${release.version} is available (this is ${BuildConfig.VERSION_NAME})")
            _state.value = State.Available(release)
            if (!manual && settings.updateNotified != release.version) {
                settings.updateNotified = release.version
                Notifications.showUpdate(context, release.version)
            }
        } else {
            _state.value = State.Current(release)
        }
    }

    /** Downloads (when needed), verifies and installs the available version. */
    fun install() {
        val release = when (val s = _state.value) {
            is State.Available -> s.release
            is State.NeedsPermission -> s.release
            is State.Error -> s.release
            else -> null
        } ?: return
        if (!UpdateInfo.isNewer(release.version, BuildConfig.VERSION_NAME)) return
        if (job?.isActive == true) return
        job = scope.launch {
            val file = verified?.takeIf { it.isFile && it.name == UpdateInfo.androidAsset(release.version) }
                ?: download(release) ?: return@launch
            verified = file
            if (!context.packageManager.canRequestPackageInstalls()) {
                _state.value = State.NeedsPermission(release)
                return@launch
            }
            commit(release, file)
        }
    }

    /** Opens Android's "install unknown apps" setting for MediaDuet. */
    fun openInstallPermission(): Intent =
        Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, "package:${context.packageName}".toUri()).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)

    private suspend fun download(release: UpdateInfo.Release): File? {
        val name = UpdateInfo.androidAsset(release.version)
        val asset = release.find(name)
        val sums = release.find(UpdateInfo.CHECKSUMS_ASSET)
        if (asset == null || sums == null) {
            _state.value = State.Error(ERR_NO_ASSET, release)
            return null
        }
        _state.value = State.Downloading(release, 0f)
        val result = withContext(Dispatchers.IO) {
            try {
                val want = get(sums.url)?.let { UpdateInfo.checksumFor(it, name) } ?: return@withContext ERR_NO_CHECKSUM to null
                dir.mkdirs()
                val file = File(dir, name)
                val digest = MessageDigest.getInstance("SHA-256")
                val c = open(asset.url)
                try {
                    if (c.responseCode !in 200..299) return@withContext ERR_NETWORK to null
                    val total = c.contentLengthLong.takeIf { it > 0 } ?: asset.size
                    var done = 0L
                    var shown = 0f
                    c.inputStream.use { input ->
                        file.outputStream().use { out ->
                            val buf = ByteArray(64 * 1024)
                            while (true) {
                                val n = input.read(buf)
                                if (n < 0) break
                                out.write(buf, 0, n)
                                digest.update(buf, 0, n)
                                done += n
                                val p = if (total > 0) (done.toFloat() / total).coerceIn(0f, 1f) else 0f
                                if (p - shown >= 0.02f) {
                                    shown = p
                                    _state.value = State.Downloading(release, p)
                                }
                            }
                        }
                    }
                } finally {
                    c.disconnect()
                }
                val got = digest.digest().joinToString("") { "%02x".format(it) }
                if (got != want) {
                    MdLog.w(TAG, "checksum mismatch for $name")
                    file.delete()
                    return@withContext ERR_CHECKSUM to null
                }
                // It must be this app, newer than the installed one.
                val info = context.packageManager.getPackageArchiveInfo(file.path, 0)
                if (info == null || info.packageName != context.packageName || info.longVersionCode <= BuildConfig.VERSION_CODE) {
                    file.delete()
                    return@withContext ERR_NOT_NEWER to null
                }
                null to file
            } catch (e: IOException) {
                MdLog.i(TAG, "update download failed: ${e.javaClass.simpleName}")
                ERR_NETWORK to null
            }
        }
        result.first?.let { _state.value = State.Error(it, release) }
        return result.second
    }

    private suspend fun commit(release: UpdateInfo.Release, file: File) {
        _state.value = State.Installing(release)
        val ok = withContext(Dispatchers.IO) {
            try {
                val installer = context.packageManager.packageInstaller
                val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
                    setAppPackageName(context.packageName)
                    if (Build.VERSION.SDK_INT >= 34) setPackageSource(PackageInstaller.PACKAGE_SOURCE_DOWNLOADED_FILE)
                }
                val id = installer.createSession(params)
                installer.openSession(id).use { session ->
                    session.openWrite("base.apk", 0, file.length()).use { out ->
                        file.inputStream().use { it.copyTo(out) }
                        session.fsync(out)
                    }
                    val status = PendingIntent.getBroadcast(
                        context, id,
                        Intent(context, UpdateInstallReceiver::class.java).setPackage(context.packageName),
                        PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE,
                    )
                    session.commit(status.intentSender)
                }
                true
            } catch (e: Exception) {
                MdLog.w(TAG, "could not start the installation", e)
                false
            }
        }
        if (!ok) _state.value = State.Error(ERR_INSTALL, release)
    }

    /** From [UpdateInstallReceiver]: the system needs the user's confirmation. */
    internal fun onConfirm(confirm: Intent) {
        confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        val release = (_state.value as? State.Installing)?.release
        if (inForeground()) {
            runCatching { context.startActivity(confirm) }.onSuccess { return }
        }
        // In the background Android does not let us open the installer: ask with a notification.
        Notifications.showUpdateConfirm(context, confirm, release?.version ?: "")
    }

    /** From [UpdateInstallReceiver]: the installation failed or was cancelled. */
    internal fun onFailed(status: Int, message: String?) {
        MdLog.w(TAG, "update not installed: status $status ${message.orEmpty()}")
        val release = (_state.value as? State.Installing)?.release
        _state.value = when {
            // Play Protect or the OEM's security check refused the APK (the user saw why).
            message?.contains("VERIFICATION", ignoreCase = true) == true -> State.Error(ERR_BLOCKED, release)
            status == PackageInstaller.STATUS_FAILURE_ABORTED -> release?.let { State.Available(it) } ?: State.Idle
            status == PackageInstaller.STATUS_FAILURE_CONFLICT || status == PackageInstaller.STATUS_FAILURE_INCOMPATIBLE -> State.Error(ERR_SIGNATURE, release)
            status == PackageInstaller.STATUS_FAILURE_STORAGE -> State.Error(ERR_STORAGE, release)
            else -> State.Error(ERR_INSTALL, release)
        }
    }

    private fun inForeground(): Boolean {
        val info = ActivityManager.RunningAppProcessInfo()
        ActivityManager.getMyMemoryState(info)
        return info.importance <= ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND
    }

    private fun open(url: String): HttpURLConnection = (URL(url).openConnection() as HttpURLConnection).apply {
        connectTimeout = 15_000
        readTimeout = 30_000
        instanceFollowRedirects = true
        useCaches = false
        setRequestProperty("User-Agent", "MediaDuet/${BuildConfig.VERSION_NAME} (update check)")
    }

    /** GET as text; null on any failure. */
    private fun get(url: String, github: Boolean = false): String? = try {
        val c = open(url)
        if (github) c.setRequestProperty("Accept", "application/vnd.github+json")
        try {
            if (c.responseCode in 200..299) c.inputStream.use { String(it.readBytes(), Charsets.UTF_8) } else {
                MdLog.i(TAG, "GET $url -> HTTP ${c.responseCode}")
                null
            }
        } finally {
            c.disconnect()
        }
    } catch (e: IOException) {
        MdLog.i(TAG, "GET $url failed: ${e.javaClass.simpleName}")
        null
    }

    companion object {
        private const val TAG = "MD/Update"
        private const val FIRST_CHECK_MS = 30_000L
        private const val RECHECK_MS = 6 * 3600_000L
        private const val INTERVAL_MS = 24 * 3600_000L

        const val ERR_NETWORK = "network"
        const val ERR_NO_RELEASE = "no_release"
        const val ERR_NO_ASSET = "no_asset"
        const val ERR_NO_CHECKSUM = "no_checksum"
        const val ERR_CHECKSUM = "checksum"
        const val ERR_NOT_NEWER = "not_newer"
        const val ERR_INSTALL = "install"
        const val ERR_SIGNATURE = "signature"
        const val ERR_STORAGE = "storage"
        const val ERR_BLOCKED = "blocked"
    }
}
