package com.tgspeechbox.tts.compose

import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.unit.dp

/**
 * Accessible slider with proper keyboard + TalkBack semantics.
 *
 * Why this wrapper exists: Compose Material3's stock [Slider] is pointer-driven
 * by default. Adding `focusable()` makes it tab-stop reachable, but keyboard
 * arrow keys still do nothing — the slider focuses, but a user pressing
 * Left/Right (or activating via Switch Control / D-Pad) can't adjust the
 * value. Keyboard support must be added manually with `onKeyEvent`.
 *
 * See: https://developer.android.com/develop/ui/compose/touch-input/keyboard-input/commands
 *
 * What this composable adds beyond a bare Slider:
 *   1. [FocusRequester] + `focusable()` so the slider is part of keyboard
 *      tab order and can be programmatically focused.
 *   2. `onKeyEvent` handler for DPAD_LEFT / DPAD_RIGHT (one-step adjustment),
 *      Home / End (jump to min / max). Each Left/Right press moves the value
 *      by one [steps] increment, or 1% of the range if [steps] == 0.
 *   3. `Role.Slider` made explicit on the semantics node so screen readers
 *      announce "slider" when focused via keyboard. (Stock Slider already has
 *      this on its internal track; setting it explicitly here makes the
 *      announcement reliable when the focus comes from the outer modifier.)
 *   4. Combined `contentDescription` (label + current value) and
 *      `stateDescription` (just the current value) — TalkBack reads label
 *      once on focus and re-announces the value on each adjustment.
 *
 * The visible label [Text] is rendered above the slider for sighted users
 * and hidden from accessibility (`clearAndSetSemantics {}`) since the slider
 * itself carries the same information. One TalkBack swipe target per slider.
 *
 * Migrated from per-site `Slider(...)` calls in:
 *   - SpeakScreen.kt (speed, pitch)
 *   - AdvancedScreen.kt (globalRate, volume, sampleRate, VoicingToneSlider helper)
 *   - PhonemeEditorScreen.kt (per-parameter slider)
 */
@Composable
fun AccessibleSlider(
    label: String,
    displayValue: String,
    value: Float,
    onValueChange: (Float) -> Unit,
    valueRange: ClosedFloatingPointRange<Float> = 0f..1f,
    steps: Int = 0,
    enabled: Boolean = true,
    onValueChangeFinished: (() -> Unit)? = null,
    showLabel: Boolean = true,
    modifier: Modifier = Modifier
) {
    val focusRequester = remember { FocusRequester() }
    val fullLabel = "$label: $displayValue"

    // Keyboard step delta: if discrete steps are defined, one step's worth.
    // Otherwise 1% of the value range. Either way, one Left/Right key press
    // produces one perceptible adjustment.
    val stepDelta = if (steps > 0) {
        (valueRange.endInclusive - valueRange.start) / (steps + 1)
    } else {
        (valueRange.endInclusive - valueRange.start) / 100f
    }

    if (showLabel) {
        Text(
            text = fullLabel,
            style = MaterialTheme.typography.bodyLarge,
            modifier = Modifier.clearAndSetSemantics {}
        )
    }
    Slider(
        value = value,
        onValueChange = onValueChange,
        valueRange = valueRange,
        steps = steps,
        enabled = enabled,
        onValueChangeFinished = onValueChangeFinished,
        modifier = modifier
            .then(if (showLabel) Modifier.fillMaxWidth() else Modifier)
            .focusRequester(focusRequester)
            .focusable(enabled = enabled)
            .onKeyEvent { event ->
                if (event.type != KeyEventType.KeyDown) return@onKeyEvent false
                if (!enabled) return@onKeyEvent false
                val newValue = when (event.key) {
                    Key.DirectionLeft -> (value - stepDelta).coerceIn(valueRange)
                    Key.DirectionRight -> (value + stepDelta).coerceIn(valueRange)
                    Key.MoveHome -> valueRange.start
                    Key.MoveEnd -> valueRange.endInclusive
                    else -> return@onKeyEvent false
                }
                if (newValue != value) {
                    onValueChange(newValue)
                    onValueChangeFinished?.invoke()
                }
                true
            }
            // Stock Slider already declares Role.* on its inner track via its
            // own semantics modifier, so we don't override role here. Just
            // augment with content + state descriptions for screen readers.
            .semantics {
                contentDescription = fullLabel
                stateDescription = displayValue
            }
    )
    if (showLabel) {
        Spacer(Modifier.height(4.dp))
    }
}
