/*
 * TgsbSpeakEngine — Standalone TTS engine for the consumer Speak UI.
 *
 * Drives the full pipeline (eSpeak → nvspFrontend → speechPlayer)
 * directly via JNI, bypassing the Android TextToSpeechService.
 * This avoids TalkBack fighting our utterances for the shared TTS
 * audio path.  Audio is played via AudioTrack on a background thread.
 *
 * Mirrors iOS TgsbEngine.swift.
 *
 * License: GPL-3.0
 */

package com.tgspeechbox.tts

import android.content.Context
import android.content.res.AssetManager
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.io.IOException

class TgsbSpeakEngine(private val context: Context) {

    companion object {
        private const val TAG = "TgsbSpeak"
        private const val DEFAULT_SAMPLE_RATE = 22050
        const val DATA_SETTINGS = 0
        const val DATA_PHONEMES = 1
        const val DATA_DICTIONARY = 2

        init {
            System.loadLibrary("tgspeechbox_jni")
        }
    }

    var sampleRate: Int = DEFAULT_SAMPLE_RATE
        private set

    private var nativeHandle: Long = 0L
    @Volatile
    private var stopRequested = false
    private var synthThread: Thread? = null
    private var audioTrack: AudioTrack? = null
    @Volatile
    private var currentVolume: Float = 1.0f

    var isSpeaking: Boolean = false
        private set

    var onSpeakingChanged: ((Boolean) -> Unit)? = null

    // ── JNI declarations ─────────────────────────────────────────────

    private external fun nativeCreate(
        espeakDataPath: String, packDirPath: String, sampleRate: Int
    ): Long
    private external fun nativeDestroy(handle: Long)
    private external fun nativeSetVoice(handle: Long, voiceName: String)
    private external fun nativeSetVoiceProfile(handle: Long, profileName: String)
    private external fun nativeGetVoiceProfileNames(handle: Long): String
    private external fun nativeSetLanguage(
        handle: Long, espeakLang: String, tgsbLang: String
    ): Int
    private external fun nativeQueueText(
        handle: Long, text: String, speed: Double, pitchHz: Double
    )
    private external fun nativeQueueIpa(
        handle: Long, ipa: String, speed: Double, pitchHz: Double
    )
    private external fun nativePullAudio(
        handle: Long, outBuffer: ShortArray, maxSamples: Int
    ): Int
    private external fun nativeStop(handle: Long)
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
    private external fun nativeGetAvailableLanguages(handle: Long): String?
    private external fun nativeGetDataCount(handle: Long, domain: Int, langTag: String): Int
    private external fun nativeQueryData(handle: Long, domain: Int, langTag: String, offset: Int, limit: Int): String?
    private external fun nativeSetData(handle: Long, domain: Int, langTag: String, key: String, value: String): Int
    private external fun nativeExportData(handle: Long, domain: Int, langTag: String, overridesJson: String): String?
    private external fun nativeTextToIpa(handle: Long, text: String): String

    // ── Lifecycle ────────────────────────────────────────────────────

    fun start(): Boolean {
        if (nativeHandle != 0L) return true

        extractAssets()

        val filesDir = context.filesDir
        val espeakDataPath = filesDir.absolutePath
        val packDirPath = File(filesDir, "tgsb").absolutePath

        if (!File(espeakDataPath, "espeak-ng-data").exists()) {
            Log.e(TAG, "espeak-ng-data not found at $espeakDataPath")
            return false
        }

        nativeHandle = nativeCreate(espeakDataPath, packDirPath, sampleRate)
        if (nativeHandle == 0L) {
            Log.e(TAG, "nativeCreate failed")
            return false
        }

        Log.i(TAG, "Engine started (handle=$nativeHandle)")
        return true
    }

    fun shutdown() {
        stop()
        if (nativeHandle != 0L) {
            nativeDestroy(nativeHandle)
            nativeHandle = 0L
        }
    }

    // ── Voice quality settings ─────────────────────────────────────

    fun setVoicingTone(
        voicedTiltDbPerOct: Double,
        noiseGlottalModDepth: Double,
        pitchSyncF1DeltaHz: Double,
        pitchSyncB1DeltaHz: Double,
        speedQuotient: Double,
        aspirationTiltDbPerOct: Double,
        cascadeBwScale: Double,
        tremorDepth: Double,
        f4FreqScale: Double = 1.0,
        chorusDepth: Double = 0.0,
        chorusDetuneHz: Double = 2.0
    ) {
        if (nativeHandle == 0L) return
        nativeSetVoicingTone(
            nativeHandle,
            voicedTiltDbPerOct, noiseGlottalModDepth,
            pitchSyncF1DeltaHz, pitchSyncB1DeltaHz,
            speedQuotient, aspirationTiltDbPerOct,
            cascadeBwScale, tremorDepth,
            1.0, f4FreqScale, 1.0,
            chorusDepth, chorusDetuneHz
        )
    }

    fun setFrameExDefaults(
        creakiness: Double,
        breathiness: Double,
        jitter: Double,
        shimmer: Double,
        sharpness: Double
    ) {
        if (nativeHandle == 0L) return
        nativeSetFrameExDefaults(
            nativeHandle,
            creakiness, breathiness, jitter, shimmer, sharpness
        )
    }

    fun setPitchMode(mode: String): Boolean {
        if (nativeHandle == 0L) return false
        return nativeSetPitchMode(nativeHandle, mode) == 1
    }

    fun setInflectionScale(scale: Double) {
        if (nativeHandle == 0L) return
        nativeSetInflectionScale(nativeHandle, scale)
    }

    fun setInflection(value: Double) {
        if (nativeHandle == 0L) return
        nativeSetInflection(nativeHandle, value)
    }

    fun setSampleRate(rate: Int) {
        if (rate == sampleRate) return
        sampleRate = rate
        if (nativeHandle != 0L) {
            nativeSetSampleRate(nativeHandle, rate)
        }
    }

    fun setVolume(volume: Float) {
        currentVolume = volume.coerceIn(0.05f, 1.0f)
        if (nativeHandle != 0L) {
            nativeSetVolume(nativeHandle, currentVolume.toDouble())
        }
    }

    fun setPauseMode(mode: Int) {
        if (nativeHandle == 0L) return
        nativeSetPauseMode(nativeHandle, mode)
    }

    // ── Speak / Stop ─────────────────────────────────────────────────

    fun setLanguage(espeakLang: String, tgsbLang: String): Boolean {
        if (nativeHandle == 0L) return false
        val result = nativeSetLanguage(nativeHandle, espeakLang, tgsbLang)
        Log.i(TAG, "setLanguage($espeakLang, $tgsbLang) = $result")
        return result == 0
    }

    fun setVoice(voiceName: String) {
        if (nativeHandle == 0L) return
        val voiceDef = TgsbTtsService.VOICES.find { it.id == voiceName }
        if (voiceDef != null && voiceDef.isProfile) {
            nativeSetVoice(nativeHandle, "adam")
            nativeSetVoiceProfile(nativeHandle, voiceDef.id)
        } else {
            nativeSetVoice(nativeHandle, voiceName)
        }
    }

    fun speak(text: String, speed: Double, pitchHz: Double) {
        if (nativeHandle == 0L) return
        stop()

        stopRequested = false
        setSpeaking(true)

        synthThread = Thread({
            try {
                // Queue text (fast: eSpeak IPA + frontend frames)
                Log.i(TAG, "Queuing text: '${text.take(40)}' speed=$speed pitch=$pitchHz")
                nativeQueueText(nativeHandle, text, speed, pitchHz)

                // Pull all PCM into a buffer
                val chunk = ShortArray(4096)
                val allSamples = mutableListOf<Short>()

                while (!stopRequested) {
                    val n = nativePullAudio(nativeHandle, chunk, chunk.size)
                    if (n <= 0) break
                    for (i in 0 until n) allSamples.add(chunk[i])
                }

                Log.i(TAG, "Pulled ${allSamples.size} samples (stopRequested=$stopRequested)")

                if (stopRequested || allSamples.isEmpty()) {
                    setSpeaking(false)
                    return@Thread
                }

                // Play via AudioTrack (MODE_STREAM — blocks until done)
                val pcmArray = ShortArray(allSamples.size) { allSamples[it] }
                playPcm(pcmArray)
            } catch (e: Exception) {
                Log.e(TAG, "Synthesis error: ${e.message}", e)
                setSpeaking(false)
            }
        }, "TgsbSynth").also { it.start() }
    }

    // ── Phoneme preview ────────────────────────────────────────────

    /**
     * Preview a phoneme in isolation by synthesizing raw IPA.
     * Used by the phoneme editor — plays the sound immediately.
     */
    fun previewPhoneme(ipa: String, speed: Double = 1.0, pitchHz: Double = 120.0) {
        if (nativeHandle == 0L) return
        stop()

        stopRequested = false
        setSpeaking(true)

        synthThread = Thread({
            try {
                nativeQueueIpa(nativeHandle, ipa, speed, pitchHz)

                val chunk = ShortArray(4096)
                val allSamples = mutableListOf<Short>()

                while (!stopRequested) {
                    val n = nativePullAudio(nativeHandle, chunk, chunk.size)
                    if (n <= 0) break
                    for (i in 0 until n) allSamples.add(chunk[i])
                }

                if (stopRequested || allSamples.isEmpty()) {
                    setSpeaking(false)
                    return@Thread
                }

                val pcmArray = ShortArray(allSamples.size) { allSamples[it] }
                playPcm(pcmArray)
            } catch (e: Exception) {
                Log.e(TAG, "Phoneme preview error: ${e.message}", e)
                setSpeaking(false)
            }
        }, "TgsbPhonemePreview").also { it.start() }
    }

    // ── Pack settings editor ────────────────────────────────────────

    fun applySettingOverrides(yamlSnippet: String): Boolean {
        if (nativeHandle == 0L) return false
        return nativeApplySettingOverrides(nativeHandle, yamlSnippet) != 0
    }

    fun getAvailableLanguages(): List<String> {
        if (nativeHandle == 0L) return emptyList()
        val raw = nativeGetAvailableLanguages(nativeHandle) ?: return emptyList()
        return raw.trim().split('\n').filter { it.isNotEmpty() }
    }

    // ── Generic Data Query API (ABI v5+) ────────────────────────────

    fun getDataCount(domain: Int, langTag: String): Int {
        if (nativeHandle == 0L) return -1
        return nativeGetDataCount(nativeHandle, domain, langTag)
    }

    fun queryData(domain: Int, langTag: String, offset: Int = 0, limit: Int = 0): String? {
        if (nativeHandle == 0L) return null
        return nativeQueryData(nativeHandle, domain, langTag, offset, limit)
    }

    fun setData(domain: Int, langTag: String, key: String, value: String): Boolean {
        if (nativeHandle == 0L) return false
        return nativeSetData(nativeHandle, domain, langTag, key, value) != 0
    }

    fun exportData(domain: Int, langTag: String, overridesJson: String): String? {
        if (nativeHandle == 0L) return null
        return nativeExportData(nativeHandle, domain, langTag, overridesJson)
    }

    fun textToIpa(text: String): String {
        if (nativeHandle == 0L) return ""
        return nativeTextToIpa(nativeHandle, text)
    }

    fun stop() {
        stopRequested = true
        if (nativeHandle != 0L) nativeStop(nativeHandle)
        audioTrack?.stop()
        audioTrack?.release()
        audioTrack = null
        synthThread?.join(500)
        synthThread = null
        setSpeaking(false)
    }

    // ── Audio playback ───────────────────────────────────────────────

    private fun playPcm(samples: ShortArray) {
        val minBuf = AudioTrack.getMinBufferSize(
            sampleRate,
            AudioFormat.CHANNEL_OUT_MONO,
            AudioFormat.ENCODING_PCM_16BIT
        )

        val track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_ASSISTANCE_ACCESSIBILITY)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(sampleRate)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build()
            )
            .setBufferSizeInBytes(minBuf)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()

        audioTrack = track
        track.play()
        Log.i(TAG, "Playing ${samples.size} samples via MODE_STREAM")

        // Blocking write — feeds audio to the track in chunks.
        // write() in MODE_STREAM blocks when the buffer is full,
        // so this naturally paces playback.
        var offset = 0
        while (offset < samples.size && !stopRequested) {
            val written = track.write(samples, offset, samples.size - offset)
            if (written < 0) {
                Log.e(TAG, "AudioTrack.write error: $written")
                break
            }
            offset += written
        }

        // Wait for the track to finish playing the buffered audio
        if (!stopRequested) {
            // Drain: write silence equal to the internal buffer to flush
            // all real audio through the hardware.
            val silence = ShortArray(minBuf / 2)
            track.write(silence, 0, silence.size)
        }

        track.stop()
        track.release()
        audioTrack = null
        setSpeaking(false)
        Log.i(TAG, "Playback finished")
    }

    private fun setSpeaking(value: Boolean) {
        isSpeaking = value
        onSpeakingChanged?.invoke(value)
    }

    // ── Asset extraction (same logic as TgsbTtsService) ─────────────

    private fun extractAssets() {
        val assetVersion = 26
        val marker = File(context.filesDir, ".assets_v$assetVersion")
        if (marker.exists()) return

        context.filesDir.listFiles()
            ?.filter { it.name.startsWith(".assets_") }
            ?.forEach { it.delete() }
        File(context.filesDir, "espeak-ng-data").deleteRecursively()
        File(context.filesDir, "tgsb").deleteRecursively()

        Log.i(TAG, "Extracting assets to ${context.filesDir.absolutePath}")
        copyAssetsDir("espeak-ng-data", File(context.filesDir, "espeak-ng-data"))
        copyAssetsDir("tgsb", File(context.filesDir, "tgsb"))
        marker.createNewFile()
    }

    private fun copyAssetsDir(assetPath: String, targetDir: File) {
        val entries = context.assets.list(assetPath) ?: return
        if (entries.isEmpty()) {
            copyAssetFile(assetPath, targetDir)
            return
        }
        targetDir.mkdirs()
        for (entry in entries) {
            val childAsset = "$assetPath/$entry"
            val childTarget = File(targetDir, entry)
            val subEntries = context.assets.list(childAsset)
            if (subEntries != null && subEntries.isNotEmpty()) {
                copyAssetsDir(childAsset, childTarget)
            } else {
                copyAssetFile(childAsset, childTarget)
            }
        }
    }

    private fun copyAssetFile(assetPath: String, target: File) {
        try {
            target.parentFile?.mkdirs()
            context.assets.open(assetPath).use { input ->
                FileOutputStream(target).use { output ->
                    input.copyTo(output)
                }
            }
        } catch (e: IOException) {
            Log.e(TAG, "Failed to copy asset $assetPath: ${e.message}")
        }
    }
}
