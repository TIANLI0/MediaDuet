package app.mediaduet.ui

import android.content.Context
import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.runtime.mutableStateOf
import app.mediaduet.service.AppLanguage
import app.mediaduet.service.Engine
import app.mediaduet.service.LinkService
import app.mediaduet.ui.theme.MediaDuetTheme

class MainActivity : ComponentActivity() {
    private val requestedTab = mutableStateOf<String?>(null)

    override fun attachBaseContext(newBase: Context) {
        super.attachBaseContext(AppLanguage.wrap(newBase))
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        val engine = Engine.get(this)
        engine.start()
        requestedTab.value = intent?.getStringExtra(EXTRA_TAB)
        // Back at the root leaves the app without finishing it (state survives). Android 12+
        // already does this for the root launcher activity (with the predictive animation).
        if (android.os.Build.VERSION.SDK_INT < 31) {
            onBackPressedDispatcher.addCallback(this, object : androidx.activity.OnBackPressedCallback(true) {
                override fun handleOnBackPressed() {
                    moveTaskToBack(true)
                }
            })
        }
        setContent {
            MediaDuetTheme {
                MediaDuetRoot(engine, requestedTab)
            }
        }
    }

    override fun onStart() {
        super.onStart()
        // (Re)start the service every time we become visible: a start from the foreground gives the
        // running service while-in-use capability (Android 17 background volume / audio rules),
        // which background starts (boot, companion presence, restart after a crash) do not get.
        LinkService.start(this, "activity")
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        intent.getStringExtra(EXTRA_TAB)?.let { requestedTab.value = it }
    }

    companion object {
        const val EXTRA_TAB = "tab"
    }
}
