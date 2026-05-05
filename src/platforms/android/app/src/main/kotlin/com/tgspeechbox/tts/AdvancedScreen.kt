/*
 * AdvancedScreen — Tab 2: "Advanced".
 *
 * Voice quality sliders (VoicingTone + FrameEx) and a language filter
 * dialog behind a button.  All TalkBack-accessible.
 *
 * License: GPL-3.0
 */

package com.tgspeechbox.tts

import android.content.Intent
import android.net.Uri
import android.os.PowerManager
import android.provider.Settings
import com.tgspeechbox.tts.compose.AccessibleSlider
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Checkbox
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.isTraversalGroup
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlin.math.roundToInt

@Composable
fun AdvancedScreen(
    viewModel: TgsbViewModel,
    snackbarHostState: SnackbarHostState
) {
    var showLanguageDialog by remember { mutableStateOf(false) }
    var showResetDialog by remember { mutableStateOf(false) }
    var resetAll by remember { mutableStateOf(false) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(20.dp)
    ) {
        Text(
            text = stringResource(R.string.advanced_subtitle),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        Spacer(Modifier.height(12.dp))

        // ── Voice selector (per-voice settings, matching iOS) ────────
        Column(modifier = Modifier.semantics { isTraversalGroup = true }) {
            EditingVoiceDropdown(viewModel)
        }

        Spacer(Modifier.height(12.dp))

        // ── Reset to Defaults ───────────────────────────────────────
        Column(modifier = Modifier.semantics { isTraversalGroup = true }) {
            Button(
                onClick = { showResetDialog = true },
                modifier = Modifier.fillMaxWidth()
            ) {
                Text(stringResource(R.string.reset_defaults_button))
            }
        }

        Spacer(Modifier.height(20.dp))

        // ── Pitch section ───────────────────────────────────────────
        Column(modifier = Modifier.semantics { isTraversalGroup = true }) {
            Text(
                text = stringResource(R.string.pitch_section_title),
                style = MaterialTheme.typography.headlineMedium,
                modifier = Modifier.semantics { heading() }
            )

            Spacer(Modifier.height(12.dp))

            PitchModeDropdown(viewModel)

            Spacer(Modifier.height(8.dp))

            VoicingToneSlider(
                label = stringResource(R.string.inflection_scale_label),
                flow = viewModel.inflectionScale,
                onChange = { viewModel.onInflectionScaleChanged(it) },
                format = { v -> "${v.roundToInt()}" }
            )
        }

        Spacer(Modifier.height(20.dp))

        // ── Voice Quality section ───────────────────────────────────
        Column(modifier = Modifier.semantics { isTraversalGroup = true }) {
            Text(
                text = stringResource(R.string.voice_quality_title),
                style = MaterialTheme.typography.headlineMedium,
                modifier = Modifier.semantics { heading() }
            )

            Spacer(Modifier.height(12.dp))

            VoicingToneSlider(
                label = stringResource(R.string.inflection_label),
                flow = viewModel.inflection,
                onChange = { viewModel.onInflectionChanged(it) },
                format = { v -> "${v.roundToInt()}" }
            )
            VoicingToneSlider(
                label = stringResource(R.string.voice_tilt_label),
                flow = viewModel.voiceTilt,
                onChange = { viewModel.onVoiceTiltChanged(it) },
                format = { v -> "${(v - 50).roundToInt()}" }
            )
            VoicingToneSlider(
                label = stringResource(R.string.speed_quotient_label),
                flow = viewModel.speedQuotient,
                onChange = { viewModel.onSpeedQuotientChanged(it) },
                format = { v -> "${v.roundToInt()}" }
            )
            VoicingToneSlider(
                label = stringResource(R.string.aspiration_tilt_label),
                flow = viewModel.aspirationTilt,
                onChange = { viewModel.onAspirationTiltChanged(it) },
                format = { v -> "${(v - 50).roundToInt()}" }
            )
            VoicingToneSlider(
                label = stringResource(R.string.cascade_bw_label),
                flow = viewModel.cascadeBwScale,
                onChange = { viewModel.onCascadeBwScaleChanged(it) },
                format = { v -> "${v.roundToInt()}" }
            )
            VoicingToneSlider(
                label = stringResource(R.string.noise_glottal_mod_label),
                flow = viewModel.noiseGlottalMod,
                onChange = { viewModel.onNoiseGlottalModChanged(it) },
                format = { v -> "${v.roundToInt()}" }
            )
            VoicingToneSlider(
                label = stringResource(R.string.pitch_sync_f1_label),
                flow = viewModel.pitchSyncF1,
                onChange = { viewModel.onPitchSyncF1Changed(it) },
                format = { v -> "${(v - 50).roundToInt()}" }
            )
            VoicingToneSlider(
                label = stringResource(R.string.pitch_sync_b1_label),
                flow = viewModel.pitchSyncB1,
                onChange = { viewModel.onPitchSyncB1Changed(it) },
                format = { v -> "${(v - 50).roundToInt()}" }
            )
            VoicingToneSlider(
                label = stringResource(R.string.voice_tremor_label),
                flow = viewModel.voiceTremor,
                onChange = { viewModel.onVoiceTremorChanged(it) },
                format = { v -> "${v.roundToInt()}" }
            )
            VoicingToneSlider(
                label = stringResource(R.string.head_size_label),
                flow = viewModel.headSize,
                onChange = { viewModel.onHeadSizeChanged(it) },
                format = { v -> "${v.roundToInt()}" }
            )
            VoicingToneSlider(
                label = stringResource(R.string.chorus_depth_label),
                flow = viewModel.chorusDepth,
                onChange = { viewModel.onChorusDepthChanged(it) },
                format = { v -> "${v.roundToInt()}" }
            )
            VoicingToneSlider(
                label = stringResource(R.string.chorus_detune_label),
                flow = viewModel.chorusDetune,
                onChange = { viewModel.onChorusDetuneChanged(it) },
                format = { v -> "${v.roundToInt()}" }
            )
        }

        Spacer(Modifier.height(20.dp))

        // ── Per-Frame Voice Quality section ─────────────────────────
        Column(modifier = Modifier.semantics { isTraversalGroup = true }) {
            Text(
                text = stringResource(R.string.frame_quality_title),
                style = MaterialTheme.typography.headlineMedium,
                modifier = Modifier.semantics { heading() }
            )

            Spacer(Modifier.height(12.dp))

            VoicingToneSlider(
                label = stringResource(R.string.creakiness_label),
                flow = viewModel.creakiness,
                onChange = { viewModel.onCreakinessChanged(it) },
                format = { v -> "${v.roundToInt()}" }
            )
            VoicingToneSlider(
                label = stringResource(R.string.breathiness_label),
                flow = viewModel.breathiness,
                onChange = { viewModel.onBreathinessChanged(it) },
                format = { v -> "${v.roundToInt()}" }
            )
            VoicingToneSlider(
                label = stringResource(R.string.jitter_label),
                flow = viewModel.jitter,
                onChange = { viewModel.onJitterChanged(it) },
                format = { v -> "${v.roundToInt()}" }
            )
            VoicingToneSlider(
                label = stringResource(R.string.shimmer_label),
                flow = viewModel.shimmer,
                onChange = { viewModel.onShimmerChanged(it) },
                format = { v -> "${v.roundToInt()}" }
            )
            VoicingToneSlider(
                label = stringResource(R.string.glottal_sharpness_label),
                flow = viewModel.glottalSharpness,
                onChange = { viewModel.onGlottalSharpnessChanged(it) },
                format = { v -> "${v.roundToInt()}" }
            )
        }

        Spacer(Modifier.height(20.dp))

        // ── System Rate Override section ────────────────────────────
        Column(modifier = Modifier.semantics { isTraversalGroup = true }) {
            Text(
                text = stringResource(R.string.system_rate_title),
                style = MaterialTheme.typography.headlineMedium,
                modifier = Modifier.semantics { heading() }
            )

            Spacer(Modifier.height(12.dp))

            val overrideRate by viewModel.overrideSystemRate.collectAsState()
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .toggleable(
                        value = overrideRate,
                        onValueChange = { viewModel.onOverrideSystemRateChanged(it) },
                        role = androidx.compose.ui.semantics.Role.Checkbox
                    )
                    .padding(vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Checkbox(
                    checked = overrideRate,
                    onCheckedChange = null
                )
                Column(modifier = Modifier.padding(start = 12.dp)) {
                    Text(
                        text = stringResource(R.string.override_rate_label),
                        style = MaterialTheme.typography.bodyLarge
                    )
                    Text(
                        text = stringResource(R.string.override_rate_description),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }

            val globalRateVal by viewModel.globalRate.collectAsState()
            AccessibleSlider(
                label = stringResource(R.string.global_rate_label),
                displayValue = "${"%.1f".format(globalRateVal)}x",
                value = globalRateVal,
                onValueChange = { viewModel.onGlobalRateChanged(it) },
                valueRange = 0.3f..4.0f,
                steps = 36,
                enabled = overrideRate
            )
        }

        // Rate Boost checkbox
        val rateBoost by viewModel.rateBoostEnabled.collectAsState()
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .toggleable(
                    value = rateBoost,
                    onValueChange = { viewModel.onRateBoostEnabledChanged(it) },
                    role = androidx.compose.ui.semantics.Role.Checkbox
                )
                .padding(vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Checkbox(
                checked = rateBoost,
                onCheckedChange = null
            )
            Text(
                text = "Rate boost",
                modifier = Modifier.padding(start = 12.dp)
            )
        }

        Spacer(Modifier.height(20.dp))

        // ── Output section ──────────────────────────────────────────
        Column(modifier = Modifier.semantics { isTraversalGroup = true }) {
            Text(
                text = stringResource(R.string.output_section_title),
                style = MaterialTheme.typography.headlineMedium,
                modifier = Modifier.semantics { heading() }
            )

            Spacer(Modifier.height(12.dp))

            PauseModeDropdown(viewModel)

            Spacer(Modifier.height(8.dp))

            val volumeVal by viewModel.systemVolume.collectAsState()
            val volumePercent = (volumeVal * 100).roundToInt()
            AccessibleSlider(
                label = stringResource(R.string.system_volume_label),
                displayValue = "$volumePercent%",
                value = volumeVal,
                onValueChange = { viewModel.onSystemVolumeChanged(it) },
                valueRange = 0.05f..1.0f,
                steps = 18
            )

            Spacer(Modifier.height(8.dp))

            val sampleRateIdx by viewModel.sampleRateIndex.collectAsState()
            val sampleRates = TgsbViewModel.SAMPLE_RATES
            val currentRate = sampleRates[sampleRateIdx.roundToInt().coerceIn(0, sampleRates.size - 1)]
            AccessibleSlider(
                label = stringResource(R.string.sample_rate_label),
                displayValue = "$currentRate Hz",
                value = sampleRateIdx,
                onValueChange = { viewModel.onSampleRateChanged(it) },
                valueRange = 0f..(sampleRates.size - 1).toFloat(),
                steps = sampleRates.size - 2
            )

            Spacer(Modifier.height(8.dp))

            val lockLang by viewModel.lockLanguage.collectAsState()
            val langIdx by viewModel.selectedLanguageIndex.collectAsState()
            val lockLangName = viewModel.languages.getOrNull(langIdx)?.displayName ?: ""

            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .toggleable(
                        value = lockLang,
                        onValueChange = { viewModel.onLockLanguageChanged(it) },
                        role = androidx.compose.ui.semantics.Role.Checkbox
                    )
                    .padding(vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Checkbox(
                    checked = lockLang,
                    onCheckedChange = null
                )
                Column(modifier = Modifier.padding(start = 12.dp)) {
                    Text(text = "Lock language")
                    if (lockLang) {
                        Text(
                            text = lockLangName,
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }
        }

        Spacer(Modifier.height(16.dp))

        // ── Battery Optimization ────────────────────────────────────
        // Request exemption so Android doesn't kill the TTS service
        // in the background (important for screen reader users).
        val context = LocalContext.current
        val pm = context.getSystemService(PowerManager::class.java)
        val isExempt = pm?.isIgnoringBatteryOptimizations(context.packageName) == true

        if (!isExempt) {
            OutlinedButton(
                onClick = {
                    val intent = Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS).apply {
                        data = Uri.parse("package:${context.packageName}")
                    }
                    context.startActivity(intent)
                },
                modifier = Modifier.fillMaxWidth()
            ) {
                Text(stringResource(R.string.battery_optimization_button))
            }
        }

        Spacer(Modifier.height(24.dp))

        // ── Engine Languages button ─────────────────────────────────
        Column(modifier = Modifier.semantics { isTraversalGroup = true }) {
            OutlinedButton(
                onClick = { showLanguageDialog = true },
                modifier = Modifier.fillMaxWidth()
            ) {
                Text(stringResource(R.string.choose_languages_button))
            }
        }
    }

    // ── Reset to Defaults confirmation dialog ──────────────────────
    if (showResetDialog) {
        val voiceName = viewModel.voices.getOrNull(
            viewModel.selectedVoiceIndex.collectAsState().value
        )?.label ?: "Adam"
        val message = if (resetAll)
            stringResource(R.string.reset_defaults_message_all)
        else
            stringResource(R.string.reset_defaults_message_single, voiceName)

        AlertDialog(
            onDismissRequest = { showResetDialog = false; resetAll = false },
            title = { Text(stringResource(R.string.reset_defaults_title)) },
            text = {
                Column {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .toggleable(
                                value = resetAll,
                                onValueChange = { resetAll = it },
                                role = androidx.compose.ui.semantics.Role.Switch
                            )
                            .padding(vertical = 6.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Checkbox(
                            checked = resetAll,
                            onCheckedChange = null
                        )
                        Text(
                            text = stringResource(R.string.reset_all_toggle),
                            style = MaterialTheme.typography.bodyLarge,
                            modifier = Modifier.padding(start = 12.dp)
                        )
                    }
                    Spacer(Modifier.height(8.dp))
                    Text(
                        text = message,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    viewModel.resetToDefaults(allVoices = resetAll)
                    showResetDialog = false
                    resetAll = false
                }) {
                    Text(stringResource(R.string.reset_button))
                }
            },
            dismissButton = {
                TextButton(onClick = { showResetDialog = false; resetAll = false }) {
                    Text(stringResource(R.string.cancel_button))
                }
            }
        )
    }

    // ── Language filter dialog ───────────────────────────────────────
    if (showLanguageDialog) {
        LanguageFilterDialog(
            viewModel = viewModel,
            snackbarHostState = snackbarHostState,
            onDismiss = { showLanguageDialog = false }
        )
    }
}

/**
 * Pitch mode dropdown — 5 options matching iOS/macOS.
 * Uses plain Box + DropdownMenu instead of ExposedDropdownMenuBox
 * to avoid TalkBack traversal order bugs (ExposedDropdownMenuBox
 * creates internal Surface traversal groups that confuse linear nav).
 */
@Composable
private fun PitchModeDropdown(viewModel: TgsbViewModel) {
    val pitchModes = listOf(
        "espeak_style"   to stringResource(R.string.pitch_mode_espeak),
        "legacy"         to stringResource(R.string.pitch_mode_legacy),
        "fujisaki_style" to stringResource(R.string.pitch_mode_fujisaki),
        "impulse_style"  to stringResource(R.string.pitch_mode_impulse),
        "klatt_style"    to stringResource(R.string.pitch_mode_klatt),
    )

    val currentMode by viewModel.pitchMode.collectAsState()
    val currentLabel = pitchModes.firstOrNull { it.first == currentMode }?.second
        ?: pitchModes[0].second
    val label = stringResource(R.string.pitch_mode_label)

    var expanded by remember { mutableStateOf(false) }

    Box(modifier = Modifier.fillMaxWidth()) {
        Surface(
            onClick = { expanded = true },
            modifier = Modifier
                .fillMaxWidth()
                .semantics {
                    role = androidx.compose.ui.semantics.Role.DropdownList
                    contentDescription = "$label: $currentLabel"
                },
            shape = MaterialTheme.shapes.small,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline)
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(16.dp),
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Column {
                    Text(
                        text = label,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.clearAndSetSemantics {}
                    )
                    Text(
                        text = currentLabel,
                        style = MaterialTheme.typography.bodyLarge,
                        modifier = Modifier.clearAndSetSemantics {}
                    )
                }
            }
        }
        DropdownMenu(
            expanded = expanded,
            onDismissRequest = { expanded = false }
        ) {
            for ((modeId, modeLabel) in pitchModes) {
                DropdownMenuItem(
                    text = { Text(modeLabel) },
                    onClick = {
                        viewModel.onPitchModeChanged(modeId)
                        expanded = false
                    }
                )
            }
        }
    }
}

/**
 * Pause mode dropdown — 3 options (Off, Short, Long) matching iOS.
 * Plain Box + DropdownMenu for correct TalkBack traversal order.
 */
@Composable
private fun PauseModeDropdown(viewModel: TgsbViewModel) {
    val pauseModes = listOf(
        0 to stringResource(R.string.pause_mode_off),
        1 to stringResource(R.string.pause_mode_short),
        2 to stringResource(R.string.pause_mode_long),
    )

    val currentMode by viewModel.pauseMode.collectAsState()
    val currentLabel = pauseModes.firstOrNull { it.first == currentMode }?.second
        ?: pauseModes[1].second
    val label = stringResource(R.string.pause_mode_label)

    var expanded by remember { mutableStateOf(false) }

    Box(modifier = Modifier.fillMaxWidth()) {
        Surface(
            onClick = { expanded = true },
            modifier = Modifier
                .fillMaxWidth()
                .semantics {
                    role = androidx.compose.ui.semantics.Role.DropdownList
                    contentDescription = "$label: $currentLabel"
                },
            shape = MaterialTheme.shapes.small,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline)
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(16.dp),
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Column {
                    Text(
                        text = label,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.clearAndSetSemantics {}
                    )
                    Text(
                        text = currentLabel,
                        style = MaterialTheme.typography.bodyLarge,
                        modifier = Modifier.clearAndSetSemantics {}
                    )
                }
            }
        }
        DropdownMenu(
            expanded = expanded,
            onDismissRequest = { expanded = false }
        ) {
            for ((modeId, modeLabel) in pauseModes) {
                DropdownMenuItem(
                    text = { Text(modeLabel) },
                    onClick = {
                        viewModel.onPauseModeChanged(modeId)
                        expanded = false
                    }
                )
            }
        }
    }
}

/**
 * Voice selector dropdown for per-voice settings — "Editing Voice".
 * Matches iOS EngineSettingsView segmented picker.
 * Uses plain Box + DropdownMenu for correct TalkBack traversal order.
 */
@Composable
private fun EditingVoiceDropdown(viewModel: TgsbViewModel) {
    val voiceIndex by viewModel.selectedVoiceIndex.collectAsState()
    val currentLabel = viewModel.voices.getOrNull(voiceIndex)?.label ?: "Adam"
    val label = stringResource(R.string.editing_voice_label)

    var expanded by remember { mutableStateOf(false) }

    Box(modifier = Modifier.fillMaxWidth()) {
        Surface(
            onClick = { expanded = true },
            modifier = Modifier
                .fillMaxWidth()
                .semantics {
                    role = androidx.compose.ui.semantics.Role.DropdownList
                    contentDescription = "$label: $currentLabel"
                },
            shape = MaterialTheme.shapes.small,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline)
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(16.dp),
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Column {
                    Text(
                        text = label,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.clearAndSetSemantics {}
                    )
                    Text(
                        text = currentLabel,
                        style = MaterialTheme.typography.bodyLarge,
                        modifier = Modifier.clearAndSetSemantics {}
                    )
                }
            }
        }
        DropdownMenu(
            expanded = expanded,
            onDismissRequest = { expanded = false }
        ) {
            for ((index, voice) in viewModel.voices.withIndex()) {
                DropdownMenuItem(
                    text = { Text(voice.label) },
                    onClick = {
                        viewModel.onEditingVoiceSelected(index)
                        expanded = false
                    }
                )
            }
        }
    }
}

/**
 * Reusable slider with accessible label (single TalkBack swipe target).
 * Delegates to [AccessibleSlider] for keyboard + TalkBack semantics.
 */
@Composable
private fun VoicingToneSlider(
    label: String,
    flow: MutableStateFlow<Float>,
    onChange: (Float) -> Unit,
    format: (Float) -> String
) {
    val value by flow.collectAsState()
    AccessibleSlider(
        label = label,
        displayValue = format(value),
        value = value,
        onValueChange = { onChange(it) },
        valueRange = 0f..100f,
        steps = 99
    )
}

/**
 * Dialog with language checkboxes — moved out of the main scroll view.
 */
@Composable
private fun LanguageFilterDialog(
    viewModel: TgsbViewModel,
    snackbarHostState: SnackbarHostState,
    onDismiss: () -> Unit
) {
    val enabledKeys by viewModel.enabledLocaleKeys.collectAsState()
    val scope = rememberCoroutineScope()
    val errorMsg = stringResource(R.string.at_least_one_language)

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.supported_languages_title)) },
        text = {
            Column(
                modifier = Modifier.verticalScroll(rememberScrollState())
            ) {
                Text(
                    text = stringResource(R.string.supported_languages_hint),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Spacer(Modifier.height(8.dp))

                for ((localeKey, displayName) in viewModel.allLocaleEntries) {
                    val isChecked = localeKey in enabledKeys
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .toggleable(
                                value = isChecked,
                                onValueChange = { newValue ->
                                    val ok = viewModel.toggleLocaleKey(localeKey, newValue)
                                    if (!ok) {
                                        scope.launch {
                                            snackbarHostState.showSnackbar(errorMsg)
                                        }
                                    }
                                },
                                role = androidx.compose.ui.semantics.Role.Checkbox
                            )
                            .padding(vertical = 6.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Checkbox(
                            checked = isChecked,
                            onCheckedChange = null
                        )
                        Text(
                            text = displayName,
                            style = MaterialTheme.typography.bodyLarge,
                            modifier = Modifier.padding(start = 12.dp)
                        )
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) {
                Text(stringResource(R.string.done_button))
            }
        }
    )
}
