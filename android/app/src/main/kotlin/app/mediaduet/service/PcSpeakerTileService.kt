package app.mediaduet.service

import android.service.quicksettings.Tile
import android.service.quicksettings.TileService
import app.mediaduet.R
import app.mediaduet.protocol.SinkState
import app.mediaduet.ui.screens.PC_SPEAKER_ERRORS
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch

/**
 * Quick Settings tile "PC speaker": one tap from anywhere asks the connected PC to play this
 * phone's Bluetooth audio (Windows A2DP sink, `audio.sink.set`) or to stop. The tile follows the
 * PC's `audio.sink.state` while the shade is open; it is unavailable while no PC is connected or
 * the phone is not a paired Bluetooth audio source of the PC.
 */
class PcSpeakerTileService : TileService() {
    private var job: Job? = null

    override fun onStartListening() {
        val engine = Engine.get(applicationContext)
        job?.cancel()
        job = engine.scope.launch(Dispatchers.Main) {
            combine(engine.remote.sink, engine.remote.state, engine.link.status, engine.settings.mode) { sink, remote, link, mode ->
                val pc = link.current?.peer?.name
                if (mode.linked && pc != null && remote?.connected == true) sink?.let { it to pc } else null
            }.collect { render(it?.first, it?.second) }
        }
    }

    override fun onStopListening() {
        job?.cancel()
        job = null
    }

    override fun onClick() {
        val engine = Engine.get(applicationContext)
        val sink = engine.remote.sink.value ?: return
        if (!sink.available && !sink.active) return
        engine.scope.launch { engine.setPcSpeaker(!sink.active) }
    }

    private fun render(sink: SinkState?, pcName: String?) {
        val tile = qsTile ?: return
        tile.label = getString(R.string.tile_pc_speaker)
        when {
            sink == null || pcName == null -> {
                tile.state = Tile.STATE_UNAVAILABLE
                tile.subtitle = getString(R.string.tile_pc_unavailable)
            }
            !sink.available && !sink.active -> {
                tile.state = Tile.STATE_UNAVAILABLE
                tile.subtitle = getString(R.string.tile_pc_unsupported)
            }
            else -> {
                tile.state = if (sink.active) Tile.STATE_ACTIVE else Tile.STATE_INACTIVE
                tile.subtitle = when (sink.state) {
                    SinkState.CONNECTING -> getString(R.string.pc_speaker_connecting)
                    SinkState.ERROR -> getString(PC_SPEAKER_ERRORS[sink.error] ?: R.string.pc_speaker_error_failed)
                    else -> pcName
                }
            }
        }
        tile.updateTile()
    }
}
