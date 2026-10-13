package app.mediaduet.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp
import com.kyant.backdrop.isRenderEffectSupported

/**
 * Flat, neutral palette (no gradients): solid backgrounds or blurred artwork, neutral glass
 * tints, one restrained system accent. Below API 31 (no RenderEffect) glass surfaces become
 * more opaque so text stays readable without blur.
 */
@Immutable
data class GlassColors(
    val background: Color,
    val surface: Color,
    val surfaceStrong: Color,
    val bar: Color,
    val fill: Color,
    val track: Color,
    val sliderFill: Color,
    val toggleOn: Color,
    val accent: Color,
    val good: Color,
    val warn: Color,
    val bad: Color,
    val scrim: Color,
    val isDark: Boolean,
)

private val lightGlass = run {
    val fx = isRenderEffectSupported()
    GlassColors(
        background = Color(0xFFF5F5F7),
        surface = Color.White.copy(alpha = if (fx) 0.55f else 0.88f),
        surfaceStrong = Color.White.copy(alpha = if (fx) 0.78f else 0.96f),
        bar = Color(0xFFFAFAFA).copy(alpha = if (fx) 0.45f else 0.92f),
        fill = Color(0xFF1D1D1F).copy(alpha = 0.06f),
        track = Color(0xFF787880).copy(alpha = 0.2f),
        sliderFill = Color(0xFF1D1D1F),
        toggleOn = Color(0xFF007AFF),
        accent = Color(0xFF007AFF),
        good = Color(0xFF248A3D),
        warn = Color(0xFFB25000),
        bad = Color(0xFFD70015),
        scrim = Color(0xFFF5F5F7).copy(alpha = 0.55f),
        isDark = false,
    )
}

private val darkGlass = run {
    val fx = isRenderEffectSupported()
    GlassColors(
        background = Color(0xFF0B0B0C),
        surface = Color(0xFF1C1C1E).copy(alpha = if (fx) 0.5f else 0.9f),
        surfaceStrong = Color(0xFF2C2C2E).copy(alpha = if (fx) 0.78f else 0.96f),
        bar = Color(0xFF121212).copy(alpha = if (fx) 0.45f else 0.92f),
        fill = Color.White.copy(alpha = 0.08f),
        track = Color(0xFF787880).copy(alpha = 0.36f),
        sliderFill = Color(0xFFF5F5F7),
        toggleOn = Color(0xFF0A84FF),
        accent = Color(0xFF0A84FF),
        good = Color(0xFF30D158),
        warn = Color(0xFFFF9F0A),
        bad = Color(0xFFFF453A),
        scrim = Color.Black.copy(alpha = 0.5f),
        isDark = true,
    )
}

val LocalGlass = staticCompositionLocalOf { lightGlass }

private val LightScheme = lightColorScheme(
    primary = Color(0xFF007AFF),
    onPrimary = Color.White,
    background = Color(0xFFF5F5F7),
    surface = Color(0xFFF5F5F7),
    onSurface = Color(0xFF1D1D1F),
    onSurfaceVariant = Color(0xFF1D1D1F).copy(alpha = 0.62f),
    onBackground = Color(0xFF1D1D1F),
)

private val DarkScheme = darkColorScheme(
    primary = Color(0xFF0A84FF),
    onPrimary = Color.White,
    background = Color(0xFF0B0B0C),
    surface = Color(0xFF0B0B0C),
    onSurface = Color(0xFFF5F5F7),
    onSurfaceVariant = Color(0xFFF5F5F7).copy(alpha = 0.62f),
    onBackground = Color(0xFFF5F5F7),
)

private val AppTypography = Typography(
    headlineLarge = TextStyle(fontSize = 34.sp, lineHeight = 41.sp, fontWeight = FontWeight.Bold, letterSpacing = 0.2.sp),
    headlineSmall = TextStyle(fontSize = 22.sp, lineHeight = 28.sp, fontWeight = FontWeight.SemiBold),
    titleLarge = TextStyle(fontSize = 20.sp, lineHeight = 25.sp, fontWeight = FontWeight.SemiBold),
    titleMedium = TextStyle(fontSize = 17.sp, lineHeight = 22.sp, fontWeight = FontWeight.SemiBold),
    bodyLarge = TextStyle(fontSize = 17.sp, lineHeight = 22.sp),
    bodyMedium = TextStyle(fontSize = 15.sp, lineHeight = 20.sp),
    bodySmall = TextStyle(fontSize = 13.sp, lineHeight = 18.sp),
    labelLarge = TextStyle(fontSize = 15.sp, lineHeight = 20.sp, fontWeight = FontWeight.Medium),
    labelMedium = TextStyle(fontSize = 12.sp, lineHeight = 16.sp, fontWeight = FontWeight.Medium),
    labelSmall = TextStyle(fontSize = 11.sp, lineHeight = 13.sp, fontWeight = FontWeight.Medium),
)

@Composable
fun MediaDuetTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    CompositionLocalProvider(LocalGlass provides if (dark) darkGlass else lightGlass) {
        val scheme = if (dark) DarkScheme else LightScheme
        MaterialTheme(colorScheme = scheme, typography = AppTypography) {
            // No Material Surface is used, so provide the content colour explicitly (dark mode).
            CompositionLocalProvider(LocalContentColor provides scheme.onSurface, content = content)
        }
    }
}
