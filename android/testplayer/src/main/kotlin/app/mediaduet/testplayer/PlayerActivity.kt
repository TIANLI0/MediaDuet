package app.mediaduet.testplayer

import android.app.Activity
import android.os.Bundle
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView

/** TEST FIXTURE ONLY: buttons that drive [PlayerService]. */
class PlayerActivity : Activity() {
    private lateinit var status: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(48, 160, 48, 48)
        }
        root.addView(TextView(this).apply {
            text = "Test fixture for MediaDuet — plays a generated sine tone through a real MediaSession."
            textSize = 16f
        })
        status = TextView(this).apply {
            textSize = 18f
            setPadding(0, 32, 0, 32)
            text = "idle"
        }
        root.addView(status)
        for (action in listOf("play", "pause", "previous", "next", "stop")) {
            root.addView(Button(this).apply {
                text = action
                setOnClickListener { PlayerService.send(this@PlayerActivity, action) }
            })
        }
        setContentView(root)
        if (intent?.getBooleanExtra("autoplay", false) == true) PlayerService.send(this, "play")
    }

    override fun onResume() {
        super.onResume()
        onStateChanged = { s -> runOnUiThread { status.text = s } }
    }

    override fun onPause() {
        onStateChanged = null
        super.onPause()
    }

    companion object {
        @Volatile var onStateChanged: ((String) -> Unit)? = null
    }
}
