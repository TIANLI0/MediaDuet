package app.mediaduet.ui.components

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.VectorConverter
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.animation.core.spring
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.runtime.Stable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.RoundRect
import androidx.compose.ui.geometry.isSpecified
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.clipPath
import androidx.compose.ui.graphics.isSpecified
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import com.kyant.backdrop.highlight.Highlight
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import kotlin.math.abs
import kotlin.math.tanh
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.requiredHeightIn
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.input.InputTransformation
import androidx.compose.foundation.text.input.KeyboardActionHandler
import androidx.compose.foundation.text.input.TextFieldDecorator
import androidx.compose.foundation.text.input.TextFieldLineLimits
import androidx.compose.foundation.text.input.TextFieldState
import androidx.compose.foundation.text.input.maxLengthTrim
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.material3.HorizontalDivider
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import app.mediaduet.ui.theme.LocalGlass
import com.kyant.backdrop.Backdrop
import com.kyant.backdrop.drawBackdrop
import com.kyant.backdrop.effects.blur
import com.kyant.backdrop.effects.lens
import com.kyant.backdrop.effects.vibrancy
import com.kyant.backdrop.shadow.Shadow
import com.kyant.shapes.Capsule
import com.kyant.shapes.RoundedRectangle

/** Corner radius used by every card (8 dp grid). */
val CardRadius = 24.dp

/** A liquid-glass panel: the backdrop layer is blurred, refracted at the edges and tinted neutrally. */
@Composable
fun GlassCard(
    backdrop: Backdrop,
    modifier: Modifier = Modifier,
    cornerRadius: Dp = CardRadius,
    strong: Boolean = false,
    contentPadding: PaddingValues = PaddingValues(20.dp),
    content: @Composable ColumnScope.() -> Unit,
) {
    val glass = LocalGlass.current
    val surface = if (strong) glass.surfaceStrong else glass.surface
    Column(
        modifier
            .drawBackdrop(
                backdrop = backdrop,
                shape = { RoundedRectangle(cornerRadius) },
                effects = {
                    vibrancy()
                    blur(24.dp.toPx())
                    lens(12.dp.toPx(), 24.dp.toPx())
                },
                shadow = { Shadow(radius = 16.dp, color = Color.Black.copy(alpha = if (glass.isDark) 0.24f else 0.06f)) },
                onDrawSurface = { drawRect(surface) },
            )
            .padding(contentPadding),
        content = content,
    )
}

/**
 * Press state of a liquid-glass control (iOS 26 style): the glass swells a little while pressed,
 * stretches elastically toward a dragging finger and glows under it; everything springs back on
 * release. It only observes the pointer (nothing is consumed), so clicks work as usual.
 */
@Stable
class LiquidPress(private val scope: CoroutineScope) {
    private val pressAnim = Animatable(0f)
    private val dragAnim = Animatable(Offset.Zero, Offset.VectorConverter)

    /** 0 (released) … 1 (pressed). */
    val progress: Float get() = pressAnim.value
    val drag: Offset get() = dragAnim.value
    var position by mutableStateOf(Offset.Unspecified)
        private set

    val modifier: Modifier = Modifier.pointerInput(Unit) {
        awaitEachGesture {
            val down = awaitFirstDown(requireUnconsumed = false, pass = PointerEventPass.Initial)
            position = down.position
            scope.launch { pressAnim.animateTo(1f, spring(dampingRatio = 0.62f, stiffness = 520f)) }
            while (true) {
                val change = awaitPointerEvent(PointerEventPass.Initial).changes.firstOrNull { it.id == down.id } ?: break
                if (!change.pressed) break
                position = change.position
                val d = change.position - down.position
                scope.launch { dragAnim.snapTo(d) }
            }
            scope.launch { pressAnim.animateTo(0f, spring(dampingRatio = 0.5f, stiffness = 260f)) }
            scope.launch { dragAnim.animateTo(Offset.Zero, spring(dampingRatio = 0.45f, stiffness = 320f)) }
        }
    }
}

@Composable
fun rememberLiquidPress(): LiquidPress {
    val scope = rememberCoroutineScope()
    return remember(scope) { LiquidPress(scope) }
}

/**
 * The liquid-glass material for controls: a nearly clear [tint] over the backdrop, refracted by a
 * deep lens with chromatic aberration at the rim, a specular highlight, a soft shadow, and the
 * [press] response (swell, elastic stretch, glow under the finger).
 */
fun Modifier.liquidGlass(
    backdrop: Backdrop,
    press: LiquidPress?,
    tint: Color,
    isDark: Boolean,
    overlay: Color = Color.Unspecified,
): Modifier = drawBackdrop(
    backdrop = backdrop,
    shape = { Capsule() },
    effects = {
        val m = size.minDimension
        vibrancy()
        blur(2.dp.toPx())
        lens(m * 0.22f, m * 0.5f, depthEffect = true, chromaticAberration = true)
    },
    highlight = { glassHighlight(isDark) },
    shadow = { glassShadow(isDark) },
    layerBlock = if (press == null) null else {
        {
            val p = press.progress
            val grow = 1f + p * (6.dp.toPx() / size.height).coerceAtMost(0.14f)
            val d = press.drag
            val reach = size.minDimension * 0.18f
            translationX = reach * tanh(d.x / (reach * 5f))
            translationY = reach * tanh(d.y / (reach * 5f))
            val dist = d.getDistance()
            val stretch = (dist / size.maxDimension).coerceAtMost(1f) * 0.1f
            val cx = if (dist > 0f) abs(d.x) / dist else 0f
            val cy = if (dist > 0f) abs(d.y) / dist else 0f
            scaleX = grow + stretch * (cx - 0.5f * cy)
            scaleY = grow + stretch * (cy - 0.5f * cx)
        }
    },
    onDrawSurface = {
        drawRect(tint)
        if (overlay.isSpecified) drawRect(overlay)
    },
    onDrawFront = if (press == null) null else {
        { drawPressGlow(press, isDark) }
    },
)

/** Specular highlight of liquid glass (a little softer on dark backgrounds). */
fun glassHighlight(isDark: Boolean): Highlight = Highlight.Default.copy(alpha = if (isDark) 0.85f else 1f)

/** Soft shadow under floating liquid glass. */
fun glassShadow(isDark: Boolean): Shadow = Shadow(radius = 14.dp, color = Color.Black.copy(alpha = if (isDark) 0.28f else 0.10f))

/** The glow under the finger while a liquid-glass control is pressed (clipped to its capsule). */
fun DrawScope.drawPressGlow(press: LiquidPress, isDark: Boolean) {
    val p = press.progress
    val at = press.position
    if (p > 0.01f && at.isSpecified) {
        val r = size.height / 2
        clipPath(Path().apply { addRoundRect(RoundRect(0f, 0f, size.width, size.height, r, r)) }) {
            drawRect(
                Brush.radialGradient(
                    0f to Color.White.copy(alpha = (if (isDark) 0.22f else 0.38f) * p),
                    1f to Color.Transparent,
                    center = at,
                    radius = size.maxDimension * 0.8f,
                ),
                blendMode = BlendMode.Plus,
            )
        }
    }
}

/**
 * The material of the floating bars (tab bar and mini player): both are tinted with
 * [app.mediaduet.ui.theme.GlassColors.bar] and use these blur / lens values, so they read as one.
 */
object BarGlass {
    val Blur = 12.dp
    val LensHeight = 24.dp
    val LensAmount = 24.dp
}

/** Minimum touch target of every control (Android accessibility guideline). */
val TouchTarget = 48.dp

/** The two heights of [GlassButton]: compact (rows, cards, chips) and large (the primary action of a view). */
object ButtonHeight {
    val Compact = 44.dp
    val Large = 52.dp
}

/**
 * Capsule liquid-glass button. Neutral by default; [prominent] uses the single accent colour and
 * is reserved for the primary action of a view.
 */
@Composable
fun GlassButton(
    onClick: () -> Unit,
    backdrop: Backdrop,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    prominent: Boolean = false,
    selected: Boolean = false,
    destructive: Boolean = false,
    /** [ButtonHeight.Compact] or [ButtonHeight.Large]. */
    height: Dp = ButtonHeight.Compact,
    horizontalPadding: Dp = 20.dp,
    content: @Composable RowScope.() -> Unit,
) {
    val press = rememberLiquidPress()
    val glass = LocalGlass.current
    val contentColor = when {
        prominent -> Color.White
        destructive -> glass.bad
        selected -> glass.accent
        else -> MaterialTheme.colorScheme.onSurface
    }
    // The glass is [height] tall; the touch area around it is at least [TouchTarget].
    Box(
        modifier
            .alpha(if (enabled) 1f else 0.4f)
            .then(press.modifier)
            .clickable(interactionSource = null, indication = null, enabled = enabled, role = Role.Button, onClick = onClick)
            .defaultMinSize(minHeight = TouchTarget),
        contentAlignment = Alignment.Center,
        propagateMinConstraints = true,
    ) {
        Row(
            Modifier
                .requiredHeightIn(min = height)
                .liquidGlass(
                    backdrop,
                    press.takeIf { enabled },
                    tint = when {
                        prominent -> glass.accent.copy(alpha = 0.88f)
                        selected -> glass.controlActive
                        else -> glass.control
                    },
                    isDark = glass.isDark,
                    overlay = if (selected && !prominent) glass.accent.copy(alpha = 0.12f) else Color.Unspecified,
                )
                .padding(horizontal = horizontalPadding),
            horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            CompositionLocalProvider(LocalContentColor provides contentColor) { content() }
        }
    }
}

/** Compact glass button with a text label (row actions: Remove, Fix, Pair, Reset …). */
@Composable
fun GlassTextButton(
    text: String,
    onClick: () -> Unit,
    backdrop: Backdrop,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    prominent: Boolean = false,
    destructive: Boolean = false,
) {
    GlassButton(onClick, backdrop, modifier, enabled = enabled, prominent = prominent, destructive = destructive, horizontalPadding = 16.dp) {
        Text(text, style = MaterialTheme.typography.labelLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

/**
 * Single-line text field on liquid glass: a label above, the field as a glass capsule (it brightens
 * and takes an accent tint while focused) and an optional hint below.
 */
@Composable
fun GlassTextField(
    state: TextFieldState,
    backdrop: Backdrop,
    label: String,
    modifier: Modifier = Modifier,
    supportingText: String? = null,
    maxLength: Int? = null,
    imeAction: ImeAction = ImeAction.Default,
    /** Replaces the IME action's default behaviour when set. */
    onImeAction: (() -> Unit)? = null,
) {
    val glass = LocalGlass.current
    val interaction = remember { MutableInteractionSource() }
    val focused by interaction.collectIsFocusedAsState()
    val onSurface = MaterialTheme.colorScheme.onSurface
    Column(modifier) {
        Text(
            label,
            style = MaterialTheme.typography.labelMedium,
            color = if (focused) glass.accent else MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(start = 16.dp, bottom = 6.dp),
        )
        BasicTextField(
            state = state,
            modifier = Modifier.fillMaxWidth(),
            inputTransformation = maxLength?.let { InputTransformation.maxLengthTrim(it) },
            textStyle = MaterialTheme.typography.bodyLarge.copy(color = onSurface),
            keyboardOptions = KeyboardOptions(imeAction = imeAction),
            onKeyboardAction = onImeAction?.let { action -> KeyboardActionHandler { action() } },
            lineLimits = TextFieldLineLimits.SingleLine,
            interactionSource = interaction,
            cursorBrush = SolidColor(glass.accent),
            decorator = TextFieldDecorator { inner ->
                Box(
                    Modifier
                        .fillMaxWidth()
                        .liquidGlass(
                            backdrop, null,
                            tint = if (focused) glass.controlActive else glass.control,
                            isDark = glass.isDark,
                            overlay = if (focused) glass.accent.copy(alpha = 0.10f) else Color.Unspecified,
                        )
                        .heightIn(min = ButtonHeight.Large)
                        .padding(horizontal = 20.dp),
                    contentAlignment = Alignment.CenterStart,
                ) { inner() }
            },
        )
        if (supportingText != null) {
            Text(
                supportingText,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 16.dp, top = 6.dp),
            )
        }
    }
}

/** Circular liquid-glass icon button (transport controls, small toolbar buttons). */
@Composable
fun GlassIconButton(
    icon: ImageVector,
    contentDescription: String,
    onClick: () -> Unit,
    backdrop: Backdrop,
    modifier: Modifier = Modifier,
    size: Dp = 56.dp,
    iconSize: Dp = 26.dp,
    enabled: Boolean = true,
    active: Boolean = false,
) {
    val press = rememberLiquidPress()
    val glass = LocalGlass.current
    // Smaller buttons keep their look; the touch area around them is at least [TouchTarget].
    Box(
        modifier
            .size(maxOf(size, TouchTarget))
            .alpha(if (enabled) 1f else 0.35f)
            .then(press.modifier)
            .clickable(interactionSource = null, indication = null, enabled = enabled, role = Role.Button, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Box(
            Modifier
                .size(size)
                .liquidGlass(backdrop, press.takeIf { enabled }, if (active) glass.controlActive else glass.control, glass.isDark),
            contentAlignment = Alignment.Center,
        ) {
            Icon(icon, contentDescription, Modifier.size(iconSize), tint = if (active) glass.accent else MaterialTheme.colorScheme.onSurface)
        }
    }
}

/** Small neutral pill: optional status dot or icon + label. */
@Composable
fun StatusChip(text: String, modifier: Modifier = Modifier, dot: Color? = null, icon: ImageVector? = null) {
    val glass = LocalGlass.current
    Row(
        modifier
            .background(glass.fill, RoundedCornerShape(50))
            .padding(horizontal = 10.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        when {
            icon != null -> Icon(icon, null, Modifier.size(14.dp), tint = dot ?: MaterialTheme.colorScheme.onSurfaceVariant)
            dot != null -> Box(Modifier.size(6.dp).background(dot, CircleShape))
        }
        Text(text, style = MaterialTheme.typography.labelMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

/**
 * Leading icon of list rows and the gap after it: a row's text starts [RowTextInset] after the
 * card's content edge.
 */
val RowIconSize = 22.dp
val RowIconGap = 16.dp
val RowTextInset = RowIconSize + RowIconGap

/**
 * Hairline between list rows. One rule everywhere (iOS-style): between rows with a leading icon
 * it starts at the text column ([RowTextInset]), otherwise at the card's content edge; it always
 * ends at the content edge. Cards whose rows pad themselves pass that padding as [cardPadding].
 */
@Composable
fun ListDivider(modifier: Modifier = Modifier, icon: Boolean = false, cardPadding: Dp = 0.dp) {
    HorizontalDivider(
        modifier.padding(start = cardPadding + if (icon) RowTextInset else 0.dp, end = cardPadding),
        thickness = 0.5.dp,
        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f),
    )
}

@Composable
fun SectionHeader(text: String, modifier: Modifier = Modifier) {
    Text(
        text,
        modifier.padding(start = 8.dp, top = 16.dp, bottom = 0.dp),
        style = MaterialTheme.typography.titleMedium,
        color = MaterialTheme.colorScheme.onSurface,
    )
}

/** Row inside a glass card: title + optional subtitle + trailing slot. */
@Composable
fun InfoRow(
    title: String,
    modifier: Modifier = Modifier,
    subtitle: String? = null,
    leading: (@Composable () -> Unit)? = null,
    trailing: (@Composable () -> Unit)? = null,
    /** Makes the whole row tappable (e.g. toggles the row's switch). */
    onClick: (() -> Unit)? = null,
    role: Role = Role.Button,
) {
    val haptics = androidx.compose.ui.platform.LocalHapticFeedback.current
    // The rounded press highlight is drawn behind the content instead of clipping the row: a clip
    // would also cut off a liquid-glass switch, whose thumb swells past its track while pressed.
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val highlight by animateFloatAsState(if (pressed) 1f else 0f, tween(if (pressed) 60 else 220), label = "rowPress")
    val highlightColor = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f)
    Row(
        modifier
            .fillMaxWidth()
            .then(
                if (onClick != null) {
                    Modifier
                        .drawBehind {
                            if (highlight > 0f) {
                                drawRoundRect(highlightColor.copy(alpha = highlightColor.alpha * highlight), cornerRadius = CornerRadius(12.dp.toPx()))
                            }
                        }
                        .clickable(interaction, indication = null, role = role) {
                            if (role == Role.Switch) haptics.performHapticFeedback(androidx.compose.ui.hapticfeedback.HapticFeedbackType.ContextClick)
                            onClick()
                        }
                } else Modifier,
            )
            .heightIn(min = 56.dp)
            .padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (leading != null) {
            leading()
            Spacer(Modifier.width(RowIconGap))
        }
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            if (subtitle != null) {
                Text(subtitle, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        if (trailing != null) {
            Spacer(Modifier.width(16.dp))
            trailing()
        }
    }
}

/** Simple empty state: icon + one line. */
@Composable
fun EmptyState(icon: ImageVector, text: String, modifier: Modifier = Modifier, action: (@Composable () -> Unit)? = null) {
    Column(
        modifier.fillMaxWidth().padding(vertical = 48.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Icon(icon, null, Modifier.size(48.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(text, style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
        if (action != null) action()
    }
}
