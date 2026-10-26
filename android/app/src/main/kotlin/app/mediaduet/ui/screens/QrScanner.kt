package app.mediaduet.ui.screens

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.provider.Settings
import android.util.Size
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.Camera
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.lifecycle.awaitInstance
import androidx.camera.view.PreviewView
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.snap
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.CompositingStrategy
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.mediaduet.R
import app.mediaduet.link.QrPairing
import app.mediaduet.service.WorkMode
import app.mediaduet.ui.SettingsRoute
import app.mediaduet.ui.UiContext
import app.mediaduet.ui.components.AppIcons
import app.mediaduet.ui.components.GlassButton
import app.mediaduet.ui.components.GlassCard
import app.mediaduet.ui.components.GlassIconButton
import app.mediaduet.ui.theme.LocalGlass
import com.google.zxing.BarcodeFormat
import com.google.zxing.BinaryBitmap
import com.google.zxing.DecodeHintType
import com.google.zxing.LuminanceSource
import com.google.zxing.MultiFormatReader
import com.google.zxing.PlanarYUVLuminanceSource
import com.google.zxing.ReaderException
import com.google.zxing.common.HybridBinarizer
import com.kyant.backdrop.backdrops.layerBackdrop
import com.kyant.backdrop.backdrops.rememberLayerBackdrop
import kotlinx.coroutines.delay
import java.util.concurrent.Executors

/** How long the phone waits for the PC before it explains how to connect by address. */
private const val SLOW_PAIRING_MS = 30_000L

/**
 * QR pairing (§3.5) as a full-screen overlay: the camera reads the code MediaDuet shows on the
 * PC, which arms pairing mode with the PC's one-time secret; the PC then connects and pairs
 * without a code to compare. Shows the progress until the pairing succeeds or fails.
 */
@Composable
fun QrScannerOverlay(ui: UiContext, open: Boolean, reduced: Boolean, onClose: () -> Unit) {
    BackHandler(enabled = open, onBack = onClose)
    AnimatedVisibility(open, enter = fadeIn(if (reduced) snap() else tween(220)), exit = fadeOut(if (reduced) snap() else tween(220))) {
        var scanned by remember { mutableStateOf<QrPairing.Code?>(null) }
        val code = scanned
        if (code == null) {
            ScanStep(ui, onClose) { c ->
                // Scanning a PC's code means linking with it: leave "this phone only".
                if (ui.engine.settings.mode.value != WorkMode.LINKED) ui.engine.settings.setMode(WorkMode.LINKED)
                ui.engine.pairing.armQr(c.peerId, c.peerName, c.token)
                scanned = c
            }
        } else {
            ProgressStep(ui, code, onScanAgain = {
                ui.engine.pairing.clearOutcome()
                scanned = null
            }, onClose = onClose)
        }
    }
}

// ---- step 1: camera ------------------------------------------------------------------------

@Composable
private fun ScanStep(ui: UiContext, onClose: () -> Unit, onCode: (QrPairing.Code) -> Unit) {
    val context = LocalContext.current
    val haptics = LocalHapticFeedback.current
    var granted by remember { mutableStateOf(hasCamera(context)) }
    var denials by remember { mutableIntStateOf(0) }
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { ok ->
        granted = ok
        if (!ok) denials++
    }
    LaunchedEffect(Unit) { if (!granted) launcher.launch(Manifest.permission.CAMERA) }
    LifecycleResumeEffect(Unit) {
        // Back from the system settings: the permission may have been turned on there.
        granted = hasCamera(context)
        onPauseOrDispose { }
    }
    var cameraFailed by remember { mutableStateOf(false) }

    if (!granted || cameraFailed) {
        val activity = context.findActivity()
        // After a denial without a rationale the system no longer asks: only the settings help.
        val blocked = denials > 0 && activity != null &&
            !ActivityCompat.shouldShowRequestPermissionRationale(activity, Manifest.permission.CAMERA)
        MessageScreen(ui, onClose) {
            Text(stringResource(R.string.qr_scan_title), style = MaterialTheme.typography.titleLarge)
            Spacer(Modifier.height(8.dp))
            Text(
                stringResource(
                    when {
                        cameraFailed -> R.string.qr_camera_unavailable
                        blocked -> R.string.qr_camera_denied
                        else -> R.string.qr_camera_needed
                    },
                ),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (!cameraFailed) {
                Spacer(Modifier.height(20.dp))
                GlassButton(
                    {
                        if (blocked) context.startActivity(
                            Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", context.packageName, null)),
                        ) else launcher.launch(Manifest.permission.CAMERA)
                    },
                    ui.overlayBackdrop, Modifier.fillMaxWidth(), prominent = true, height = 48.dp,
                ) { Text(stringResource(if (blocked) R.string.open_settings else R.string.qr_camera_allow)) }
            }
        }
        return
    }

    val glass = LocalGlass.current
    val camLayer = rememberLayerBackdrop()
    var camera by remember { mutableStateOf<Camera?>(null) }
    var torch by remember { mutableStateOf(false) }
    var invalidAt by remember { mutableLongStateOf(0L) }
    val currentOnCode by rememberUpdatedState(onCode)
    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black)
            .clickable(remember { MutableInteractionSource() }, indication = null) { },
    ) {
        CameraPreview(
            Modifier.fillMaxSize().layerBackdrop(camLayer),
            onCamera = { camera = it },
            onError = { cameraFailed = true },
        ) { text ->
            val c = QrPairing.parse(text)
            if (c != null) {
                haptics.performHapticFeedback(HapticFeedbackType.Confirm)
                currentOnCode(c)
            } else if (System.currentTimeMillis() - invalidAt > 3_000) {
                invalidAt = System.currentTimeMillis()
            }
        }
        Viewfinder()
        Row(
            Modifier.fillMaxWidth().statusBarsPadding().padding(16.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            GlassIconButton(AppIcons.Close, stringResource(R.string.qr_close), onClose, camLayer, size = 48.dp, iconSize = 22.dp)
            val cam = camera
            if (cam != null && cam.cameraInfo.hasFlashUnit()) {
                GlassIconButton(
                    AppIcons.Flashlight,
                    stringResource(R.string.qr_torch),
                    {
                        torch = !torch
                        cam.cameraControl.enableTorch(torch)
                    },
                    camLayer,
                    size = 48.dp,
                    iconSize = 22.dp,
                    active = torch,
                )
            }
        }
        var showInvalid by remember { mutableStateOf(false) }
        LaunchedEffect(invalidAt) {
            if (invalidAt == 0L) return@LaunchedEffect
            showInvalid = true
            delay(2_500)
            showInvalid = false
        }
        Box(Modifier.fillMaxSize().navigationBarsPadding().padding(20.dp), contentAlignment = Alignment.BottomCenter) {
            GlassCard(camLayer, Modifier.widthIn(max = 480.dp).fillMaxWidth(), strong = true, contentPadding = PaddingValues(20.dp)) {
                Text(stringResource(R.string.qr_scan_title), style = MaterialTheme.typography.titleMedium)
                Spacer(Modifier.height(4.dp))
                Text(
                    stringResource(if (showInvalid) R.string.qr_not_a_code else R.string.qr_scan_hint),
                    style = MaterialTheme.typography.bodyMedium,
                    color = if (showInvalid) glass.warn else MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

private fun hasCamera(context: Context) =
    ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

/** A flat scrim with a clear rounded window where the code goes (no gradients). */
@Composable
private fun Viewfinder() {
    Canvas(Modifier.fillMaxSize().graphicsLayer { compositingStrategy = CompositingStrategy.Offscreen }) {
        drawRect(Color.Black.copy(alpha = 0.42f))
        val side = minOf(size.width, size.height) * 0.66f
        val topLeft = Offset((size.width - side) / 2f, (size.height - side) / 2f - size.height * 0.05f)
        val corner = CornerRadius(28.dp.toPx())
        val box = androidx.compose.ui.geometry.Size(side, side)
        drawRoundRect(Color.Transparent, topLeft, box, corner, blendMode = BlendMode.Clear)
        drawRoundRect(Color.White.copy(alpha = 0.9f), topLeft, box, corner, style = Stroke(2.5.dp.toPx()))
    }
}

/** Back camera preview + QR analysis (ZXing on a worker thread); [onText] runs on the main thread. */
@Composable
private fun CameraPreview(modifier: Modifier, onCamera: (Camera) -> Unit, onError: () -> Unit, onText: (String) -> Unit) {
    val context = LocalContext.current
    val owner = LocalLifecycleOwner.current
    val previewView = remember {
        PreviewView(context).apply {
            // A TextureView: the glass controls can refract the preview.
            implementationMode = PreviewView.ImplementationMode.COMPATIBLE
            scaleType = PreviewView.ScaleType.FILL_CENTER
        }
    }
    val worker = remember { Executors.newSingleThreadExecutor() }
    val currentOnText by rememberUpdatedState(onText)
    var provider by remember { mutableStateOf<ProcessCameraProvider?>(null) }
    LaunchedEffect(owner) {
        try {
            val p = ProcessCameraProvider.awaitInstance(context)
            provider = p
            val preview = Preview.Builder().build().also { it.setSurfaceProvider(previewView.surfaceProvider) }
            val analysis = ImageAnalysis.Builder()
                .setResolutionSelector(
                    ResolutionSelector.Builder()
                        .setResolutionStrategy(ResolutionStrategy(Size(1280, 720), ResolutionStrategy.FALLBACK_RULE_CLOSEST_HIGHER_THEN_LOWER))
                        .build(),
                )
                .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                .build()
            val main = ContextCompat.getMainExecutor(context)
            analysis.setAnalyzer(worker, QrAnalyzer { text -> main.execute { currentOnText(text) } })
            p.unbindAll()
            onCamera(p.bindToLifecycle(owner, CameraSelector.DEFAULT_BACK_CAMERA, preview, analysis))
        } catch (e: kotlinx.coroutines.CancellationException) {
            throw e
        } catch (e: Exception) {
            app.mediaduet.util.MdLog.w("MD/QrScanner", "camera unavailable", e)
            onError()
        }
    }
    DisposableEffect(Unit) {
        onDispose {
            provider?.unbindAll()
            worker.shutdown()
        }
    }
    AndroidView({ previewView }, modifier)
}

/** Reads QR codes from the luminance plane; inverted codes (light on dark) too. */
private class QrAnalyzer(private val onText: (String) -> Unit) : ImageAnalysis.Analyzer {
    private val reader = MultiFormatReader().apply {
        setHints(mapOf(DecodeHintType.POSSIBLE_FORMATS to listOf(BarcodeFormat.QR_CODE), DecodeHintType.TRY_HARDER to true))
    }
    private var luma = ByteArray(0)
    private var last: String? = null

    override fun analyze(image: ImageProxy) {
        image.use {
            val plane = it.planes[0]
            val stride = plane.rowStride
            val size = stride * it.height
            if (luma.size != size) luma = ByteArray(size)
            val buf = plane.buffer.duplicate()
            buf.rewind()
            buf.get(luma, 0, minOf(buf.remaining(), size))
            val source = PlanarYUVLuminanceSource(luma, stride, it.height, 0, 0, it.width, it.height, false)
            val text = decode(source) ?: decode(source.invert()) ?: return
            if (text != last) last = text else return // each code once, until another one is seen
            onText(text)
        }
    }

    private fun decode(source: LuminanceSource): String? = try {
        reader.decodeWithState(BinaryBitmap(HybridBinarizer(source))).text
    } catch (e: ReaderException) {
        null
    } finally {
        reader.reset()
    }
}

// ---- step 2: the PC pairs --------------------------------------------------------------------

@Composable
private fun ProgressStep(ui: UiContext, code: QrPairing.Code, onScanAgain: () -> Unit, onClose: () -> Unit) {
    val engine = ui.engine
    val glass = LocalGlass.current
    val state by engine.pairing.state.collectAsStateWithLifecycle()
    val transports by engine.transports.state.collectAsStateWithLifecycle()
    val name = state.qr?.peerName?.ifBlank { null } ?: code.peerName.ifBlank { stringResource(R.string.qr_unnamed_pc) }
    val outcome = state.lastOutcome
    var slow by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        delay(SLOW_PAIRING_MS)
        slow = true
    }
    val success = outcome?.success == true
    LaunchedEffect(success) {
        if (success) {
            delay(1_800)
            onClose()
        }
    }

    MessageScreen(ui, onClose, showClose = !success) {
        when {
            success -> {
                Icon(AppIcons.CheckCircle, null, Modifier.size(40.dp), tint = glass.good)
                Spacer(Modifier.height(12.dp))
                Text(stringResource(R.string.pairing_success, outcome!!.peerName), style = MaterialTheme.typography.titleLarge)
                Spacer(Modifier.height(20.dp))
                GlassButton(onClose, ui.overlayBackdrop, Modifier.fillMaxWidth(), prominent = true, height = 48.dp) {
                    Text(stringResource(R.string.qr_done))
                }
            }
            outcome != null || state.qr == null -> {
                // A failed attempt, or the code expired with the pairing window.
                Icon(AppIcons.Warning, null, Modifier.size(36.dp), tint = glass.warn)
                Spacer(Modifier.height(12.dp))
                Text(
                    if (outcome != null) stringResource(R.string.pairing_failed, name, pairingReasonLabel(outcome.reason))
                    else stringResource(R.string.qr_expired),
                    style = MaterialTheme.typography.bodyLarge,
                )
                Spacer(Modifier.height(20.dp))
                GlassButton(onScanAgain, ui.overlayBackdrop, Modifier.fillMaxWidth(), prominent = true, height = 48.dp) {
                    Text(stringResource(R.string.qr_scan_again))
                }
            }
            else -> {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    CircularProgressIndicator(Modifier.size(22.dp), color = glass.accent, strokeWidth = 2.5.dp)
                    Spacer(Modifier.width(14.dp))
                    Text(
                        stringResource(if (state.qr?.connecting == true) R.string.qr_connecting else R.string.qr_waiting, name),
                        style = MaterialTheme.typography.titleMedium,
                    )
                }
                if (!ui.permissions.localNetwork && ui.permissions.localNetworkRequired) {
                    Spacer(Modifier.height(16.dp))
                    Text(stringResource(R.string.qr_local_network_needed), style = MaterialTheme.typography.bodyMedium, color = glass.warn)
                    Spacer(Modifier.height(12.dp))
                    GlassButton({
                        onClose()
                        ui.openSettings(SettingsRoute.PERMISSIONS)
                    }, ui.overlayBackdrop, height = 40.dp) { Text(stringResource(R.string.grant)) }
                } else if (slow && state.qr?.connecting != true) {
                    Spacer(Modifier.height(16.dp))
                    val port = transports.lanPort
                    if (transports.lanAddresses.isEmpty() || port == null) {
                        Text(stringResource(R.string.qr_no_address), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    } else {
                        Text(stringResource(R.string.qr_not_found_hint, name), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Spacer(Modifier.height(8.dp))
                        for (a in transports.lanAddresses) {
                            Text(stringResource(R.string.lan_endpoint, a, port), style = MaterialTheme.typography.titleMedium)
                        }
                    }
                }
            }
        }
    }
}

/** A calm, nearly opaque page with one glass card (permission request, progress, outcome). */
@Composable
private fun MessageScreen(ui: UiContext, onClose: () -> Unit, showClose: Boolean = true, content: @Composable () -> Unit) {
    val glass = LocalGlass.current
    Box(
        Modifier
            .fillMaxSize()
            .background(glass.background.copy(alpha = 0.97f))
            .clickable(remember { MutableInteractionSource() }, indication = null) { },
    ) {
        if (showClose) {
            Box(Modifier.statusBarsPadding().padding(16.dp)) {
                GlassIconButton(AppIcons.Close, stringResource(R.string.qr_close), onClose, ui.overlayBackdrop, size = 48.dp, iconSize = 22.dp)
            }
        }
        Column(
            Modifier.fillMaxSize().navigationBarsPadding().padding(24.dp),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            GlassCard(ui.overlayBackdrop, Modifier.widthIn(max = 440.dp).fillMaxWidth(), strong = true, contentPadding = PaddingValues(24.dp)) {
                content()
            }
        }
    }
}
