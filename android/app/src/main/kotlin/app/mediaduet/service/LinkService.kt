package app.mediaduet.service

import android.app.ForegroundServiceStartNotAllowedException
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.ServiceCompat
import app.mediaduet.R
import app.mediaduet.link.TransportKind
import app.mediaduet.util.MdLog
import kotlinx.coroutines.CoroutineExceptionHandler
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.launch

/**
 * `connectedDevice` foreground service: keeps the process at foreground priority while the user
 * wants MediaDuet to stay reachable. Shows an ongoing, minimum-importance status notification.
 */
class LinkService : Service() {
    override fun attachBaseContext(newBase: Context) {
        super.attachBaseContext(AppLanguage.wrap(newBase))
    }

    /** Last status line shown in the ongoing notification. */
    private var statusText: String? = null

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main + CoroutineExceptionHandler { _, e -> MdLog.e(TAG, "service coroutine failed", e) })

    override fun onCreate() {
        super.onCreate()
        val engine = Engine.get(this)
        engine.start()
        Notifications.createChannels(this)
        try {
            ServiceCompat.startForeground(
                this,
                Notifications.ID_LINK,
                Notifications.linkNotification(this, getString(R.string.status_waiting)),
                if (Build.VERSION.SDK_INT >= 29) ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE else 0,
            )
            _running.value = true
        } catch (e: Exception) {
            // Missing prerequisites or a background-start restriction: keep running unprivileged.
            MdLog.w(TAG, "startForeground refused: ${e.javaClass.simpleName} ${e.message}")
            stopSelf()
            return
        }
        scope.launch {
            combine(engine.link.status, engine.transports.state) { link, transports ->
                val current = link.current
                if (current != null) {
                    val via = current.transports.joinToString(getString(R.string.list_separator)) {
                        getString(if (it.kind == TransportKind.BT) R.string.transport_bt else R.string.transport_lan)
                    }
                    getString(R.string.status_connected_to, current.peer.name, via)
                } else {
                    val parts = buildList {
                        if (transports.bluetooth == app.mediaduet.transport.BluetoothListenState.LISTENING) add(getString(R.string.transport_bt))
                        transports.lanPort?.let { if (transports.localNetworkAllowed) add(getString(R.string.transport_with_port, getString(R.string.transport_lan), it)) }
                    }
                    if (parts.isEmpty()) getString(R.string.status_waiting) else getString(R.string.status_listening, parts.joinToString(getString(R.string.list_separator)))
                }
            }.distinctUntilChanged().collect { text ->
                statusText = text
                if (Notifications.canPost(this@LinkService)) {
                    runCatching {
                        getSystemService(NotificationManager::class.java)
                            .notify(Notifications.ID_LINK, Notifications.linkNotification(this@LinkService, text))
                    }
                }
            }
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (!Engine.get(this).settings.foregroundService.value) {
            stopSelf()
            return START_NOT_STICKY
        }
        // Re-assert foreground on every start: a start from a visible activity / user action
        // refreshes the service's while-in-use capability (no stop/restart, which would lose it).
        if (_running.value) {
            try {
                ServiceCompat.startForeground(
                    this,
                    Notifications.ID_LINK,
                    Notifications.linkNotification(this, statusText ?: getString(R.string.status_waiting)),
                    if (Build.VERSION.SDK_INT >= 29) ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE else 0,
                )
            } catch (e: Exception) {
                MdLog.w(TAG, "startForeground (refresh) refused: ${e.javaClass.simpleName}")
            }
        }
        return START_STICKY
    }

    override fun onTimeout(startId: Int, fgsType: Int) {
        // connectedDevice has no time limit today; honour the contract should the platform add one.
        MdLog.w(TAG, "foreground service timeout for type $fgsType")
        stopSelf()
    }

    override fun onDestroy() {
        _running.value = false
        scope.cancel()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        private const val TAG = "MD/LinkService"
        private val _running = MutableStateFlow(false)
        val running: StateFlow<Boolean> = _running

        /** Starts the foreground service if enabled; failures are logged, never fatal. */
        fun start(context: Context, reason: String) {
            val engine = Engine.get(context)
            engine.start()
            if (!engine.settings.foregroundService.value) return
            try {
                context.startForegroundService(Intent(context, LinkService::class.java))
                MdLog.i(TAG, "foreground service requested ($reason)")
            } catch (e: Exception) {
                val notAllowed = Build.VERSION.SDK_INT >= 31 && e is ForegroundServiceStartNotAllowedException
                MdLog.w(TAG, "cannot start foreground service ($reason): ${if (notAllowed) "background start not allowed" else e.toString()}")
            }
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, LinkService::class.java))
        }
    }
}
