/*
 * TgsbViewModel — Shared state for the TGSpeechBox consumer UI.
 *
 * Drives the Speak & Basics tab and the Advanced tab.  Uses
 * TgsbSpeakEngine (direct JNI) for speech — NOT the TextToSpeech
 * client API, which conflicts with TalkBack using the same service.
 *
 * License: GPL-3.0
 */

package com.tgspeechbox.tts

import android.app.Application
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.net.Uri
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.io.File
import java.util.Locale
import kotlin.math.roundToInt

/** One entry in the language dropdown (deduplicated). */
data class LanguageItem(
    val displayName: String,
    val langDef: TgsbTtsService.Companion.LangDef
)

class TgsbViewModel(application: Application) : AndroidViewModel(application) {

    companion object {
        private const val TAG = "TgsbUI"
        private const val PREF_PREFIX = "adv_"
        private const val PREF_SPEAK_LANG = "tgsb_speak_lang"
        val SAMPLE_RATES = listOf(11025, 16000, 22050, 44100)
    }

    private val prefs: SharedPreferences =
        application.getSharedPreferences(TgsbTtsService.PREFS_NAME, 0)

    private val engine = TgsbSpeakEngine(application)

    // ── UI state (Speak tab) ────────────────────────────────────────

    val textToSpeak = MutableStateFlow("Hello world. This is TGSpeechBox.")
    val selectedLanguageIndex = MutableStateFlow(0)
    val selectedVoiceIndex = MutableStateFlow(0)
    val speedRate = MutableStateFlow(1.0f)        // 0.3 – 3.0
    val pitchHz = MutableStateFlow(110f)           // 40 – 300 Hz
    val isSpeaking = MutableStateFlow(false)
    val engineReady = MutableStateFlow(false)
    val errorMessage = MutableStateFlow<String?>(null)

    // ── VoicingTone sliders (0–100, mapped to real ranges) ──────────

    val voiceTilt = MutableStateFlow(loadSlider("voiceTilt", 50f))
    val noiseGlottalMod = MutableStateFlow(loadSlider("noiseGlottalMod", 0f))
    val pitchSyncF1 = MutableStateFlow(loadSlider("pitchSyncF1", 50f))
    val pitchSyncB1 = MutableStateFlow(loadSlider("pitchSyncB1", 50f))
    val speedQuotient = MutableStateFlow(loadSlider("speedQuotient", 50f))
    val aspirationTilt = MutableStateFlow(loadSlider("aspirationTilt", 50f))
    val cascadeBwScale = MutableStateFlow(loadSlider("cascadeBwScale", 50f))
    val voiceTremor = MutableStateFlow(loadSlider("voiceTremor", 0f))
    val headSize = MutableStateFlow(loadSlider("headSize", if (currentVoiceId == "david") 100f else 50f))
    val chorusDepth = MutableStateFlow(loadSlider("chorusDepth", 0f))
    val chorusDetune = MutableStateFlow(loadSlider("chorusDetune", 33f))

    // ── Pitch settings ──────────────────────────────────────────────

    val pitchMode = MutableStateFlow(loadString("pitchMode", "espeak_style"))
    val inflectionScale = MutableStateFlow(loadSlider("inflectionScale", 58f))
    val inflection = MutableStateFlow(loadSlider("inflection", 50f))

    // ── System rate override (global, NOT per-voice) ────────────────

    val overrideSystemRate = MutableStateFlow(loadGlobalBool("overrideSystemRate", false))
    val globalRate = MutableStateFlow(loadGlobalFloat("globalRate", 1.0f))
    val rateBoostEnabled = MutableStateFlow(loadGlobalBool("rateBoostEnabled", false))

    // ── Output (global, NOT per-voice) ───────────────────────────────

    val lockLanguage = MutableStateFlow(loadGlobalBool("lockLanguage", false))
    val pauseMode = MutableStateFlow(loadGlobalInt("pauseMode", 1))  // 0=off, 1=short, 2=long
    val systemVolume = MutableStateFlow(loadGlobalFloat("systemVolume", 1.0f))
    val sampleRateIndex = MutableStateFlow(loadGlobalInt("sampleRate", 22050).let { rate ->
        SAMPLE_RATES.indexOfFirst { it == rate }.coerceAtLeast(0).toFloat()
    })

    // ── FrameEx sliders (0–100) ─────────────────────────────────────

    val creakiness = MutableStateFlow(loadSlider("creakiness", 0f))
    val breathiness = MutableStateFlow(loadSlider("breathiness", 0f))
    val jitter = MutableStateFlow(loadSlider("jitter", 0f))
    val shimmer = MutableStateFlow(loadSlider("shimmer", 0f))
    val glottalSharpness = MutableStateFlow(loadSlider("glottalSharpness", 50f))

    // ── Data lists ────────────────────────────────────────────────────

    val languages: List<LanguageItem> = buildLanguageList()
    val voices: List<TgsbTtsService.Companion.VoiceDef> = TgsbTtsService.VOICES

    // ── Language filter state ────────────────────────────────────────

    private val _enabledLocaleKeys = MutableStateFlow(loadEnabledKeys())
    val enabledLocaleKeys: StateFlow<Set<String>> = _enabledLocaleKeys
    val allLocaleEntries: List<Pair<String, String>> = buildLocaleEntries()

    // ── Init ────────────────────────────────────────────────────────

    init {
        val savedPreset = prefs.getString(
            TgsbTtsService.PREF_VOICE_PRESET,
            TgsbTtsService.DEFAULT_PRESET
        ) ?: TgsbTtsService.DEFAULT_PRESET
        val idx = voices.indexOfFirst { it.id == savedPreset }
        if (idx >= 0) selectedVoiceIndex.value = idx

        // Restore saved Speak-tab language, fall back to device locale.
        val savedSpeakLang = prefs.getString(PREF_SPEAK_LANG, null)
        val savedLangIdx = if (savedSpeakLang != null) {
            languages.indexOfFirst { it.langDef.tgsbLang == savedSpeakLang }
        } else -1

        if (savedLangIdx >= 0) {
            selectedLanguageIndex.value = savedLangIdx
            Log.i(TAG, "Restored speak language: $savedSpeakLang")
        } else {
            val deviceLocale = Locale.getDefault()
            val langMatch = languages.indexOfFirst { item ->
                val ld = item.langDef.displayLocale
                ld.language == deviceLocale.language && ld.country == deviceLocale.country
            }
            val langFallback = if (langMatch < 0) {
                languages.indexOfFirst { it.langDef.displayLocale.language == deviceLocale.language }
            } else langMatch
            if (langFallback >= 0) {
                selectedLanguageIndex.value = langFallback
                Log.i(TAG, "Default language: ${languages[langFallback].displayName}")
            }
        }

        if (engine.start()) {
            engineReady.value = true
            errorMessage.value = null

            val ld = languages[selectedLanguageIndex.value].langDef
            engine.setLanguage(ld.espeakLang, ld.tgsbLang)
            applyStoredOverrides(ld.tgsbLang)
            reapplyDictOverrides(ld.tgsbLang)
            engine.setVoice(voices[selectedVoiceIndex.value].id)
            applyVoicingTone()
            applyFrameExDefaults()
            applyPitchSettings()
            engine.setVolume(systemVolume.value)
            val savedRate = SAMPLE_RATES[sampleRateIndex.value.roundToInt().coerceIn(0, SAMPLE_RATES.size - 1)]
            engine.setSampleRate(savedRate)
            engine.setPauseMode(pauseMode.value)

            engine.onSpeakingChanged = { speaking ->
                isSpeaking.value = speaking
            }

            Log.i(TAG, "Standalone engine ready")
        } else {
            errorMessage.value = "Failed to start speech engine."
            Log.e(TAG, "Engine start failed")
        }
    }

    override fun onCleared() {
        engine.shutdown()
        super.onCleared()
    }

    // ── Speak / Stop ────────────────────────────────────────────────

    fun speak() {
        val text = textToSpeak.value
        if (text.isBlank()) return

        val ld = languages[selectedLanguageIndex.value].langDef
        engine.setLanguage(ld.espeakLang, ld.tgsbLang)
        applyStoredOverrides(ld.tgsbLang)
        reapplyDictOverrides(ld.tgsbLang)
        engine.setVoice(voices[selectedVoiceIndex.value].id)
        applyVoicingTone()
        applyFrameExDefaults()
        applyPitchSettings()
        engine.setPauseMode(pauseMode.value)

        errorMessage.value = null
        val effectiveSpeed = speedRate.value.toDouble() * (if (rateBoostEnabled.value) 2.0 else 1.0)
        engine.speak(text, effectiveSpeed, pitchHz.value.toDouble())
        Log.i(TAG, "speak: lang=${ld.espeakLang} speed=$effectiveSpeed pitch=${pitchHz.value}")
    }

    /** Speak arbitrary text using current Speak-tab settings. */
    fun speakText(text: String) {
        if (text.isBlank()) return
        val ld = languages[selectedLanguageIndex.value].langDef
        engine.setLanguage(ld.espeakLang, ld.tgsbLang)
        applyStoredOverrides(ld.tgsbLang)
        reapplyDictOverrides(ld.tgsbLang)
        engine.setVoice(voices[selectedVoiceIndex.value].id)
        applyVoicingTone()
        applyFrameExDefaults()
        applyPitchSettings()
        engine.setPauseMode(pauseMode.value)
        val effectiveSpeed = speedRate.value.toDouble() * (if (rateBoostEnabled.value) 2.0 else 1.0)
        engine.speak(text, effectiveSpeed, pitchHz.value.toDouble())
    }

    /** Preview a dictionary entry: uses IPA directly if available, else speaks replacement text. */
    fun previewDictEntry(fromText: String, toText: String, toIpa: String = "") {
        if (toIpa.isNotBlank()) {
            val ld = languages[selectedLanguageIndex.value].langDef
            engine.setLanguage(ld.espeakLang, ld.tgsbLang)
            applyStoredOverrides(ld.tgsbLang)
            engine.setVoice(voices[selectedVoiceIndex.value].id)
            applyVoicingTone()
            applyFrameExDefaults()
            applyPitchSettings()
            val effectiveSpeed = speedRate.value.toDouble() * (if (rateBoostEnabled.value) 2.0 else 1.0)
            engine.previewPhoneme(toIpa, effectiveSpeed, pitchHz.value.toDouble())
        } else {
            speakText(toText)
        }
    }

    fun stop() {
        engine.stop()
    }

    // ── Selection handlers ──────────────────────────────────────────

    fun onLanguageSelected(index: Int) {
        selectedLanguageIndex.value = index
        val ld = languages[index].langDef
        engine.setLanguage(ld.espeakLang, ld.tgsbLang)
        applyStoredOverrides(ld.tgsbLang)
        reapplyDictOverrides(ld.tgsbLang)
        prefs.edit().putString(PREF_SPEAK_LANG, ld.tgsbLang).apply()
        Log.i(TAG, "Language selected: ${ld.espeakLang}")
    }

    fun onVoiceSelected(index: Int) {
        selectedVoiceIndex.value = index
        val voiceId = voices[index].id
        engine.setVoice(voiceId)
        prefs.edit().putString(TgsbTtsService.PREF_VOICE_PRESET, voiceId).apply()
        loadSettingsForVoice()
        applyVoicingTone()
        applyFrameExDefaults()
        applyPitchSettings()
    }

    /** Switch which voice's settings are shown in Engine Settings,
     *  WITHOUT changing TalkBack's active voice or saving the preset. */
    fun onEditingVoiceSelected(index: Int) {
        selectedVoiceIndex.value = index
        loadSettingsForVoice()
    }

    /**
     * Reload all per-voice slider/setting StateFlows from SharedPreferences.
     * Called when the user switches voices so the UI reflects that voice's
     * saved values (matching iOS loadSettingsForVoice behaviour).
     */
    private fun loadSettingsForVoice() {
        // VoicingTone sliders
        voiceTilt.value          = loadSlider("voiceTilt", 50f)
        noiseGlottalMod.value    = loadSlider("noiseGlottalMod", 0f)
        pitchSyncF1.value        = loadSlider("pitchSyncF1", 50f)
        pitchSyncB1.value        = loadSlider("pitchSyncB1", 50f)
        speedQuotient.value      = loadSlider("speedQuotient", 50f)
        aspirationTilt.value     = loadSlider("aspirationTilt", 50f)
        cascadeBwScale.value     = loadSlider("cascadeBwScale", 50f)
        voiceTremor.value        = loadSlider("voiceTremor", 0f)
        headSize.value           = loadSlider("headSize", if (currentVoiceId == "david") 100f else 50f)
        chorusDepth.value        = loadSlider("chorusDepth", 0f)
        chorusDetune.value       = loadSlider("chorusDetune", 33f)

        // FrameEx sliders
        creakiness.value         = loadSlider("creakiness", 0f)
        breathiness.value        = loadSlider("breathiness", 0f)
        jitter.value             = loadSlider("jitter", 0f)
        shimmer.value            = loadSlider("shimmer", 0f)
        glottalSharpness.value   = loadSlider("glottalSharpness", 50f)

        // Pitch settings
        pitchMode.value          = loadString("pitchMode", "espeak_style")
        inflectionScale.value    = loadSlider("inflectionScale", 58f)
        inflection.value         = loadSlider("inflection", 50f)
    }

    // ── Voice quality: slider change handlers ───────────────────────

    fun onVoiceTiltChanged(v: Float)       { voiceTilt.value = v;       saveSlider("voiceTilt", v);       applyVoicingTone() }
    fun onNoiseGlottalModChanged(v: Float) { noiseGlottalMod.value = v; saveSlider("noiseGlottalMod", v); applyVoicingTone() }
    fun onPitchSyncF1Changed(v: Float)     { pitchSyncF1.value = v;     saveSlider("pitchSyncF1", v);     applyVoicingTone() }
    fun onPitchSyncB1Changed(v: Float)     { pitchSyncB1.value = v;     saveSlider("pitchSyncB1", v);     applyVoicingTone() }
    fun onSpeedQuotientChanged(v: Float)   { speedQuotient.value = v;   saveSlider("speedQuotient", v);   applyVoicingTone() }
    fun onAspirationTiltChanged(v: Float)  { aspirationTilt.value = v;  saveSlider("aspirationTilt", v);  applyVoicingTone() }
    fun onCascadeBwScaleChanged(v: Float)  { cascadeBwScale.value = v;  saveSlider("cascadeBwScale", v);  applyVoicingTone() }
    fun onVoiceTremorChanged(v: Float)     { voiceTremor.value = v;     saveSlider("voiceTremor", v);     applyVoicingTone() }
    fun onHeadSizeChanged(v: Float)       { headSize.value = v;       saveSlider("headSize", v);       applyVoicingTone() }
    fun onChorusDepthChanged(v: Float)    { chorusDepth.value = v;    saveSlider("chorusDepth", v);    applyVoicingTone() }
    fun onChorusDetuneChanged(v: Float)   { chorusDetune.value = v;   saveSlider("chorusDetune", v);   applyVoicingTone() }

    fun onCreakinessChanged(v: Float)      { creakiness.value = v;      saveSlider("creakiness", v);      applyFrameExDefaults() }
    fun onBreathinessChanged(v: Float)     { breathiness.value = v;     saveSlider("breathiness", v);     applyFrameExDefaults() }
    fun onJitterChanged(v: Float)          { jitter.value = v;          saveSlider("jitter", v);          applyFrameExDefaults() }
    fun onShimmerChanged(v: Float)         { shimmer.value = v;         saveSlider("shimmer", v);         applyFrameExDefaults() }
    fun onGlottalSharpnessChanged(v: Float){ glottalSharpness.value = v; saveSlider("glottalSharpness", v); applyFrameExDefaults() }

    fun onPitchModeChanged(mode: String)       { pitchMode.value = mode;       saveString("pitchMode", mode);       applyPitchSettings() }
    fun onInflectionScaleChanged(v: Float)     { inflectionScale.value = v;    saveSlider("inflectionScale", v);    applyPitchSettings() }
    fun onInflectionChanged(v: Float)          { inflection.value = v;         saveSlider("inflection", v);         applyPitchSettings() }
    fun onOverrideSystemRateChanged(v: Boolean){ overrideSystemRate.value = v;  saveGlobalBool("overrideSystemRate", v) }
    fun onGlobalRateChanged(v: Float)          { globalRate.value = v;          saveGlobalFloat("globalRate", v) }
    fun onRateBoostEnabledChanged(v: Boolean)  { rateBoostEnabled.value = v;    saveGlobalBool("rateBoostEnabled", v) }
    fun onSystemVolumeChanged(v: Float)        { systemVolume.value = v;        saveGlobalFloat("systemVolume", v);  engine.setVolume(v) }
    fun onSampleRateChanged(index: Float) {
        sampleRateIndex.value = index
        val rate = SAMPLE_RATES[index.roundToInt().coerceIn(0, SAMPLE_RATES.size - 1)]
        saveGlobalInt("sampleRate", rate)
        engine.setSampleRate(rate)
    }
    fun onLockLanguageChanged(enabled: Boolean) {
        lockLanguage.value = enabled
        saveGlobalBool("lockLanguage", enabled)
    }
    fun onPauseModeChanged(mode: Int) {
        pauseMode.value = mode
        saveGlobalInt("pauseMode", mode)
        engine.setPauseMode(mode)
    }

    // ── Reset to defaults ──────────────────────────────────────────

    fun resetToDefaults(allVoices: Boolean = false) {
        if (allVoices) {
            // Reset per-voice prefs for ALL voices directly in SharedPreferences
            val ed = prefs.edit()
            for (voice in voices) {
                val v = voice.id
                ed.putFloat("${PREF_PREFIX}voiceTilt.$v", 50f)
                ed.putFloat("${PREF_PREFIX}speedQuotient.$v", 50f)
                ed.putFloat("${PREF_PREFIX}aspirationTilt.$v", 50f)
                ed.putFloat("${PREF_PREFIX}cascadeBwScale.$v", 50f)
                ed.putFloat("${PREF_PREFIX}noiseGlottalMod.$v", 0f)
                ed.putFloat("${PREF_PREFIX}pitchSyncF1.$v", 50f)
                ed.putFloat("${PREF_PREFIX}pitchSyncB1.$v", 50f)
                ed.putFloat("${PREF_PREFIX}voiceTremor.$v", 0f)
                ed.putFloat("${PREF_PREFIX}headSize.$v", 50f)
                ed.putFloat("${PREF_PREFIX}chorusDepth.$v", 0f)
                ed.putFloat("${PREF_PREFIX}chorusDetune.$v", 33f)
                ed.putFloat("${PREF_PREFIX}creakiness.$v", 0f)
                ed.putFloat("${PREF_PREFIX}breathiness.$v", 0f)
                ed.putFloat("${PREF_PREFIX}jitter.$v", 0f)
                ed.putFloat("${PREF_PREFIX}shimmer.$v", 0f)
                ed.putFloat("${PREF_PREFIX}glottalSharpness.$v", 50f)
                ed.putString("${PREF_PREFIX}pitchMode.$v", "espeak_style")
                ed.putFloat("${PREF_PREFIX}inflectionScale.$v", 58f)
                ed.putFloat("${PREF_PREFIX}inflection.$v", 50f)
            }
            ed.apply()
            // Reload current voice's UI state from the just-written defaults
            loadSettingsForVoice()
            applyVoicingTone()
            applyFrameExDefaults()
            applyPitchSettings()
        } else {
            // Per-voice: VoicingTone sliders (saved under current voice)
            onVoiceTiltChanged(50f)
            onSpeedQuotientChanged(50f)
            onAspirationTiltChanged(50f)
            onCascadeBwScaleChanged(50f)
            onNoiseGlottalModChanged(0f)
            onPitchSyncF1Changed(50f)
            onPitchSyncB1Changed(50f)
            onVoiceTremorChanged(0f)
            onHeadSizeChanged(50f)
            onChorusDepthChanged(0f)
            onChorusDetuneChanged(33f)

            // Per-voice: FrameEx sliders
            onCreakinessChanged(0f)
            onBreathinessChanged(0f)
            onJitterChanged(0f)
            onShimmerChanged(0f)
            onGlottalSharpnessChanged(50f)

            // Per-voice: Pitch
            onPitchModeChanged("espeak_style")
            onInflectionScaleChanged(58f)
            onInflectionChanged(50f)
        }

        // Global: System rate
        onOverrideSystemRateChanged(false)
        onRateBoostEnabledChanged(false)
        globalRate.value = 1.0f;     onGlobalRateChanged(1.0f)

        // Global: Output
        onLockLanguageChanged(false)
        onPauseModeChanged(1)  // short
        onSampleRateChanged(2f)  // 22050 Hz
        onSystemVolumeChanged(1.0f)

        // Global: Reset language filter — all checked
        val allKeys = allLocaleEntries.map { it.first }.toSet()
        _enabledLocaleKeys.value = allKeys
        prefs.edit().remove(TgsbTtsService.PREF_SUPPORTED_LANGUAGES).apply()
    }

    // ── Slider → engine value mapping (matches NVDA driver math) ────

    private fun applyVoicingTone() {
        val tilt = (voiceTilt.value - 50f) * (24f / 50f)          // -24..+24 dB/oct
        val noiseMod = noiseGlottalMod.value / 100f               // 0..1
        val psF1 = (pitchSyncF1.value - 50f) * 1.2f               // -60..+60 Hz
        val psB1 = (pitchSyncB1.value - 50f) * 1.0f               // -50..+50 Hz

        val sqSlider = speedQuotient.value
        val sq = if (sqSlider <= 50f)
            0.5 + (sqSlider / 50.0) * 1.5                         // 0.5..2.0
        else
            2.0 + ((sqSlider - 50.0) / 50.0) * 2.0                // 2.0..4.0

        val aspTilt = (aspirationTilt.value - 50f) * 0.24f         // -12..+12 dB/oct

        val bwSlider = cascadeBwScale.value
        val bw = if (bwSlider <= 50f)
            2.0 - (bwSlider / 50.0) * 1.0                         // 2.0..1.0
        else
            1.0 - ((bwSlider - 50.0) / 50.0) * 0.7                // 1.0..0.3

        val tremor = (voiceTremor.value / 100f) * 0.4f             // 0..0.4

        val hsSlider = headSize.value.toDouble()
        val hs = if (hsSlider <= 50.0)
            1.25 - (hsSlider / 50.0) * 0.25                        // 1.25..1.0
        else
            1.0 - ((hsSlider - 50.0) / 50.0) * 0.15                // 1.0..0.85

        val cd = chorusDepth.value / 100.0                          // 0..1.0
        val cdt = 0.5 + (chorusDetune.value / 100.0) * 4.5         // 0.5..5.0 Hz

        engine.setVoicingTone(
            tilt.toDouble(), noiseMod.toDouble(),
            psF1.toDouble(), psB1.toDouble(),
            sq, aspTilt.toDouble(), bw, tremor.toDouble(), hs,
            cd, cdt
        )
    }

    private fun applyFrameExDefaults() {
        engine.setFrameExDefaults(
            (creakiness.value / 100f).toDouble(),
            (breathiness.value / 100f).toDouble(),
            (jitter.value / 100f).toDouble(),
            (shimmer.value / 100f).toDouble(),
            (glottalSharpness.value / 50f).toDouble()   // 0..2.0, 50=1.0
        )
    }

    private fun applyPitchSettings() {
        engine.setPitchMode(pitchMode.value)
        engine.setInflectionScale((inflectionScale.value / 100f).toDouble())
        engine.setInflection((inflection.value / 100f).toDouble())
    }

    // ── Language filter ─────────────────────────────────────────────

    fun toggleLocaleKey(localeKey: String, enabled: Boolean): Boolean {
        val current = _enabledLocaleKeys.value.toMutableSet()
        if (enabled) {
            current.add(localeKey)
        } else {
            if (current.size <= 1) return false
            current.remove(localeKey)
        }
        _enabledLocaleKeys.value = current.toSet()
        saveEnabledKeys(current)
        return true
    }

    // ── Helpers ─────────────────────────────────────────────────────

    /** Current voice ID for per-voice keying (e.g. "adam", "benjamin"). */
    private val currentVoiceId: String
        get() = TgsbTtsService.VOICES.getOrNull(selectedVoiceIndex.value)?.id ?: "adam"

    /**
     * Load a per-voice slider value.
     * Tries "adv_key.voiceName" first, falls back to "adv_key" (migration).
     */
    private fun loadSlider(key: String, default: Float): Float {
        val voiceKey = "${PREF_PREFIX}$key.${currentVoiceId}"
        if (prefs.contains(voiceKey)) return prefs.getFloat(voiceKey, default)
        // Fallback: old global key (pre-per-voice migration)
        return prefs.getFloat("${PREF_PREFIX}$key", default)
    }

    private fun saveSlider(key: String, value: Float) {
        prefs.edit().putFloat("${PREF_PREFIX}$key.${currentVoiceId}", value).apply()
    }

    /**
     * Load a per-voice string value.
     * Tries "adv_key.voiceName" first, falls back to "adv_key" (migration).
     */
    private fun loadString(key: String, default: String): String {
        val voiceKey = "${PREF_PREFIX}$key.${currentVoiceId}"
        if (prefs.contains(voiceKey)) return prefs.getString(voiceKey, default) ?: default
        return prefs.getString("${PREF_PREFIX}$key", default) ?: default
    }

    private fun saveString(key: String, value: String) {
        prefs.edit().putString("${PREF_PREFIX}$key.${currentVoiceId}", value).apply()
    }

    /**
     * Load a per-voice boolean.
     * Tries "adv_key.voiceName" first, falls back to "adv_key" (migration).
     */
    private fun loadBool(key: String, default: Boolean): Boolean {
        val voiceKey = "${PREF_PREFIX}$key.${currentVoiceId}"
        if (prefs.contains(voiceKey)) return prefs.getBoolean(voiceKey, default)
        return prefs.getBoolean("${PREF_PREFIX}$key", default)
    }

    private fun saveBool(key: String, value: Boolean) {
        prefs.edit().putBoolean("${PREF_PREFIX}$key.${currentVoiceId}", value).apply()
    }

    /**
     * Load a per-voice int.
     * Tries "adv_key.voiceName" first, falls back to "adv_key" (migration).
     */
    private fun loadInt(key: String, default: Int): Int {
        val voiceKey = "${PREF_PREFIX}$key.${currentVoiceId}"
        if (prefs.contains(voiceKey)) return prefs.getInt(voiceKey, default)
        return prefs.getInt("${PREF_PREFIX}$key", default)
    }

    private fun saveInt(key: String, value: Int) {
        prefs.edit().putInt("${PREF_PREFIX}$key.${currentVoiceId}", value).apply()
    }

    // ── Global (NOT per-voice) preference helpers ─────────────────────

    private fun loadGlobalFloat(key: String, default: Float): Float =
        prefs.getFloat("${PREF_PREFIX}$key", default)

    private fun saveGlobalFloat(key: String, value: Float) {
        prefs.edit().putFloat("${PREF_PREFIX}$key", value).apply()
    }

    private fun loadGlobalBool(key: String, default: Boolean): Boolean =
        prefs.getBoolean("${PREF_PREFIX}$key", default)

    private fun saveGlobalBool(key: String, value: Boolean) {
        prefs.edit().putBoolean("${PREF_PREFIX}$key", value).apply()
    }

    private fun loadGlobalInt(key: String, default: Int): Int =
        prefs.getInt("${PREF_PREFIX}$key", default)

    private fun saveGlobalInt(key: String, value: Int) {
        prefs.edit().putInt("${PREF_PREFIX}$key", value).apply()
    }

    private fun buildLanguageList(): List<LanguageItem> {
        val enabledKeys = TgsbTtsService.getEnabledLocaleKeys(prefs)
        val seen = mutableSetOf<String>()
        val items = mutableListOf<LanguageItem>()
        for (ld in TgsbTtsService.LANGUAGES) {
            if (!TgsbTtsService.isLangEnabled(ld, enabledKeys)) continue
            val key = ld.displayLocale.toString()
            if (key in seen) continue
            seen.add(key)
            items.add(LanguageItem(ld.displayLocale.getDisplayName(), ld))
        }
        items.sortBy { it.displayName }
        return items
    }

    private fun buildLocaleEntries(): List<Pair<String, String>> {
        val seen = mutableSetOf<String>()
        val entries = mutableListOf<Pair<String, String>>()
        for (ld in TgsbTtsService.LANGUAGES) {
            val key = ld.displayLocale.toString()
            if (key in seen) continue
            seen.add(key)
            entries.add(key to ld.displayLocale.getDisplayName())
        }
        entries.sortBy { it.second }
        return entries
    }

    private fun loadEnabledKeys(): Set<String> {
        val saved = TgsbTtsService.getEnabledLocaleKeys(prefs)
        if (saved != null) return saved
        return buildLocaleEntries().map { it.first }.toSet()
    }

    private fun saveEnabledKeys(keys: Set<String>) {
        if (keys.size >= allLocaleEntries.size) {
            prefs.edit().remove(TgsbTtsService.PREF_SUPPORTED_LANGUAGES).apply()
        } else {
            prefs.edit()
                .putStringSet(TgsbTtsService.PREF_SUPPORTED_LANGUAGES, keys)
                .apply()
        }
    }

    // ── Pack settings editor ─────────────────────────────────────────

    enum class SettingType { Bool, Number, Text }

    data class PackSetting(
        val key: String,
        val displayName: String,
        val value: String,
        val isOverridden: Boolean,
        val type: SettingType,
        val options: List<String>? = null  // dropdown options for enum-like strings
    )

    val editorLanguages = MutableStateFlow<List<String>>(emptyList())
    val editorSettings = MutableStateFlow<List<PackSetting>>(emptyList())
    private var editorLangTag: String = ""

    fun loadEditorLanguages() {
        editorLanguages.value = engine.getAvailableLanguages()
    }

    fun loadEditorSettings(langTag: String) {
        editorLangTag = langTag

        // Query settings directly by langTag — no temp language switch needed.
        val jsonStr = engine.queryData(TgsbSpeakEngine.DATA_SETTINGS, langTag) ?: return
        val overrides = loadOverrides(langTag)

        // Settings managed by Engine Settings sliders — hide from editor.
        val hiddenKeys = setOf("legacyPitchMode", "legacyPitchInflectionScale")

        val settings = mutableListOf<PackSetting>()
        val arr = org.json.JSONArray(jsonStr)
        for (i in 0 until arr.length()) {
            val obj = arr.getJSONObject(i)
            val key = obj.getString("key")
            if (key in hiddenKeys) continue
            val baseValue = obj.get("value").toString()
            val effectiveValue = overrides[key] ?: baseValue
            val jsonType = obj.getString("type")
            val type = when (jsonType) {
                "bool" -> SettingType.Bool
                "float" -> SettingType.Number
                else -> SettingType.Text
            }
            val options = if (obj.has("options")) {
                val optArr = obj.getJSONArray("options")
                (0 until optArr.length()).map { optArr.getString(it) }
            } else null
            settings.add(PackSetting(
                key = key,
                displayName = camelToDisplay(key),
                value = effectiveValue,
                isOverridden = overrides.containsKey(key),
                type = type,
                options = options
            ))
        }
        editorSettings.value = settings

        // Apply overrides to the active language if it matches.
        if (overrides.isNotEmpty()) {
            for ((k, v) in overrides) {
                engine.setData(TgsbSpeakEngine.DATA_SETTINGS, langTag, k, v)
            }
        }
    }

    fun setEditorOverride(langTag: String, key: String, value: String) {
        // If the new value matches the pack base, remove the override instead.
        val baseValues = getBaseValues(langTag)
        val overrides = loadOverrides(langTag).toMutableMap()
        if (baseValues[key] == value) {
            overrides.remove(key)
        } else {
            overrides[key] = value
        }
        saveOverrides(langTag, overrides)
        reloadCurrentLanguage()
        loadEditorSettings(langTag)
    }

    /** Read base pack values (no overrides) for comparison. */
    private fun getBaseValues(langTag: String): Map<String, String> {
        val jsonStr = engine.queryData(TgsbSpeakEngine.DATA_SETTINGS, langTag) ?: return emptyMap()
        val map = mutableMapOf<String, String>()
        val arr = org.json.JSONArray(jsonStr)
        for (i in 0 until arr.length()) {
            val obj = arr.getJSONObject(i)
            map[obj.getString("key")] = obj.get("value").toString()
        }
        return map
    }

    fun removeEditorOverride(langTag: String, key: String) {
        val overrides = loadOverrides(langTag).toMutableMap()
        overrides.remove(key)
        saveOverrides(langTag, overrides)
        reloadCurrentLanguage()
        loadEditorSettings(langTag)
    }

    fun resetAllEditorOverrides(langTag: String) {
        val ver = prefs.getInt("pack_overrides_version", 0) + 1
        prefs.edit()
            .remove("pack_overrides_$langTag")
            .putInt("pack_overrides_version", ver)
            .apply()
        reloadCurrentLanguage()
        loadEditorSettings(langTag)
    }

    /** Reload the current language from disk so removed overrides take effect. */
    private fun reloadCurrentLanguage() {
        val curLang = languages.getOrNull(selectedLanguageIndex.value) ?: return
        engine.setLanguage(curLang.langDef.espeakLang, curLang.langDef.tgsbLang)
        applyStoredOverrides(curLang.langDef.tgsbLang)
        reapplyDictOverrides(curLang.langDef.tgsbLang)
    }

    /** Apply stored overrides after setLanguage via per-key setData. */
    fun applyStoredOverrides(tgsbLang: String) {
        val overrides = loadOverrides(tgsbLang)
        if (overrides.isEmpty()) return
        for ((k, v) in overrides) {
            engine.setData(TgsbSpeakEngine.DATA_SETTINGS, tgsbLang, k, v)
        }
    }

    private fun loadOverrides(langTag: String): Map<String, String> {
        val json = prefs.getString("pack_overrides_$langTag", null) ?: return emptyMap()
        return try {
            val obj = org.json.JSONObject(json)
            obj.keys().asSequence().associateWith { obj.getString(it) }
        } catch (e: Exception) { emptyMap() }
    }

    private fun saveOverrides(langTag: String, overrides: Map<String, String>) {
        val e = prefs.edit()
        if (overrides.isEmpty()) {
            e.remove("pack_overrides_$langTag")
        } else {
            val obj = org.json.JSONObject()
            for ((k, v) in overrides) obj.put(k, v)
            e.putString("pack_overrides_$langTag", obj.toString())
        }
        // Bump version so the TTS Service knows to reload the pack.
        val ver = prefs.getInt("pack_overrides_version", 0) + 1
        e.putInt("pack_overrides_version", ver)
        e.apply()
    }

    private fun detectType(value: String): SettingType = when {
        value == "true" || value == "false" -> SettingType.Bool
        value.toDoubleOrNull() != null -> SettingType.Number
        else -> SettingType.Text
    }

    private fun camelToDisplay(key: String): String {
        // "boundarySmoothing.enabled" → "Boundary Smoothing: Enabled"
        // "yearSplittingEnabled" → "Year Splitting Enabled"
        return key.replace(".", ": ").fold(StringBuilder()) { sb, c ->
            if (c.isUpperCase() && sb.isNotEmpty() && sb.last().isLowerCase()) {
                sb.append(' ')
            }
            sb.append(c)
        }.toString().replaceFirstChar { it.uppercase() }
    }

    // ── Dictionary editor ─────────────────────────────────────────────

    data class DictType(
        val type: String,    // "compound", "pronounce", "stress"
        val count: Int
    )

    data class DictEntry(
        val fromText: String,
        val toText: String,
        val fromIpa: String = "",
        val toIpa: String = "",
        val category: String = "",
        val source: String = "main",  // "main" or "user"
        val masked: Boolean = false
    )

    val dictTypes = MutableStateFlow<List<DictType>>(emptyList())
    val dictionaryEntries = MutableStateFlow<List<DictEntry>>(emptyList())
    val dictionaryTotalCount = MutableStateFlow(0)
    val dictionaryCategories = MutableStateFlow<List<String>>(emptyList())
    private var dictLangTag: String = ""
    private var dictSubType: String = ""

    /** Returns the current engine language tag (e.g. "en-us"). */
    fun currentEngineLangTag(): String {
        val idx = selectedLanguageIndex.value.coerceIn(0, languages.size - 1)
        return languages[idx].langDef.tgsbLang
    }

    fun loadDictTypes(langTag: String = "") {
        val tag = if (langTag.isNotEmpty()) "types:$langTag" else "types"
        val jsonStr = engine.queryData(TgsbSpeakEngine.DATA_DICTIONARY, tag, 0, 0) ?: run {
            dictTypes.value = emptyList()
            return
        }
        try {
            val arr = org.json.JSONArray(jsonStr)
            val types = mutableListOf<DictType>()
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                types.add(DictType(
                    type = obj.optString("type", ""),
                    count = obj.optInt("count", 0)
                ))
            }
            dictTypes.value = types
        } catch (e: Exception) {
            Log.e(TAG, "Failed to parse dict types: ${e.message}")
            dictTypes.value = emptyList()
        }
    }

    /**
     * Build the prefixed langTag for the data query API.
     * "pronounce" type uses bare langTag, others use "type:langTag".
     */
    private fun prefixedLangTag(subType: String, langTag: String): String {
        return if (subType.isEmpty() || subType == "pronounce") langTag
        else "$subType:$langTag"
    }

    fun loadDictionary(langTag: String, subType: String = "", offset: Int = 0, limit: Int = 100, append: Boolean = false, search: String = "") {
        // Cross-language browsing: the C++ backend loads dict files from disk
        // for non-current languages via getDictRefs(), no setLanguage needed.
        dictLangTag = langTag
        dictSubType = subType
        val prefixed = prefixedLangTag(subType, langTag) +
            if (search.isNotEmpty()) "?$search" else ""
        dictionaryTotalCount.value = engine.getDataCount(TgsbSpeakEngine.DATA_DICTIONARY, prefixed)
        val jsonStr = engine.queryData(TgsbSpeakEngine.DATA_DICTIONARY, prefixed, offset, limit) ?: run {
            if (!append) dictionaryEntries.value = emptyList()
            return
        }
        val arr = org.json.JSONArray(jsonStr)
        val entries = mutableListOf<DictEntry>()
        val cats = mutableSetOf<String>()
        for (i in 0 until arr.length()) {
            val obj = arr.getJSONObject(i)
            val entry = DictEntry(
                fromText = obj.optString("fromText", obj.optString("key", "")),
                toText = obj.optString("toText", ""),
                fromIpa = obj.optString("fromIpa", ""),
                toIpa = obj.optString("toIpa", ""),
                category = obj.optString("category", ""),
                source = obj.optString("source", "main"),
                masked = obj.optBoolean("masked", false)
            )
            entries.add(entry)
            if (entry.category.isNotEmpty()) cats.add(entry.category)
        }
        if (append) {
            dictionaryEntries.value = dictionaryEntries.value + entries
        } else {
            dictionaryEntries.value = entries
        }
        dictionaryCategories.value = cats.sorted()
    }

    fun addDictEntry(
        fromText: String, toText: String, category: String = "",
        fromIpa: String = "", toIpa: String = ""
    ) {
        if (fromText.isBlank() || toText.isBlank()) return
        val prefixed = prefixedLangTag(dictSubType, dictLangTag)
        val json = org.json.JSONObject().apply {
            put("toText", toText)
            if (category.isNotEmpty()) put("category", category)
            if (fromIpa.isNotEmpty()) put("fromIpa", fromIpa)
            if (toIpa.isNotEmpty()) put("toIpa", toIpa)
        }
        engine.setData(TgsbSpeakEngine.DATA_DICTIONARY, prefixed, fromText, json.toString())
        saveDictOverride(prefixed, fromText, json.toString())
        loadDictionary(dictLangTag, dictSubType)
    }

    fun maskDictCategory(category: String, masked: Boolean, entries: List<DictEntry>) {
        val prefixed = prefixedLangTag(dictSubType, dictLangTag)
        val matching = entries.filter {
            it.category.equals(category, ignoreCase = true) && it.masked != masked
        }
        for (e in matching) {
            val json = org.json.JSONObject().apply { put("masked", masked) }
            engine.setData(TgsbSpeakEngine.DATA_DICTIONARY, prefixed, e.fromText, json.toString())
            saveDictOverride(prefixed, e.fromText, json.toString())
        }
        if (matching.isNotEmpty()) loadDictionary(dictLangTag, dictSubType)
    }

    fun textToIpa(text: String): String = engine.textToIpa(text)

    fun getPhonemeKeys(): List<Pair<String, String>> {
        val json = engine.queryData(TgsbSpeakEngine.DATA_PHONEMES, dictLangTag) ?: return emptyList()
        return try {
            val arr = org.json.JSONArray(json)
            val seen = linkedSetOf<String>()
            val result = mutableListOf<Pair<String, String>>()
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                val group = obj.optString("group", "")
                if (group.isNotEmpty() && seen.add(group)) {
                    result.add(group to obj.optString("class", ""))
                }
            }
            result
        } catch (_: Exception) { emptyList() }
    }

    fun maskDictEntry(fromText: String, masked: Boolean) {
        val prefixed = prefixedLangTag(dictSubType, dictLangTag)
        val json = org.json.JSONObject().apply {
            put("masked", masked)
        }
        engine.setData(TgsbSpeakEngine.DATA_DICTIONARY, prefixed, fromText, json.toString())
        saveDictOverride(prefixed, fromText, json.toString())
        loadDictionary(dictLangTag, dictSubType)
    }

    fun deleteDictEntry(fromText: String) {
        val prefixed = prefixedLangTag(dictSubType, dictLangTag)
        removeDictOverride(prefixed, fromText)
        // Reload language to restore base dict entries from disk,
        // then reapply remaining user overrides. This ensures that
        // removing a modified base entry reverts to the original.
        engine.setLanguage(dictLangTag, dictLangTag)
        reapplyDictOverrides(dictLangTag)
        loadDictionary(dictLangTag, dictSubType)
    }

    private fun loadDictOverrides(langTag: String): Map<String, String> {
        val json = prefs.getString("dict_overrides_$langTag", null) ?: return emptyMap()
        return try {
            val obj = org.json.JSONObject(json)
            obj.keys().asSequence().associateWith { obj.getString(it) }
        } catch (e: Exception) { emptyMap() }
    }

    private fun saveDictOverride(langTag: String, key: String, value: String) {
        val overrides = loadDictOverrides(langTag).toMutableMap()
        overrides[key] = value
        val obj = org.json.JSONObject()
        for ((k, v) in overrides) obj.put(k, v)
        val ver = prefs.getInt("pack_overrides_version", 0) + 1
        prefs.edit()
            .putString("dict_overrides_$langTag", obj.toString())
            .putInt("pack_overrides_version", ver)
            .apply()
    }

    private fun removeDictOverride(langTag: String, key: String) {
        val overrides = loadDictOverrides(langTag).toMutableMap()
        overrides.remove(key)
        val ver = prefs.getInt("pack_overrides_version", 0) + 1
        val e = prefs.edit()
        if (overrides.isEmpty()) {
            e.remove("dict_overrides_$langTag")
        } else {
            val obj = org.json.JSONObject()
            for ((k, v) in overrides) obj.put(k, v)
            e.putString("dict_overrides_$langTag", obj.toString())
        }
        e.putInt("pack_overrides_version", ver).apply()
    }

    fun reapplyDictOverrides(langTag: String) {
        val overrides = loadDictOverrides(langTag)
        for ((k, v) in overrides) {
            engine.setData(TgsbSpeakEngine.DATA_DICTIONARY, langTag, k, v)
        }
        // Re-apply dict type disabled state
        val disabled = loadDictDisabled(langTag)
        for (type in disabled) {
            engine.setData(TgsbSpeakEngine.DATA_DICTIONARY, "config:$langTag", type, "false")
        }
    }

    // ── Dict type exclusion ───────────────────────────────────────────

    fun loadDictConfig(langTag: String): Map<String, Boolean> {
        val jsonStr = engine.queryData(TgsbSpeakEngine.DATA_DICTIONARY, "config:$langTag") ?: return emptyMap()
        return try {
            val arr = org.json.JSONArray(jsonStr)
            val result = mutableMapOf<String, Boolean>()
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                result[obj.getString("type")] = obj.getBoolean("enabled")
            }
            result
        } catch (e: Exception) { emptyMap() }
    }

    fun setDictTypeEnabled(langTag: String, type: String, enabled: Boolean) {
        engine.setData(TgsbSpeakEngine.DATA_DICTIONARY, "config:$langTag", type, if (enabled) "true" else "false")
        saveDictDisabled(langTag, type, !enabled)
    }

    private fun loadDictDisabled(langTag: String): Set<String> {
        val json = prefs.getString("dict_disabled_$langTag", null) ?: return emptySet()
        return try {
            val arr = org.json.JSONArray(json)
            (0 until arr.length()).map { arr.getString(it) }.toSet()
        } catch (e: Exception) { emptySet() }
    }

    private fun saveDictDisabled(langTag: String, type: String, disabled: Boolean) {
        val current = loadDictDisabled(langTag).toMutableSet()
        if (disabled) current.add(type) else current.remove(type)
        if (current.isEmpty()) {
            prefs.edit().remove("dict_disabled_$langTag").apply()
        } else {
            prefs.edit().putString("dict_disabled_$langTag", org.json.JSONArray(current.toList()).toString()).apply()
        }
    }

    // ── Phoneme editor ────────────────────────────────────────────────

    data class PhonemeEntry(
        val key: String,         // IPA key e.g. "ɪ"
        val phonemeClass: String, // "vowel", "stop", etc.
        val mappingFrom: String   // non-empty in lang-filtered view
    )

    data class PhonemeField(
        val key: String,          // full dot key e.g. "ɪ.cf2"
        val fieldName: String,    // just the field part e.g. "cf2"
        val displayName: String,  // human-readable e.g. "F2 Frequency"
        val value: String,
        val isOverridden: Boolean,
        val isUserAdded: Boolean, // true if field only exists in user overrides, not in base phoneme
        val type: SettingType
    )

    /** Human-readable names and sort order for phoneme fields. */
    private val phonemeFieldInfo = linkedMapOf(
        // ── Voicing ──
        "voicePitch" to "Voice Pitch (Hz)",
        "endVoicePitch" to "End Voice Pitch (Hz)",
        "voiceAmplitude" to "Voice Amplitude",
        "aspirationAmplitude" to "Aspiration Amplitude",
        "glottalOpenQuotient" to "Glottal Open Quotient",
        "voiceTurbulenceAmplitude" to "Voice Turbulence",
        "vibratoPitchOffset" to "Vibrato Pitch Offset",
        "vibratoSpeed" to "Vibrato Speed (Hz)",
        // ── Cascade formants ──
        "cf1" to "F1 Frequency (Hz)",
        "cf2" to "F2 Frequency (Hz)",
        "cf3" to "F3 Frequency (Hz)",
        "cf4" to "F4 Frequency (Hz)",
        "cf5" to "F5 Frequency (Hz)",
        "cf6" to "F6 Frequency (Hz)",
        "cb1" to "F1 Bandwidth (Hz)",
        "cb2" to "F2 Bandwidth (Hz)",
        "cb3" to "F3 Bandwidth (Hz)",
        "cb4" to "F4 Bandwidth (Hz)",
        "cb5" to "F5 Bandwidth (Hz)",
        "cb6" to "F6 Bandwidth (Hz)",
        // ── Nasal formants ──
        "cfN0" to "Nasal Zero Frequency",
        "cfNP" to "Nasal Pole Frequency",
        "cbN0" to "Nasal Zero Bandwidth",
        "cbNP" to "Nasal Pole Bandwidth",
        "caNP" to "Nasal Pole Amplitude",
        // ── Frication ──
        "fricationAmplitude" to "Frication Amplitude",
        "preFormantGain" to "Pre-Formant Gain",
        // ── Parallel formants ──
        "pf1" to "Parallel F1 Frequency",
        "pf2" to "Parallel F2 Frequency",
        "pf3" to "Parallel F3 Frequency",
        "pf4" to "Parallel F4 Frequency",
        "pf5" to "Parallel F5 Frequency",
        "pf6" to "Parallel F6 Frequency",
        "pb1" to "Parallel F1 Bandwidth",
        "pb2" to "Parallel F2 Bandwidth",
        "pb3" to "Parallel F3 Bandwidth",
        "pb4" to "Parallel F4 Bandwidth",
        "pb5" to "Parallel F5 Bandwidth",
        "pb6" to "Parallel F6 Bandwidth",
        "pa1" to "Parallel F1 Amplitude",
        "pa2" to "Parallel F2 Amplitude",
        "pa3" to "Parallel F3 Amplitude",
        "pa4" to "Parallel F4 Amplitude",
        "pa5" to "Parallel F5 Amplitude",
        "pa6" to "Parallel F6 Amplitude",
        "parallelBypass" to "Parallel Bypass",
        "outputGain" to "Output Gain",
        // ── Flags ──
        "_isVowel" to "Is Vowel",
        "_isVoiced" to "Is Voiced",
        "_isStop" to "Is Stop",
        "_isNasal" to "Is Nasal",
        "_isLiquid" to "Is Liquid",
        "_isSemivowel" to "Is Semivowel",
        "_isAffricate" to "Is Affricate",
        "_isTap" to "Is Tap",
        "_isTrill" to "Is Trill",
        "_copyAdjacent" to "Copy Adjacent",
        // ── FrameEx ──
        "frameEx.creakiness" to "Creakiness",
        "frameEx.breathiness" to "Breathiness",
        "frameEx.jitter" to "Jitter",
        "frameEx.shimmer" to "Shimmer",
        "frameEx.sharpness" to "Glottal Sharpness",
        "frameEx.endCf1" to "Diphthong End F1",
        "frameEx.endCf2" to "Diphthong End F2",
        "frameEx.endCf3" to "Diphthong End F3",
        "frameEx.endPf1" to "Diphthong End Parallel F1",
        "frameEx.endPf2" to "Diphthong End Parallel F2",
        "frameEx.endPf3" to "Diphthong End Parallel F3",
        "frameEx.fricationTiltDb" to "Frication Tilt (dB, neg=darker)",
        // ── Micro-events ──
        "burstDurationMs" to "Burst Duration (ms)",
        "burstDecayRate" to "Burst Decay Rate",
        "burstSpectralTilt" to "Burst Spectral Tilt",
        "voiceBarAmplitude" to "Voice Bar Amplitude",
        "voiceBarF1" to "Voice Bar F1 (Hz)",
        "releaseSpreadMs" to "Release Spread (ms)",
        "fricAttackMs" to "Frication Attack (ms)",
        "fricDecayMs" to "Frication Decay (ms)",
        "durationScale" to "Duration Scale",
        "closureGapMs" to "Closure Gap (ms, stops)",
    )

    /** Sort order: fields in phonemeFieldInfo map order, unknowns at end. */
    private val phonemeFieldOrder: Map<String, Int> by lazy {
        phonemeFieldInfo.keys.withIndex().associate { (i, k) -> k to i }
    }

    private fun phonemeDisplayName(fieldName: String): String =
        phonemeFieldInfo[fieldName] ?: camelToDisplay(fieldName)

    val phonemeList = MutableStateFlow<List<PhonemeEntry>>(emptyList())
    val phonemeFields = MutableStateFlow<List<PhonemeField>>(emptyList())
    private var phonemeLangFilter: String = ""

    fun loadPhonemeList(langTag: String = "") {
        phonemeLangFilter = langTag
        val jsonStr = engine.queryData(TgsbSpeakEngine.DATA_PHONEMES, langTag)
            ?: return
        val arr = org.json.JSONArray(jsonStr)
        val seen = mutableMapOf<String, PhonemeEntry>()
        for (i in 0 until arr.length()) {
            val obj = arr.getJSONObject(i)
            val group = obj.getString("group")
            if (group !in seen) {
                seen[group] = PhonemeEntry(
                    key = group,
                    phonemeClass = obj.optString("class", "other"),
                    mappingFrom = obj.optString("mappingFrom", "")
                )
            }
        }
        phonemeList.value = seen.values.toList()
    }

    fun loadPhonemeFields(phonemeKey: String) {
        // Query all phonemes (always from base) and filter to this phoneme.
        val jsonStr = engine.queryData(TgsbSpeakEngine.DATA_PHONEMES, "")
            ?: return
        val overrides = loadPhonemeOverrides()
        val arr = org.json.JSONArray(jsonStr)
        val fields = mutableListOf<PhonemeField>()
        val baseKeys = mutableSetOf<String>()
        for (i in 0 until arr.length()) {
            val obj = arr.getJSONObject(i)
            if (obj.getString("group") != phonemeKey) continue
            val fullKey = obj.getString("key")
            baseKeys.add(fullKey)
            val fieldName = fullKey.removePrefix("$phonemeKey.")
            val baseValue = obj.get("value").toString()
            val effectiveValue = overrides[fullKey] ?: baseValue
            val jsonType = obj.getString("type")
            val type = when (jsonType) {
                "bool" -> SettingType.Bool
                "float" -> SettingType.Number
                else -> SettingType.Text
            }
            fields.add(PhonemeField(
                key = fullKey,
                fieldName = fieldName,
                displayName = phonemeDisplayName(fieldName),
                value = effectiveValue,
                isOverridden = overrides.containsKey(fullKey),
                isUserAdded = false,
                type = type
            ))
        }
        // Append user-added fields (overrides that don't exist in the base phoneme).
        val prefix = "$phonemeKey."
        for ((fullKey, value) in overrides) {
            if (!fullKey.startsWith(prefix)) continue
            if (fullKey in baseKeys) continue
            val fieldName = fullKey.removePrefix(prefix)
            // Determine type from field name, not stored value.
            val isBool = fieldName.startsWith("_is") || fieldName.startsWith("_copy")
            fields.add(PhonemeField(
                key = fullKey,
                fieldName = fieldName,
                displayName = phonemeDisplayName(fieldName),
                value = value,
                isOverridden = true,
                isUserAdded = true,
                type = if (isBool) SettingType.Bool else SettingType.Number
            ))
        }
        val maxOrder = phonemeFieldOrder.size
        phonemeFields.value = fields.sortedBy { phonemeFieldOrder[it.fieldName] ?: maxOrder }
    }

    /** Returns fields from phonemeFieldInfo that are not currently in the phoneme's field list. */
    fun getAvailableFieldsToAdd(phonemeKey: String): List<Pair<String, String>> {
        val existingFields = phonemeFields.value.map { it.fieldName }.toSet()
        return phonemeFieldInfo.entries
            .filter { it.key !in existingFields }
            .map { it.key to it.value }
    }

    fun setPhonemeOverride(fullKey: String, value: String) {
        // Apply in-memory immediately for live preview.
        engine.setData(TgsbSpeakEngine.DATA_PHONEMES, "", fullKey, value)

        // Persist to SharedPreferences.
        val overrides = loadPhonemeOverrides().toMutableMap()
        overrides[fullKey] = value
        savePhonemeOverrides(overrides)
    }

    fun removePhonemeOverride(fullKey: String) {
        val overrides = loadPhonemeOverrides().toMutableMap()
        overrides.remove(fullKey)
        savePhonemeOverrides(overrides)
        // Reload language to get base value back.
        reloadCurrentLanguage()
        reapplyAllPhonemeOverrides()
    }

    /** Remove all overrides for a single phoneme (keys starting with "phonemeKey."). */
    fun resetPhonemeOverrides(phonemeKey: String) {
        val prefix = "$phonemeKey."
        val overrides = loadPhonemeOverrides().toMutableMap()
        overrides.keys.removeAll { it.startsWith(prefix) }
        savePhonemeOverrides(overrides)
        reloadCurrentLanguage()
        reapplyAllPhonemeOverrides()
    }

    /** Remove all phoneme overrides, optionally filtered to a specific language's phoneme list. */
    fun resetAllPhonemeOverrides(langFilter: String = "") {
        if (langFilter.isEmpty()) {
            savePhonemeOverrides(emptyMap())
        } else {
            val phonemeKeys = phonemeList.value.map { it.key }.toSet()
            val overrides = loadPhonemeOverrides().toMutableMap()
            overrides.keys.removeAll { key ->
                phonemeKeys.any { key.startsWith("$it.") }
            }
            savePhonemeOverrides(overrides)
        }
        reloadCurrentLanguage()
        reapplyAllPhonemeOverrides()
    }

    fun previewPhoneme(ipa: String) {
        engine.previewPhoneme(ipa)
    }

    private fun loadPhonemeOverrides(): Map<String, String> {
        val json = prefs.getString("phoneme_overrides", null) ?: return emptyMap()
        return try {
            val obj = org.json.JSONObject(json)
            obj.keys().asSequence().associateWith { obj.getString(it) }
        } catch (e: Exception) { emptyMap() }
    }

    private fun savePhonemeOverrides(overrides: Map<String, String>) {
        val e = prefs.edit()
        if (overrides.isEmpty()) {
            e.remove("phoneme_overrides")
        } else {
            val obj = org.json.JSONObject()
            for ((k, v) in overrides) obj.put(k, v)
            e.putString("phoneme_overrides", obj.toString())
        }
        e.apply()
    }

    /** Re-apply all phoneme overrides after a language reload. */
    fun reapplyAllPhonemeOverrides() {
        val overrides = loadPhonemeOverrides()
        for ((k, v) in overrides) {
            engine.setData(TgsbSpeakEngine.DATA_PHONEMES, "", k, v)
        }
    }

    // ── Pack import / export ───────────────────────────────────────────

    private val _importExportStatus = MutableStateFlow<String?>(null)
    val importExportStatus: StateFlow<String?> = _importExportStatus

    fun clearImportExportStatus() { _importExportStatus.value = null }

    private fun packFileForLang(context: Context, langTag: String): File =
        File(context.filesDir, "tgsb/packs/lang/$langTag.yaml")

    private fun packOverridesJson(langTag: String): String {
        val overrides = loadOverrides(langTag)
        if (overrides.isEmpty()) return "{}"
        val obj = org.json.JSONObject()
        for ((k, v) in overrides) obj.put(k, v)
        return obj.toString()
    }

    fun exportPackYaml(context: Context, langTag: String, destUri: Uri) {
        val yaml = engine.exportData(
            TgsbSpeakEngine.DATA_SETTINGS, langTag, packOverridesJson(langTag)
        )
        if (yaml == null) {
            _importExportStatus.value = "Export failed — no pack data found"
            return
        }
        try {
            context.contentResolver.openOutputStream(destUri)?.use { out ->
                out.write(yaml.toByteArray(Charsets.UTF_8))
            }
            _importExportStatus.value = "Exported $langTag.yaml"
        } catch (e: Exception) {
            _importExportStatus.value = "Export failed: ${e.message}"
        }
    }

    fun sharePackYaml(context: Context, langTag: String) {
        val yaml = engine.exportData(
            TgsbSpeakEngine.DATA_SETTINGS, langTag, packOverridesJson(langTag)
        )
        if (yaml == null) {
            _importExportStatus.value = "No pack data to share"
            return
        }
        val intent = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_SUBJECT, "$langTag.yaml")
            putExtra(Intent.EXTRA_TEXT, yaml)
        }
        context.startActivity(Intent.createChooser(intent, "Share $langTag pack"))
    }

    fun importPackYaml(context: Context, langTag: String, sourceUri: Uri): Boolean {
        val content = try {
            context.contentResolver.openInputStream(sourceUri)?.use {
                it.bufferedReader().readText()
            }
        } catch (e: Exception) {
            _importExportStatus.value = "Could not read file: ${e.message}"
            return false
        }
        if (content.isNullOrBlank()) {
            _importExportStatus.value = "File is empty"
            return false
        }

        // Reject phonemes files — they have phoneme-specific keys.
        if (content.contains("_isVowel:") && content.contains("_isNasal:")) {
            _importExportStatus.value = context.getString(R.string.editor_import_not_lang_pack)
            return false
        }

        // Extract settings from the YAML and apply as per-key overrides.
        // Only the settings: block is imported — normalization rules,
        // allophone rules, etc. are skipped (they reference phonemes
        // that may not exist in the user's phoneme table).
        val settings = extractSettingsFromYaml(content)
        if (settings.isEmpty()) {
            _importExportStatus.value = "No settings found in file"
            return false
        }

        // Clear existing overrides and apply imported settings.
        prefs.edit().remove("pack_overrides_$langTag").apply()
        for ((key, value) in settings) {
            engine.setData(TgsbSpeakEngine.DATA_SETTINGS, langTag, key, value)
        }
        saveOverrides(langTag, settings)
        reloadCurrentLanguage()
        loadEditorSettings(langTag)
        _importExportStatus.value = "Imported ${settings.size} settings into $langTag"
        return true
    }

    /**
     * Parse a YAML file and extract the settings: block as flat dot-notation
     * key-value pairs. Handles up to 3 levels of nesting.
     * Ignores normalization, transforms, allophone rules, etc.
     */
    private fun extractSettingsFromYaml(yaml: String): Map<String, String> {
        val result = mutableMapOf<String, String>()
        val lines = yaml.lines()
        var inSettings = false
        val keyStack = mutableListOf<Pair<Int, String>>()  // (indent, key)

        for (line in lines) {
            val trimmed = line.trimEnd()
            if (trimmed.isEmpty() || trimmed.trimStart().startsWith("#")) continue

            val indent = trimmed.length - trimmed.trimStart().length

            // Detect top-level blocks.
            if (indent == 0 && trimmed.contains(":")) {
                val topKey = trimmed.substringBefore(":").trim()
                inSettings = (topKey == "settings")
                keyStack.clear()
                continue
            }

            if (!inSettings) continue

            // Settings line: parse key: value at this indent level.
            val colonPos = trimmed.indexOf(":")
            if (colonPos < 0) continue

            val key = trimmed.substring(indent, colonPos).trim()
                .removeSurrounding("\"")
            val afterColon = trimmed.substring(colonPos + 1).trim()
                .let { v -> // strip inline comments
                    val hashPos = v.indexOf(" #")
                    if (hashPos >= 0) v.substring(0, hashPos).trim() else v
                }

            // Pop keyStack to current indent level.
            while (keyStack.isNotEmpty() && keyStack.last().first >= indent) {
                keyStack.removeAt(keyStack.size - 1)
            }

            if (afterColon.isEmpty()) {
                // This is a parent key (e.g. "boundarySmoothing:") — push to stack.
                keyStack.add(indent to key)
            } else {
                // This is a leaf value — build the dot-notation key.
                val prefix = keyStack.joinToString(".") { it.second }
                val fullKey = if (prefix.isEmpty()) key else "$prefix.$key"
                val value = afterColon.removeSurrounding("\"")
                result[fullKey] = value
            }
        }
        return result
    }

    // ── Phoneme overrides import / export ────────────────────────────

    private fun phonemeOverridesJson(): String {
        val overrides = loadPhonemeOverrides()
        if (overrides.isEmpty()) return "{}"
        val obj = org.json.JSONObject()
        for ((k, v) in overrides) obj.put(k, v)
        return obj.toString()
    }

    fun exportPhonemeYaml(context: Context, destUri: Uri) {
        val yaml = engine.exportData(
            TgsbSpeakEngine.DATA_PHONEMES, "", phonemeOverridesJson()
        )
        if (yaml == null) {
            _importExportStatus.value = "Export failed — no phoneme data found"
            return
        }
        try {
            context.contentResolver.openOutputStream(destUri)?.use { out ->
                out.write(yaml.toByteArray(Charsets.UTF_8))
            }
            _importExportStatus.value = "Exported phonemes.yaml"
        } catch (e: Exception) {
            _importExportStatus.value = "Export failed: ${e.message}"
        }
    }

    fun importPhonemeOverrides(context: Context, sourceUri: Uri) {
        val content = try {
            context.contentResolver.openInputStream(sourceUri)?.use {
                it.bufferedReader().readText()
            }
        } catch (e: Exception) {
            _importExportStatus.value = "Could not read file: ${e.message}"
            return
        }
        if (content.isNullOrBlank()) {
            _importExportStatus.value = "File is empty"
            return
        }
        try {
            val obj = org.json.JSONObject(content)
            val overrides = mutableMapOf<String, String>()
            for (key in obj.keys()) {
                overrides[key] = obj.getString(key)
            }
            savePhonemeOverrides(overrides)
            reloadCurrentLanguage()
            reapplyAllPhonemeOverrides()
            _importExportStatus.value = "Imported ${overrides.size} phoneme overrides"
        } catch (e: Exception) {
            _importExportStatus.value = "Invalid phoneme overrides file: ${e.message}"
        }
    }

    fun sharePhonemeYaml(context: Context) {
        val yaml = engine.exportData(
            TgsbSpeakEngine.DATA_PHONEMES, "", phonemeOverridesJson()
        )
        if (yaml == null) {
            _importExportStatus.value = "No phoneme data to share"
            return
        }
        val intent = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_SUBJECT, "phonemes.yaml")
            putExtra(Intent.EXTRA_TEXT, yaml)
        }
        context.startActivity(Intent.createChooser(intent, "Share phonemes"))
    }
}
