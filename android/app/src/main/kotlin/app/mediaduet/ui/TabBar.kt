package app.mediaduet.ui

import androidx.compose.foundation.layout.size
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import app.mediaduet.R
import app.mediaduet.ui.components.AppIcons
import app.mediaduet.ui.components.liquid.LIQUID_TABS_HEIGHT
import app.mediaduet.ui.components.liquid.LiquidBottomTab
import app.mediaduet.ui.components.liquid.LiquidBottomTabs
import com.kyant.backdrop.Backdrop

/** Height of the floating tab bar (without the gesture inset under it). */
val TAB_BAR_HEIGHT = LIQUID_TABS_HEIGHT

/**
 * The floating tab bar (Home / Devices / Settings): Kyant0's liquid-glass tabs, whose selection lens
 * can also be dragged between the tabs. Tapping the current tab returns to its root ([onReselect]).
 */
@Composable
fun TabBar(current: Tab, backdrop: Backdrop, onSelect: (Tab) -> Unit, onReselect: (Tab) -> Unit, modifier: Modifier = Modifier) {
    val haptics = LocalHapticFeedback.current
    val now by rememberUpdatedState(current)
    LiquidBottomTabs(
        selectedTabIndex = current.ordinal,
        // A drag that ends on another tab (and the echo of every selection, ignored here).
        onTabSelected = { i ->
            val tab = Tab.entries[i]
            if (tab != now) {
                haptics.performHapticFeedback(HapticFeedbackType.SegmentTick)
                onSelect(tab)
            }
        },
        backdrop = backdrop,
        tabsCount = Tab.entries.size,
        modifier = modifier,
    ) {
        for (tab in Tab.entries) {
            val (icon, label) = when (tab) {
                Tab.HOME -> AppIcons.Home to stringResource(R.string.tab_home)
                Tab.DEVICES -> AppIcons.Devices to stringResource(R.string.tab_devices)
                Tab.SETTINGS -> AppIcons.Gear to stringResource(R.string.tab_settings)
            }
            LiquidBottomTab(
                {
                    haptics.performHapticFeedback(HapticFeedbackType.ContextClick)
                    if (tab == now) onReselect(tab) else onSelect(tab)
                },
                Modifier.semantics { selected = tab == current },
            ) {
                TabContent(icon, label)
            }
        }
    }
}

@Composable
private fun TabContent(icon: ImageVector, label: String) {
    val color = MaterialTheme.colorScheme.onSurface
    Icon(icon, null, Modifier.size(24.dp), tint = color)
    Text(label, style = MaterialTheme.typography.labelSmall, color = color, maxLines = 1, overflow = TextOverflow.Ellipsis)
}
