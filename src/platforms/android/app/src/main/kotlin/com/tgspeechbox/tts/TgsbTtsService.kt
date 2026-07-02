/*
 * TgsbTtsService — Android TextToSpeechService backed by TGSpeechBox.
 *
 * Pipeline: text → eSpeak IPA → nvspFrontend → speechPlayer → PCM
 *
 * License: GPL-3.0 (links eSpeak-ng GPL)
 */

package com.tgspeechbox.tts

import android.content.SharedPreferences
import android.content.res.AssetManager
import android.media.AudioFormat
import android.speech.tts.SynthesisCallback
import android.speech.tts.SynthesisRequest
import android.speech.tts.TextToSpeech
import android.speech.tts.TextToSpeechService
import android.speech.tts.Voice
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.util.Locale

class TgsbTtsService : TextToSpeechService() {

    companion object {
        private const val TAG = "TgsbTTS"
        private const val SAMPLE_RATE = 22050
        const val PREFS_NAME = "tgsb_settings"
        const val PREF_VOICE_PRESET = "voice_preset"
        const val PREF_SUPPORTED_LANGUAGES = "tgsb_supported_languages"
        private const val PREF_LAST_LANG = "tgsb_last_lang"
        const val DEFAULT_PRESET = "adam"

        init {
            System.loadLibrary("tgspeechbox_jni")
        }

        /** Voice presets — timbre only, independent of language.
         *  isProfile=true means it's a YAML voice profile (phonemeOverrides
         *  + voicingTone from phonemes.yaml) rather than a DSP preset. */
        data class VoiceDef(
            val id: String, val label: String, val isProfile: Boolean = false
        )

        val VOICES = listOf(
            VoiceDef("adam",     "Adam"),
            VoiceDef("benjamin", "Benjamin"),
            VoiceDef("caleb",    "Caleb"),
            VoiceDef("david",    "David"),
            VoiceDef("robert",   "Robert"),
            VoiceDef("Beth",     "Beth",  isProfile = true),
            VoiceDef("Bobby",    "Bobby", isProfile = true),
        )

        /**
         * Language mapping: Android ISO-639-3 lang + ISO-3166 country
         * → eSpeak language tag + tgsb lang pack name.
         */
        data class LangDef(
            val iso3Lang: String,       // Android 3-letter lang (eng, fra, deu ...)
            val iso3Country: String,    // Android 3-letter country ("" = any)
            val espeakLang: String,     // eSpeak voice language tag
            val tgsbLang: String,       // nvspFrontend language pack
            val displayLocale: Locale   // for Voice API
        )

        val LANGUAGES = listOf(
            // English
            LangDef("eng", "USA", "en-us", "en-us", Locale("en", "US")),
            LangDef("eng", "GBR", "en-gb", "en-gb", Locale("en", "GB")),
            LangDef("eng", "AUS", "en",    "en-au", Locale("en", "AU")),
            LangDef("eng", "CAN", "en-us", "en-ca", Locale("en", "CA")),
            LangDef("eng", "",    "en-us", "en-us", Locale("en", "US")), // fallback

            // Romance
            LangDef("fra", "",    "fr",    "fr",    Locale("fr")),
            LangDef("spa", "MEX", "es-mx", "es-mx", Locale("es", "MX")),
            LangDef("spa", "",    "es",    "es",    Locale("es")),
            LangDef("ita", "",    "it",    "it",    Locale("it")),
            LangDef("por", "BRA", "pt-br", "pt-br", Locale("pt", "BR")),
            LangDef("por", "PRT", "pt",    "pt-pt", Locale("pt", "PT")),
            LangDef("por", "",    "pt",    "pt",    Locale("pt")),
            LangDef("ron", "",    "ro",    "ro",    Locale("ro")),

            // Germanic
            LangDef("deu", "",    "de",    "de",    Locale("de")),
            LangDef("nld", "",    "nl",    "nl",    Locale("nl")),
            LangDef("dan", "",    "da",    "da",    Locale("da")),
            LangDef("swe", "",    "sv",    "sv",    Locale("sv")),

            // Slavic
            LangDef("pol", "",    "pl",    "pl",    Locale("pl")),
            LangDef("ces", "",    "cs",    "cs",    Locale("cs")),
            LangDef("slk", "",    "sk",    "sk",    Locale("sk")),
            LangDef("bul", "",    "bg",    "bg",    Locale("bg")),
            LangDef("hrv", "",    "hr",    "hr",    Locale("hr")),
            LangDef("rus", "",    "ru",    "ru",    Locale("ru")),
            LangDef("ukr", "",    "uk",    "uk",    Locale("uk")),

            // Uralic
            LangDef("hun", "",    "hu",    "hu",    Locale("hu")),
            LangDef("fin", "",    "fi",    "fi",    Locale("fi")),

            // Turkic
            LangDef("tur", "",    "tr",    "tr",    Locale("tr")),

            // Other
            LangDef("zho", "",    "cmn",   "zh",    Locale("zh")),
        )

        /** Unique base languages (for LANG_AVAILABLE matching) */
        private val SUPPORTED_LANGS = LANGUAGES.map { it.iso3Lang }.toSet()

        /** (lang, country) pairs that match LANG_COUNTRY_AVAILABLE */
        private val SUPPORTED_PAIRS = LANGUAGES
            .filter { it.iso3Country.isNotEmpty() }
            .map { it.iso3Lang to it.iso3Country }
            .toSet()

        /** Look up the best LangDef for a given locale */
        fun findLangDef(lang: String, country: String): LangDef? {
            if (country.isNotEmpty()) {
                LANGUAGES.find { it.iso3Lang == lang && it.iso3Country == country }
                    ?.let { return it }
            }
            return LANGUAGES.find { it.iso3Lang == lang && it.iso3Country.isEmpty() }
        }

        /** Build a voice name encoding the language */
        private fun buildVoiceName(ld: LangDef): String {
            return if (ld.iso3Country.isNotEmpty()) {
                "tgsb-${ld.iso3Lang}-${ld.iso3Country}".lowercase()
            } else {
                "tgsb-${ld.iso3Lang}".lowercase()
            }
        }

        /** Parse a voice name back into a LangDef */
        private fun parseVoiceName(voiceName: String): LangDef? {
            val lower = voiceName.lowercase()
            for (ld in LANGUAGES) {
                if (buildVoiceName(ld) == lower) return ld
            }
            return null
        }

        /**
         * Get the set of enabled locale keys from prefs.
         * null = no preference set → all languages enabled.
         */
        fun getEnabledLocaleKeys(prefs: SharedPreferences): Set<String>? {
            return prefs.getStringSet(PREF_SUPPORTED_LANGUAGES, null)
        }

        /** Check if a LangDef is enabled by the user's language filter. */
        fun isLangEnabled(ld: LangDef, enabledKeys: Set<String>?): Boolean {
            if (enabledKeys == null) return true  // no pref = all enabled
            return ld.displayLocale.toString() in enabledKeys
        }
    }

    @Volatile
    private var stopRequested = false
    /** Any settings change (app UI, editor) marks this dirty; the next
     *  utterance re-applies voice + advanced settings + overrides once.
     *  Replaces re-applying everything on every utterance. */
    @Volatile
    private var settingsDirty = true
    /* SharedPreferences holds listeners weakly — keep a strong reference. */
    private val prefsListener =
        SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
            if (key != PREF_LAST_LANG) settingsDirty = true
        }
    private var nativeHandle: Long = 0L
    private var currentPreset: String = DEFAULT_PRESET
    private var currentSampleRate: Int = SAMPLE_RATE
    private var currentLang: LangDef = LANGUAGES[0] // en-us default
    /** Tracks what the native side actually has loaded.
     *  null = unknown/failed — forces re-try on next synthesis. */
    private var confirmedNativeLang: LangDef? = null
    private var cachedOverridesVersion: Int = -1
    private lateinit var prefs: SharedPreferences

    // JNI declarations
    private external fun nativeCreate(
        espeakDataPath: String, packDirPath: String, sampleRate: Int
    ): Long
    private external fun nativeDestroy(handle: Long)
    private external fun nativeSetVoice(handle: Long, voiceName: String)
    private external fun nativeSetVoiceProfile(handle: Long, profileName: String)
    private external fun nativeGetVoiceProfileNames(handle: Long): String
    private external fun nativeQueueText(
        handle: Long, text: String, speechRate: Int, pitch: Int
    )
    private external fun nativePullAudio(
        handle: Long, outBuffer: ByteArray, maxBytes: Int
    ): Int
    private external fun nativeStop(handle: Long)
    private external fun nativeSetLanguage(
        handle: Long, espeakLang: String, tgsbLang: String
    ): Int
    private external fun nativeSetVoicingTone(
        handle: Long,
        voicedTiltDbPerOct: Double,
        noiseGlottalModDepth: Double,
        pitchSyncF1DeltaHz: Double,
        pitchSyncB1DeltaHz: Double,
        speedQuotient: Double,
        aspirationTiltDbPerOct: Double,
        cascadeBwScale: Double,
        tremorDepth: Double,
        nasalBwScale: Double,
        f4FreqScale: Double,
        nasalGainScale: Double,
        chorusDepth: Double,
        chorusDetuneHz: Double
    )
    private external fun nativeSetFrameExDefaults(
        handle: Long,
        creakiness: Double,
        breathiness: Double,
        jitter: Double,
        shimmer: Double,
        sharpness: Double
    )
    private external fun nativeSetPitchMode(handle: Long, mode: String): Int
    private external fun nativeSetInflectionScale(handle: Long, scale: Double)
    private external fun nativeSetInflection(handle: Long, value: Double)
    private external fun nativeSetVolume(handle: Long, value: Double)
    private external fun nativeSetSampleRate(handle: Long, sampleRate: Int)
    private external fun nativeSetPauseMode(handle: Long, mode: Int)
    private external fun nativeApplySettingOverrides(handle: Long, yamlSnippet: String): Int
    private external fun nativeSetData(handle: Long, domain: Int, langTag: String, key: String, value: String): Int

    override fun onCreate() {
        super.onCreate()
        Log.i(TAG, "onCreate: extracting assets and initializing engine")

        prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        prefs.registerOnSharedPreferenceChangeListener(prefsListener)
        loadPresetFromPrefs()

        extractAssets()

        val espeakDataPath = filesDir.absolutePath
        val packDirPath = File(filesDir, "tgsb").absolutePath

        nativeHandle = nativeCreate(espeakDataPath, packDirPath, SAMPLE_RATE)
        if (nativeHandle == 0L) {
            Log.e(TAG, "Failed to create native engine")
        } else {
            Log.i(TAG, "Native engine created (handle=$nativeHandle)")
            applyCurrentVoice()

            // Restore the last language from prefs (survives process kills).
            // nativeCreate initializes en-us, so if the saved language is
            // different, we need to explicitly set it now.
            val savedLang = prefs.getString(PREF_LAST_LANG, null)
            val restoredLd = if (savedLang != null) {
                LANGUAGES.find { it.tgsbLang == savedLang }
            } else null

            if (restoredLd != null && restoredLd.tgsbLang != "en-us") {
                currentLang = restoredLd
                setNativeLanguage(restoredLd)
                Log.i(TAG, "Restored language from prefs: ${restoredLd.tgsbLang}")
            } else {
                confirmedNativeLang = currentLang
            }
        }
    }

    /** Set the native language, tracking success/failure. */
    private fun setNativeLanguage(ld: LangDef): Boolean {
        if (nativeHandle == 0L) return false
        val result = nativeSetLanguage(nativeHandle, ld.espeakLang, ld.tgsbLang)
        return if (result == 0) {
            confirmedNativeLang = ld
            prefs.edit().putString(PREF_LAST_LANG, ld.tgsbLang).apply()
            applyStoredOverrides(ld.tgsbLang)
            true
        } else {
            confirmedNativeLang = null
            Log.e(TAG, "setNativeLanguage FAILED: ${ld.espeakLang}/${ld.tgsbLang}")
            false
        }
    }

    /** Apply pack setting overrides saved by the editor via per-key setData. */
    private fun applyStoredOverrides(tgsbLang: String) {
        if (nativeHandle == 0L) return
        // Phoneme overrides (global, not per-language).
        applyPhonemeOverrides()
        // Pack settings overrides (may not exist — that's OK).
        val json = prefs.getString("pack_overrides_$tgsbLang", null)
        if (json != null) {
            val overrides = try {
                val obj = org.json.JSONObject(json)
                obj.keys().asSequence().associateWith { obj.getString(it) }
            } catch (e: Exception) { emptyMap() }
            for ((k, v) in overrides) {
                nativeSetData(nativeHandle, TgsbSpeakEngine.DATA_SETTINGS, tgsbLang, k, v)
            }
        }
        // Always apply dictionary overlays and exclusions.
        applyDictOverrides(tgsbLang)
        applyDictDisabled(tgsbLang)
    }

    /** Apply user phoneme overrides saved by the phoneme editor. */
    private fun applyPhonemeOverrides() {
        if (nativeHandle == 0L) return
        val json = prefs.getString("phoneme_overrides", null) ?: return
        val overrides = try {
            val obj = org.json.JSONObject(json)
            obj.keys().asSequence().associateWith { obj.getString(it) }
        } catch (e: Exception) { return }
        for ((k, v) in overrides) {
            nativeSetData(nativeHandle, TgsbSpeakEngine.DATA_PHONEMES, "", k, v)
        }
    }

    /** Apply user dictionary overlays saved by the editor. */
    private fun applyDictOverrides(tgsbLang: String) {
        if (nativeHandle == 0L) return
        val json = prefs.getString("dict_overrides_$tgsbLang", null)
        if (json == null) return
        val overrides = try {
            val obj = org.json.JSONObject(json)
            obj.keys().asSequence().associateWith { obj.getString(it) }
        } catch (e: Exception) { return }
        for ((k, v) in overrides) {
            nativeSetData(nativeHandle, TgsbSpeakEngine.DATA_DICTIONARY, tgsbLang, k, v)
        }
    }

    /** Apply dictionary type exclusions saved by the editor. */
    private fun applyDictDisabled(tgsbLang: String) {
        if (nativeHandle == 0L) return
        val json = prefs.getString("dict_disabled_$tgsbLang", null) ?: return
        val disabled = try {
            val arr = org.json.JSONArray(json)
            (0 until arr.length()).map { arr.getString(it) }
        } catch (e: Exception) { return }
        for (type in disabled) {
            nativeSetData(nativeHandle, TgsbSpeakEngine.DATA_DICTIONARY, "config:$tgsbLang", type, "false")
        }
    }

    /**
     * Read Advanced tab sliders from SharedPreferences and apply to the
     * native engine.  Called before each synthesis so TalkBack picks up
     * any changes the user made in the consumer UI.
     *
     * Per-voice settings use key "adv_key.voiceName" with fallback to
     * the global "adv_key" for migration from pre-per-voice builds.
     */
    private fun applyAdvancedSettings() {
        if (nativeHandle == 0L) return
        val voice = currentPreset  // e.g. "adam", "benjamin"

        // VoicingTone (slider 0-100 → real range, same math as TgsbViewModel)
        val tiltSlider   = prefFloat("voiceTilt", 50f, voice)
        val noiseMod     = prefFloat("noiseGlottalMod", 0f, voice) / 100f
        val psF1         = (prefFloat("pitchSyncF1", 50f, voice) - 50f) * 1.2f
        val psB1         = (prefFloat("pitchSyncB1", 50f, voice) - 50f) * 1.0f
        val sqSlider     = prefFloat("speedQuotient", 50f, voice)
        val aspTilt      = (prefFloat("aspirationTilt", 50f, voice) - 50f) * 0.24f
        val bwSlider     = prefFloat("cascadeBwScale", 50f, voice)
        val tremorSlider = prefFloat("voiceTremor", 0f, voice)
        val hsSlider     = prefFloat("headSize", if (voice == "david") 100f else 50f, voice)

        val chorusDepthSlider = prefFloat("chorusDepth", 0f, voice)
        val chorusDetuneSlider = prefFloat("chorusDetune", 33f, voice)

        val tilt = (tiltSlider - 50f) * (24f / 50f)
        val sq = if (sqSlider <= 50f)
            0.5 + (sqSlider / 50.0) * 1.5
        else
            2.0 + ((sqSlider - 50.0) / 50.0) * 2.0
        val bw = if (bwSlider <= 50f)
            2.0 - (bwSlider / 50.0) * 1.0
        else
            1.0 - ((bwSlider - 50.0) / 50.0) * 0.7
        val tremor = (tremorSlider / 100f) * 0.4f
        val hs = if (hsSlider <= 50f)
            1.25 - (hsSlider / 50.0) * 0.25
        else
            1.0 - ((hsSlider - 50.0) / 50.0) * 0.15
        val chorusDepth = chorusDepthSlider / 100.0
        val chorusDetune = 0.5 + (chorusDetuneSlider / 100.0) * 4.5

        nativeSetVoicingTone(
            nativeHandle,
            tilt.toDouble(), noiseMod.toDouble(),
            psF1.toDouble(), psB1.toDouble(),
            sq, aspTilt.toDouble(), bw, tremor.toDouble(),
            1.0, hs, 1.0,
            chorusDepth, chorusDetune
        )

        // FrameEx defaults
        val creak     = prefFloat("creakiness", 0f, voice) / 100f
        val breath    = prefFloat("breathiness", 0f, voice) / 100f
        val jit       = prefFloat("jitter", 0f, voice) / 100f
        val shim      = prefFloat("shimmer", 0f, voice) / 100f
        val sharpness = prefFloat("glottalSharpness", 50f, voice) / 50f

        nativeSetFrameExDefaults(
            nativeHandle,
            creak.toDouble(), breath.toDouble(),
            jit.toDouble(), shim.toDouble(), sharpness.toDouble()
        )

        // Pitch mode + inflection scale + inflection (per-voice)
        val pitchMode = prefString("pitchMode", "espeak_style", voice)
        nativeSetPitchMode(nativeHandle, pitchMode)

        val inflScale = prefFloat("inflectionScale", 58f, voice) / 100f
        nativeSetInflectionScale(nativeHandle, inflScale.toDouble())

        val inflection = prefFloat("inflection", 50f, voice) / 100f
        nativeSetInflection(nativeHandle, inflection.toDouble())

        // Global output settings (not per-voice)
        val volume = prefs.getFloat("adv_systemVolume", 1.0f).coerceIn(0.05f, 1.0f)
        nativeSetVolume(nativeHandle, volume.toDouble())

        val sampleRate = prefs.getInt("adv_sampleRate", SAMPLE_RATE)
        nativeSetSampleRate(nativeHandle, sampleRate)
        currentSampleRate = sampleRate

        val pauseMode = prefs.getInt("adv_pauseMode", 1)  // default: short
        nativeSetPauseMode(nativeHandle, pauseMode)
    }

    /** Read per-voice float: "adv_key.voice" with fallback to "adv_key". */
    private fun prefFloat(key: String, default: Float, voice: String): Float {
        val voiceKey = "adv_$key.$voice"
        if (prefs.contains(voiceKey)) return prefs.getFloat(voiceKey, default)
        return prefs.getFloat("adv_$key", default)
    }

    /** Read per-voice string: "adv_key.voice" with fallback to "adv_key". */
    private fun prefString(key: String, default: String, voice: String): String {
        val voiceKey = "adv_$key.$voice"
        if (prefs.contains(voiceKey)) return prefs.getString(voiceKey, default) ?: default
        return prefs.getString("adv_$key", default) ?: default
    }

    override fun onDestroy() {
        prefs.unregisterOnSharedPreferenceChangeListener(prefsListener)
        if (nativeHandle != 0L) {
            nativeDestroy(nativeHandle)
            nativeHandle = 0L
        }
        super.onDestroy()
    }

    private fun loadPresetFromPrefs() {
        val saved = prefs.getString(PREF_VOICE_PRESET, DEFAULT_PRESET) ?: DEFAULT_PRESET
        currentPreset = if (VOICES.any { it.id == saved }) saved else DEFAULT_PRESET
    }

    /** Apply the current voice — either a DSP preset or a YAML profile. */
    private fun applyCurrentVoice() {
        if (nativeHandle == 0L) return
        val voiceDef = VOICES.find { it.id == currentPreset }
        if (voiceDef != null && voiceDef.isProfile) {
            // YAML voice profile: set base voice to "adam" then apply profile
            nativeSetVoice(nativeHandle, "adam")
            nativeSetVoiceProfile(nativeHandle, voiceDef.id)
        } else {
            nativeSetVoice(nativeHandle, currentPreset)
        }
    }

    // ---- Asset extraction ----

    private fun extractAssets() {
        val assetVersion = 28
        val marker = File(filesDir, ".assets_v$assetVersion")
        if (marker.exists()) return

        filesDir.listFiles()?.filter { it.name.startsWith(".assets_") }?.forEach { it.delete() }
        File(filesDir, "espeak-ng-data").deleteRecursively()
        File(filesDir, "tgsb").deleteRecursively()

        Log.i(TAG, "Extracting assets to ${filesDir.absolutePath}")
        copyAssetsDir("espeak-ng-data", File(filesDir, "espeak-ng-data"))
        copyAssetsDir("tgsb", File(filesDir, "tgsb"))
        marker.createNewFile()
    }

    private fun copyAssetsDir(assetPath: String, targetDir: File) {
        val assetMgr = assets
        val entries = assetMgr.list(assetPath) ?: return

        if (entries.isEmpty()) {
            copyAssetFile(assetMgr, assetPath, targetDir)
            return
        }

        targetDir.mkdirs()
        for (entry in entries) {
            val childAsset = "$assetPath/$entry"
            val childTarget = File(targetDir, entry)
            val subEntries = assetMgr.list(childAsset)
            if (subEntries != null && subEntries.isNotEmpty()) {
                copyAssetsDir(childAsset, childTarget)
            } else {
                copyAssetFile(assetMgr, childAsset, childTarget)
            }
        }
    }

    private fun copyAssetFile(assetMgr: AssetManager, assetPath: String, target: File) {
        try {
            target.parentFile?.mkdirs()
            assetMgr.open(assetPath).use { input ->
                FileOutputStream(target).use { output ->
                    input.copyTo(output)
                }
            }
        } catch (e: IOException) {
            Log.e(TAG, "Failed to copy asset $assetPath: ${e.message}")
        }
    }

    // ---- Locale-based API (required abstract methods) ----

    override fun onIsLanguageAvailable(lang: String, country: String, variant: String): Int {
        if (lang !in SUPPORTED_LANGS) return TextToSpeech.LANG_NOT_SUPPORTED
        if (country.isNotEmpty() && (lang to country) in SUPPORTED_PAIRS)
            return TextToSpeech.LANG_COUNTRY_AVAILABLE
        return TextToSpeech.LANG_AVAILABLE
    }

    override fun onLoadLanguage(lang: String, country: String, variant: String): Int {
        val availability = onIsLanguageAvailable(lang, country, variant)
        if (availability == TextToSpeech.LANG_NOT_SUPPORTED) return availability

        val ld = findLangDef(lang, country) ?: return TextToSpeech.LANG_NOT_SUPPORTED
        currentLang = ld
        if (confirmedNativeLang != ld && setNativeLanguage(ld)) {
            Log.i(TAG, "Language loaded: ${ld.espeakLang} / ${ld.tgsbLang}")
        }
        return availability
    }

    override fun onGetLanguage(): Array<String> {
        return arrayOf(currentLang.iso3Lang, currentLang.iso3Country, "")
    }

    // ---- Voice API (API 21+) ----

    override fun onGetVoices(): List<Voice> {
        // One voice per supported language. Timbre (preset) is selected
        // via the settings activity, not through the Voice API.
        // Only expose languages the user has enabled in settings.
        // Skip fallback entries whose displayLocale duplicates a more
        // specific entry — Android gets confused by duplicate locales.
        val enabledKeys = getEnabledLocaleKeys(prefs)
        val seen = mutableSetOf<String>()
        return LANGUAGES.mapNotNull { ld ->
            if (!isLangEnabled(ld, enabledKeys)) return@mapNotNull null
            val key = ld.displayLocale.toString()
            if (key in seen) return@mapNotNull null
            seen.add(key)
            Voice(
                buildVoiceName(ld),
                ld.displayLocale,
                Voice.QUALITY_VERY_HIGH,
                Voice.LATENCY_NORMAL,
                false,
                emptySet()
            )
        }
    }

    override fun onIsValidVoiceName(voiceName: String): Int {
        return if (parseVoiceName(voiceName) != null) TextToSpeech.SUCCESS
               else TextToSpeech.ERROR
    }

    override fun onLoadVoice(voiceName: String): Int {
        val ld = parseVoiceName(voiceName) ?: return TextToSpeech.ERROR

        // Hot path: screen readers call onLoadVoice before nearly every
        // utterance. When the pack is already loaded, skip the reload
        // chain — settings freshness is handled by the dirty flag in
        // onSynthesizeText.
        currentLang = ld
        if (confirmedNativeLang == ld) return TextToSpeech.SUCCESS

        // Re-read preset and advanced voice quality settings
        loadPresetFromPrefs()
        if (nativeHandle != 0L) {
            applyCurrentVoice()
        }

        // Set language BEFORE advanced settings — setPitchMode and
        // setInflectionScale write to h->pack which is only initialized
        // after setLanguage loads the pack.
        if (setNativeLanguage(ld)) {
            Log.i(TAG, "Voice loaded: $voiceName → lang=${ld.espeakLang}/${ld.tgsbLang} preset=$currentPreset")
        }
        applyAdvancedSettings()

        return TextToSpeech.SUCCESS
    }

    override fun onGetDefaultVoiceNameFor(
        lang: String, country: String, variant: String
    ): String {
        val ld = findLangDef(lang, country)
            ?: findLangDef(lang, "")
            ?: LANGUAGES[0]
        return buildVoiceName(ld)
    }

    // ---- Synthesis ----

    override fun onSynthesizeText(request: SynthesisRequest, callback: SynthesisCallback) {
        if (nativeHandle == 0L) {
            Log.e(TAG, "Engine not initialized")
            callback.error(TextToSpeech.ERROR_SYNTHESIS)
            return
        }

        // Lock language: if enabled, use the Speak tab's saved language
        // instead of whatever the screen reader / system requests.
        val lockLang = prefs.getBoolean("adv_lockLanguage", false)
        if (lockLang) {
            val lockedTag = prefs.getString("tgsb_speak_lang", null)
            if (lockedTag != null) {
                val ld = LANGUAGES.find { it.tgsbLang == lockedTag }
                if (ld != null) currentLang = ld
            }
        } else {
            // Resolve language from the request.  Voice name takes priority
            // over locale fields (the Voice API is the modern path).
            val voiceName = request.voiceName
            if (!voiceName.isNullOrEmpty()) {
                val ld = parseVoiceName(voiceName)
                if (ld != null) currentLang = ld
            } else {
                val reqLang = request.language ?: ""
                val reqCountry = request.country ?: ""
                if (reqLang.isNotEmpty()) {
                    val ld = findLangDef(reqLang, reqCountry)
                    if (ld != null) currentLang = ld
                }
            }
        }

        // Check overrides version: if the editor changed overrides, force
        // a full pack reload so cleared/changed overrides take effect.
        val overridesVer = prefs.getInt("pack_overrides_version", 0)
        val overridesChanged = overridesVer != cachedOverridesVersion
        cachedOverridesVersion = overridesVer
        val langChanged = confirmedNativeLang != currentLang

        // Voice, overrides, and advanced settings are engine state that
        // persists across utterances — re-apply only when something
        // actually changed (prefs listener sets settingsDirty). This was
        // previously done on every utterance and dominated time-to-first-
        // audio on slow devices.
        val applySettings = settingsDirty || langChanged || overridesChanged
        if (applySettings) {
            settingsDirty = false
            loadPresetFromPrefs()
            applyCurrentVoice()
        }

        // Language must be loaded BEFORE advanced settings — setPitchMode
        // writes to h->pack which requires a loaded language pack.
        if (langChanged || overridesChanged) {
            setNativeLanguage(currentLang)  // applies stored overrides itself
        } else if (applySettings) {
            // Editor writes that don't bump pack_overrides_version
            // (phoneme overrides, dict overlays/exclusions) land here.
            applyStoredOverrides(currentLang.tgsbLang)
        }
        if (applySettings) {
            applyAdvancedSettings()
        }

        // Start audio stream
        val ret = callback.start(
            currentSampleRate, AudioFormat.ENCODING_PCM_16BIT, 1
        )
        if (ret != TextToSpeech.SUCCESS) {
            callback.error(TextToSpeech.ERROR_SYNTHESIS)
            return
        }

        // Get text
        val text = request.charSequenceText?.toString()
            ?: request.text
            ?: ""
        if (text.isEmpty()) {
            callback.done()
            return
        }

        // Queue text through pipeline (fast: eSpeak IPA + frontend frames)
        // Apply global rate override if enabled — some apps ignore user's
        // TTS rate setting, so this forces a consistent rate.
        var effectiveRate = request.speechRate
        if (prefs.getBoolean("adv_overrideSystemRate", false)) {
            val rate = prefs.getFloat("adv_globalRate", 1.0f)
            effectiveRate = (rate * 100f).toInt()
        }
        if (prefs.getBoolean("adv_rateBoostEnabled", false)) {
            effectiveRate = effectiveRate * 2
        }
        // Stale-stop guard: onStop() arrives on a binder thread with no
        // utterance identity, so a stop aimed at the PREVIOUS utterance
        // can land any time during the setup above. Arm the flag only
        // now, when this utterance becomes the one a stop should kill.
        // (nativeQueueText resets the native-side flag on entry, so the
        // two views stay consistent at this boundary.)
        stopRequested = false
        try {
            nativeQueueText(nativeHandle, text, effectiveRate, request.pitch)
        } catch (e: Exception) {
            Log.e(TAG, "Queue failed: ${e.message}")
            callback.error(TextToSpeech.ERROR_SYNTHESIS)
            return
        }

        // Stream PCM as it's synthesized — audio starts playing immediately
        val buf = ByteArray(8192) // 4096 samples ≈ 186ms at 22050Hz s16le
        while (!stopRequested) {
            val n = nativePullAudio(nativeHandle, buf, buf.size)
            if (n <= 0) break
            if (callback.audioAvailable(buf, 0, n) != TextToSpeech.SUCCESS) {
                // Framework already abandoned this item (stopped or client
                // died) — an interruption, not a failure.
                break
            }
        }

        // A stop is an interruption, not an error. Reporting error() here
        // made screen readers treat the utterance as FAILED and go silent
        // on fast navigation (issue #100 side report); eSpeak reports
        // completion on abort and clients recover.
        callback.done()
    }

    override fun onStop() {
        stopRequested = true
        if (nativeHandle != 0L) {
            nativeStop(nativeHandle)
        }
    }
}
