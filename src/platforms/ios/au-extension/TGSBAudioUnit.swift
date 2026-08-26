/*
 * TGSBAudioUnit.swift — AVSpeechSynthesisProviderAudioUnit subclass.
 *
 * Registers TGSpeechBox as a system-wide speech synthesizer,
 * usable by VoiceOver, Spoken Content, and any AVSpeechSynthesizer client.
 *
 * Both macOS and iOS run the full pipeline (eSpeak + frontend + DSP)
 * in-process. Apple hosts AU extensions in a separate process from
 * VoiceOver on both platforms, so crash isolation is built in.
 *
 * License: GPL-3.0 (links eSpeak-ng)
 */

import AVFoundation
import Accelerate
import CoreMedia

public class TGSBAudioUnit: AVSpeechSynthesisProviderAudioUnit {

    private var engine: OpaquePointer?    // TgsbEngine* (full pipeline)

    // Audio buffer — written by synthesizeSpeechRequest (synchronous),
    // read by render block. Protected by outputMutex.
    private var output: [Float32] = []
    private var outputOffset: Int = 0
    private var volume: Float32 = 1.0
    private var outputMutex = DispatchSemaphore(value: 1)

    // Cancellation generation counter — protected by outputMutex.
    // The AU API carries no utterance identity: cancelSpeechRequest()
    // can execute concurrently with (or stale, after) a synthesis. The
    // engine-side stop flag alone had two races: a cancel landing during
    // utterance setup was wiped by tgsb_queue_text's flag reset (the
    // utterance then spoke IN FULL), and a stale cancel aimed at
    // utterance N could clear utterance N+1's buffer (element silenced).
    // Each request now takes a generation at entry; cancel records the
    // generation it saw; a request only hands audio to the render block
    // if its generation was never cancelled and is still current.
    private var requestGen = 0
    private var cancelledGen = -1

    // ASBD output rate — always 22050.
    // Lower rates (11025/16000) alias on the iPhone DAC; 44100 clicks.
    // 22050 is the sweet spot. DSP rate is resampled to match.
    private let sampleRate: Double = 22050.0

    // DSP rate — the rate speechPlayer actually runs at.
    // Can differ from sampleRate; output is resampled to match ASBD.
    private var dspRate: Int

    // Cached state to avoid redundant bridge calls & UserDefaults reads
    private var cachedVoice: String = ""
    private var cachedEspeakLang: String = ""
    private var cachedTgsbLang: String = ""
    private var cachedSettingsVersion: Int = -1
    private var cachedOverridesVersion: Int = -1
    private var requestCount: Int = 0

    // Reusable synthesis buffers to avoid per-utterance allocation
    private var pullChunk = [Int16](repeating: 0, count: 4096)

    // Audio Unit output bus.
    private let outputBus: AUAudioUnitBus
    private var _outputBusses: AUAudioUnitBusArray!
    private let outputFormat: AVAudioFormat

    // Language mapping: BCP-47 tag -> (espeakTag, tgsbTag)
    private static let languageMap: [(bcp47: String, espeak: String, tgsb: String)] = [
        ("en-US", "en-us", "en-us"),
        ("en-GB", "en-gb", "en-gb"),
        ("en-CA", "en-us", "en-ca"),
        ("en-AU", "en",    "en-au"),
        ("fr-FR", "fr",    "fr"),
        ("fr-CA", "fr",    "fr"),
        ("es-ES", "es",    "es"),
        ("es-MX", "es-419","es-mx"),
        ("it-IT", "it",    "it"),
        ("pt-BR", "pt-br", "pt-br"),
        ("pt-PT", "pt",    "pt"),
        ("ro-RO", "ro",    "ro"),
        ("de-DE", "de",    "de"),
        ("nl-NL", "nl",    "nl"),
        ("da-DK", "da",    "da"),
        ("sv-SE", "sv",    "sv"),
        ("pl-PL", "pl",    "pl"),
        ("cs-CZ", "cs",    "cs"),
        ("sk-SK", "sk",    "sk"),
        ("bg-BG", "bg",    "bg"),
        ("hr-HR", "hr",    "hr"),
        ("ru-RU", "ru",    "ru"),
        ("uk-UA", "uk",    "uk"),
        ("hu-HU", "hu",    "hu"),
        ("fi-FI", "fi",    "fi"),
        ("tr-TR", "tr",    "tr"),
        ("zh-CN", "cmn",   "zh"),
    ]

    // MARK: - Initialization

    private static func loadDspRate() -> Int {
        let d = UserDefaults(suiteName: "group.com.tgspeechbox.app")
        // Force cross-process sync so AU extension sees host app's writes.
        // Required for App Group containers after VoiceOver restart.
        d?.synchronize()
        let valid: Set<Int> = [11025, 16000, 22050, 44100]
        if let d = d, d.object(forKey: "adv_sampleRate") != nil {
            let saved = d.integer(forKey: "adv_sampleRate")
            if valid.contains(saved) { return saved }
        }
        return 22050
    }

    @objc
    public override init(componentDescription: AudioComponentDescription,
                         options: AudioComponentInstantiationOptions = []) throws {

        dspRate = Self.loadDspRate()

        let asbd = AudioStreamBasicDescription(
            mSampleRate: 22050,
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagsNativeFloatPacked
                        | kAudioFormatFlagIsNonInterleaved,
            mBytesPerPacket: 4,
            mFramesPerPacket: 1,
            mBytesPerFrame: 4,
            mChannelsPerFrame: 1,
            mBitsPerChannel: 32,
            mReserved: 0)

        outputFormat = AVAudioFormat(
            cmAudioFormatDescription: try CMAudioFormatDescription(
                audioStreamBasicDescription: asbd))
        outputBus = try AUAudioUnitBus(format: outputFormat)

        try super.init(componentDescription: componentDescription,
                       options: options)

        _outputBusses = AUAudioUnitBusArray(
            audioUnit: self,
            busType: .output,
            busses: [outputBus])

        initializeBackend()
    }

    private func initializeBackend() {
        let fm = FileManager.default

        // Look for resources in the containing app's bundle first (shared),
        // then fall back to the extension's own bundle (macOS or standalone).
        let extBundle = Bundle(for: TGSBAudioUnit.self).resourcePath ?? ""
        let hostBundle: String = {
            // On iOS, .appex lives inside Host.app/PlugIns/ — go up two levels.
            let appex = Bundle(for: TGSBAudioUnit.self).bundleURL
            let host = appex.deletingLastPathComponent().deletingLastPathComponent()
            return host.path
        }()

        let espeakDataPath: String
        let packDir: String
        if fm.fileExists(atPath: hostBundle + "/espeak-ng-data") {
            espeakDataPath = hostBundle + "/espeak-ng-data"
            packDir = hostBundle + "/packs"
        } else {
            espeakDataPath = extBundle + "/espeak-ng-data"
            packDir = extBundle + "/packs"
        }
        guard fm.fileExists(atPath: espeakDataPath),
              fm.fileExists(atPath: packDir) else { return }
        engine = tgsb_create(espeakDataPath, packDir, Int32(dspRate))

        // Override directory: app group container for user-imported packs/dicts.
        if let containerURL = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: "group.com.tgspeechbox.app") {
            tgsb_set_override_directory(engine, containerURL.path)
        }
    }

    deinit {
        if let e = engine { tgsb_destroy(e) }
    }

    // MARK: - Voices

    /// Discover all available voices: DSP presets + YAML voice profiles.
    /// Profile names are queried dynamically from the engine so that
    /// user-defined profiles in phonemes.yaml appear automatically.
    private func discoverVoices() -> [(name: String, isProfile: Bool)] {
        var result: [(String, Bool)] = []

        // DSP presets (compiled-in)
        let numPresets = tgsb_get_num_voices()
        for i in 0..<numPresets {
            if let p = tgsb_get_voice_name(Int32(i)) {
                result.append((String(cString: p).capitalized, false))
            }
        }

        // YAML voice profiles (from phonemes.yaml)
        if let eng = engine,
           let namesPtr = tgsb_get_voice_profile_names(eng) {
            let names = String(cString: namesPtr)
            free(namesPtr)
            for name in names.split(separator: "\n") where !name.isEmpty {
                let n = String(name)
                // Skip if already in presets (shouldn't happen, but defensive)
                if !result.contains(where: { $0.0.lowercased() == n.lowercased() }) {
                    result.append((n, true))
                }
            }
        }

        return result
    }

    public override var speechVoices: [AVSpeechSynthesisProviderVoice] {
        get {
            let voiceDefs = discoverVoices()
            var voices: [AVSpeechSynthesisProviderVoice] = []

            for vd in voiceDefs {
                for lang in Self.languageMap {
                    let voice = AVSpeechSynthesisProviderVoice(
                        name: "\(vd.0) (\(lang.bcp47))",
                        identifier: "com.tgspeechbox.\(vd.0.lowercased()).\(lang.bcp47.lowercased())",
                        primaryLanguages: [lang.bcp47],
                        supportedLanguages: [lang.bcp47]
                    )
                    // Infer gender from profile (could add metadata later)
                    voice.gender = .male
                    voices.append(voice)
                }
            }

            return voices
        }
        set { }
    }

    // MARK: - Audio Unit Bus

    public override var outputBusses: AUAudioUnitBusArray {
        return _outputBusses
    }

    public override func allocateRenderResources() throws {
        try super.allocateRenderResources()
    }

    // MARK: - Synthesis

    public override func synthesizeSpeechRequest(
        _ speechRequest: AVSpeechSynthesisProviderRequest
    ) {
        // Claim a generation FIRST — before any setup work — so a cancel
        // arriving from here on is attributed to this utterance and a
        // stale cancel for the previous one can no longer touch us.
        outputMutex.wait()
        requestGen += 1
        let myGen = requestGen
        outputMutex.signal()

        // Extract voice name and language from identifier
        let parts = speechRequest.voice.identifier.split(separator: ".")
        let voiceName = parts.count >= 3 ? String(parts[2]) : "adam"
        let bcp47 = parts.count >= 4 ? String(parts[3]) : "en-us"

        requestCount += 1

        // Force cross-process sync FIRST so both the pause scaling below
        // and the engine settings later see the host app's latest writes.
        // Without this, VoiceOver restart can leave the extension with
        // stale/empty UserDefaults, causing all settings to revert to
        // factory defaults.
        UserDefaults(suiteName: "group.com.tgspeechbox.app")?.synchronize()

        // Pause mode scales VoiceOver's requested <break> durations
        // (DoubleTalk-style): off drops them, short halves them, long
        // honors them as sent. The engine-side punctuation pauses
        // (adv_pauseMode in applyEngineSettings) are a separate feature.
        let udEarly = UserDefaults(suiteName: "group.com.tgspeechbox.app")
        let pauseModeSetting = udEarly?.object(forKey: "adv_pauseMode") != nil
            ? udEarly!.integer(forKey: "adv_pauseMode") : 1  // default: short
        let pauseScalePercent = [0, 50, 100][max(0, min(pauseModeSetting, 2))]

        var segments = extractSegments(
            from: speechRequest.ssmlRepresentation,
            pauseScalePercent: pauseScalePercent)
        let joinedText = segments.map { $0.text }.joined()
        let totalPauseMs = segments.reduce(0) { $0 + $1.pauseAfterMs }
        if joinedText.isEmpty {
            if requestCount == 1 {
                // First request with empty text — likely a voice preview
                // from VoiceOver Settings. Speak a demo so the user can
                // hear the voice.
                segments = [SpeechSegment(
                    text: "Hello, this is \(voiceName.capitalized).",
                    pauseAfterMs: 0)]
            } else if totalPauseMs > 0 {
                // Break-only spacer (VoiceOver's semantic gap, e.g. between
                // an item's name and its hint): render the requested silence
                // at its real, scaled length — this IS the gap the user's
                // pause-mode setting controls. Unlike the one-sample branch
                // below, this can be seconds long, so it needs the same
                // generation guard as the main path: a cancelled spacer must
                // not install dead air in front of the next utterance.
                let n = max(1, totalPauseMs * Int(sampleRate) / 1000)
                outputMutex.wait()
                if myGen != cancelledGen && myGen == requestGen {
                    output = [Float32](repeating: 0, count: n)
                    outputOffset = 0
                }
                outputMutex.signal()
                return
            } else {
                // Empty text during normal use — provide a single silent
                // frame so the render block can signal completion and
                // VoiceOver proceeds to the next utterance (e.g. hint).
                outputMutex.wait()
                output = [0]
                outputOffset = 0
                outputMutex.signal()
                return
            }
        }

        let curVersion = UserDefaults(suiteName: "group.com.tgspeechbox.app")?
            .integer(forKey: "adv_settingsVersion") ?? 0
        let voiceChanged = voiceName != cachedVoice

        guard let eng = engine else {
            // No engine — still need to signal render block to complete
            // so VoiceOver doesn't hang waiting for audio.
            outputMutex.wait()
            output = [0]   // single silent frame
            outputOffset = 0
            outputMutex.signal()
            return
        }

        // Shared UserDefaults for Engine Settings.
        let ud = UserDefaults(suiteName: "group.com.tgspeechbox.app")

        // Lock language: if enabled, use the Speak tab's saved language
        // instead of the VoiceOver request's BCP-47 tag.
        let lockLang = ud?.bool(forKey: "adv_lockLanguage") == true
        let espeakLang: String
        let tgsbLang: String
        if lockLang, let lockedTag = ud?.string(forKey: "tgsb_speak_lang"),
           let locked = Self.languageMap.first(where: { $0.tgsb == lockedTag }) {
            espeakLang = locked.espeak
            tgsbLang = locked.tgsb
        } else {
            let langEntry = Self.languageMap.first {
                $0.bcp47.lowercased() == bcp47.lowercased()
            }
            espeakLang = langEntry?.espeak ?? "en-us"
            tgsbLang = langEntry?.tgsb ?? "en-us"
        }

        let ssml = speechRequest.ssmlRepresentation
        var speed = extractRate(from: ssml)
        let pitch = extractPitch(from: ssml)

        // Apply user rate overrides from Engine Settings.
        if ud?.bool(forKey: "adv_overrideRate") == true {
            let globalRate = ud?.double(forKey: "adv_globalRate") ?? 1.0
            if globalRate > 0.0 { speed = globalRate }
        }
        if ud?.bool(forKey: "rateBoost") == true {
            speed *= 1.35
        }

        // Volume: SSML prosody if present, multiplied by shared app setting
        var vol = Float32(extractVolume(from: ssml))
        let savedVol = UserDefaults(suiteName: "group.com.tgspeechbox.app")?.double(forKey: "systemVolume") ?? 0.0
        if savedVol > 0.0 {
            vol *= Float32(savedVol)
        }

        // Set voice and language identity FIRST — tgsb_set_voice resets
        // voicing tone and tgsb_set_language reloads the pack (resetting
        // pitch mode). Engine settings must be applied AFTER these.
        if voiceName != cachedVoice {
            // Check if this voice name is a YAML profile by querying
            // available profile names from the engine.
            var isProfile = false
            if let namesPtr = tgsb_get_voice_profile_names(eng) {
                let names = String(cString: namesPtr)
                free(namesPtr)
                let profileNames = names.split(separator: "\n").map {
                    String($0).lowercased()
                }
                isProfile = profileNames.contains(voiceName.lowercased())
            }

            if isProfile {
                tgsb_set_voice(eng, "adam")
                tgsb_set_voice_profile(eng, voiceName.capitalized(with: nil))
            } else {
                tgsb_set_voice(eng, voiceName)
            }
            cachedVoice = voiceName
        }
        let languageChanged = espeakLang != cachedEspeakLang || tgsbLang != cachedTgsbLang
        if languageChanged {
            tgsb_set_language(eng, espeakLang, tgsbLang)
            cachedEspeakLang = espeakLang
            cachedTgsbLang = tgsbLang
        }

        // Re-apply engine settings after voice/language identity is set.
        // Language change reloads the pack which resets pitch mode, so
        // settings must also be re-applied after a language change.
        // Settings version bump also forces a language reload to clear
        // stale pack overrides from the in-memory pack.
        let settingsChanged = curVersion != cachedSettingsVersion
        if settingsChanged && !languageChanged {
            // Force pack reload to wipe stale in-memory overrides.
            tgsb_set_language(eng, espeakLang, tgsbLang)
        }
        if settingsChanged || voiceChanged || languageChanged {
            let newRate = Self.loadDspRate()
            if newRate != dspRate {
                tgsb_set_sample_rate(eng, Int32(newRate))
                dspRate = newRate
            }
            applyEngineSettings(eng, voice: voiceName)
            cachedSettingsVersion = curVersion
        }
        // Pack/phoneme/dict overrides persist in the engine across
        // utterances — re-apply only when the host app bumped
        // pack_overrides_version, or when the pack was reloaded above
        // (language change / settings-version reload wipes them).
        // Previously these four ran on EVERY utterance: UserDefaults
        // reads + JSON parses + N sets of tgsb_set_data each.
        let curOverridesVersion = ud?.integer(forKey: "pack_overrides_version") ?? 0
        if curOverridesVersion != cachedOverridesVersion
            || languageChanged || settingsChanged {
            applyPhonemeOverrides()
            applyStoredOverrides(tgsbLang)
            applyDictOverrides(tgsbLang)
            applyDictDisabled(tgsbLang)
            cachedOverridesVersion = curOverridesVersion
        }

        // Queue text segments interleaved with real silence for each
        // (scaled) SSML break. begin_utterance purges stale frames once;
        // a stop that lands mid-loop sticks (queue_text_ex honors it).
        tgsb_begin_utterance(eng)
        for seg in segments {
            if !seg.text.isEmpty {
                tgsb_queue_text_ex(eng, seg.text, speed, pitch, 0)
            }
            if seg.pauseAfterMs > 0 {
                tgsb_queue_silence(eng, Double(seg.pauseAfterMs))
            }
        }

        // Pull PCM and convert Int16 → Float32 using vDSP
        let curDspRate = dspRate
        var samples: [Float32] = []
        samples.reserveCapacity(curDspRate * 2)

        while true {
            let n = Int(tgsb_pull_audio(eng, &pullChunk, Int32(pullChunk.count)))
            if n <= 0 { break }
            let startIdx = samples.count
            samples.append(contentsOf: repeatElement(Float32(0), count: n))
            pullChunk.withUnsafeBufferPointer { srcBuf in
                samples.withUnsafeMutableBufferPointer { dstBuf in
                    vDSP_vflt16(
                        srcBuf.baseAddress!, 1,
                        dstBuf.baseAddress! + startIdx, 1,
                        vDSP_Length(n))
                    var scale = Float32(1.0 / 32768.0)
                    vDSP_vsmul(
                        dstBuf.baseAddress! + startIdx, 1,
                        &scale,
                        dstBuf.baseAddress! + startIdx, 1,
                        vDSP_Length(n))
                }
            }
        }

        // Resample from DSP rate to ASBD rate (22050) if needed
        let asbdRate = Int(sampleRate)
        if curDspRate != asbdRate && !samples.isEmpty {
            samples = resample(samples, from: curDspRate, to: asbdRate)
        }

        // Hand complete buffer to the render block — unless this
        // utterance was cancelled while we were synthesizing (the engine
        // stop aborts the pull loop early, but a cancel that raced with
        // setup may have been wiped by tgsb_queue_text's flag reset; the
        // generation check catches it either way), or a newer request
        // has already claimed the buffer.
        outputMutex.wait()
        if myGen != cancelledGen && myGen == requestGen {
            output = samples
            outputOffset = 0
            volume = vol
        }
        // If suppressed, output stays empty; the next render cycle
        // signals offlineUnitRenderAction_Complete so the host proceeds.
        outputMutex.signal()
    }

    public override func cancelSpeechRequest() {
        if let e = engine { tgsb_stop(e) }

        outputMutex.wait()
        cancelledGen = requestGen
        output.removeAll()
        outputOffset = 0
        outputMutex.signal()
    }

    // MARK: - Audio Render

    public override var internalRenderBlock: AUInternalRenderBlock {
        // Matches eSpeak-NG-mobile's render pattern:
        // blocking semaphore, mDataByteSize = actual sample count,
        // Complete when buffer drained.
        return {
            actionFlags, timestamp, frameCount, outputBusNumber,
            outputAudioBufferList, _, _ in

            let outFrames = UnsafeMutableAudioBufferListPointer(
                outputAudioBufferList)[0].mData!
                .assumingMemoryBound(to: Float32.self)
            let frames = Int(frameCount)

            outFrames.assign(repeating: 0, count: frames)

            self.outputMutex.wait()

            let count = min(self.output.count - self.outputOffset, frames)
            if count > 0 {
                var vol = self.volume
                self.output.withUnsafeBufferPointer { buf in
                    vDSP_vsmul(buf.baseAddress! + self.outputOffset, 1,
                               &vol,
                               outFrames, 1,
                               vDSP_Length(count))
                }
                self.outputOffset += count
            }

            outputAudioBufferList.pointee.mBuffers.mDataByteSize =
                UInt32(count * MemoryLayout<Float32>.size)

            if self.outputOffset >= self.output.count {
                actionFlags.pointee = .offlineUnitRenderAction_Complete
                self.output.removeAll()
                self.outputOffset = 0
            }

            self.outputMutex.signal()
            return noErr
        }
    }

    // MARK: - Engine Settings from AppGroup

    private func applyEngineSettings(_ eng: OpaquePointer, voice: String) {
        let d = UserDefaults(suiteName: "group.com.tgspeechbox.app")

        // Per-voice key with fallback to old global key for migration
        func load(_ key: String, _ dflt: Double) -> Double {
            guard let d = d else { return dflt }
            let voiceKey = "adv_\(key).\(voice)"
            if d.object(forKey: voiceKey) != nil {
                return d.double(forKey: voiceKey)
            }
            let globalKey = "adv_\(key)"
            if d.object(forKey: globalKey) != nil {
                return d.double(forKey: globalKey)
            }
            return dflt
        }

        // VoicingTone: convert 0–100 sliders to engine parameters
        let voiceTilt      = load("voiceTilt", 50)
        let speedQuotient  = load("speedQuotient", 50)
        let aspirationTilt = load("aspirationTilt", 50)
        let cascadeBwScale = load("cascadeBwScale", 50)
        let noiseGlottalMod = load("noiseGlottalMod", 0)
        let pitchSyncF1    = load("pitchSyncF1", 50)
        let pitchSyncB1    = load("pitchSyncB1", 50)
        let voiceTremor    = load("voiceTremor", 0)
        let headSizeSlider = load("headSize", voice == "david" ? 100 : 50)
        let chorusDepthSlider  = load("chorusDepth", 0)
        let chorusDetuneSlider = load("chorusDetuneHz", 33)

        let tilt     = (voiceTilt - 50.0) * (24.0 / 50.0)
        let noiseMod = noiseGlottalMod / 100.0
        let psF1     = (pitchSyncF1 - 50.0) * 1.2
        let psB1     = (pitchSyncB1 - 50.0) * 1.0
        let sq       = speedQuotient <= 50.0
            ? 0.5 + (speedQuotient / 50.0) * 1.5
            : 2.0 + ((speedQuotient - 50.0) / 50.0) * 2.0
        let aspTilt  = (aspirationTilt - 50.0) * 0.24
        let bw       = cascadeBwScale <= 50.0
            ? 2.0 - (cascadeBwScale / 50.0) * 1.0
            : 1.0 - ((cascadeBwScale - 50.0) / 50.0) * 0.7
        let tremor   = (voiceTremor / 100.0) * 0.4
        let hs       = headSizeSlider <= 50.0
            ? 1.25 - (headSizeSlider / 50.0) * 0.25
            : 1.0 - ((headSizeSlider - 50.0) / 50.0) * 0.15
        let chDepth  = chorusDepthSlider / 100.0
        let chDetune = 0.5 + (chorusDetuneSlider / 100.0) * 4.5

        tgsb_set_voicing_tone(eng, tilt, noiseMod, psF1, psB1,
                              sq, aspTilt, bw, tremor,
                              1.0, hs, 1.0,
                              chDepth, chDetune)

        // FrameEx: convert 0–100 sliders to engine parameters
        let creak    = load("creakiness", 0) / 100.0
        let breath   = load("breathiness", 0) / 100.0
        let jit      = load("jitter", 0) / 100.0
        let shim     = load("shimmer", 0) / 100.0
        let sharp    = load("glottalSharpness", 50) / 50.0

        tgsb_set_frame_ex_defaults(eng, creak, breath, jit, shim, sharp)

        // Pitch mode (per-voice with global fallback)
        let mode = d?.string(forKey: "adv_pitchMode.\(voice)")
            ?? d?.string(forKey: "adv_pitchMode")
            ?? "espeak_style"
        tgsb_set_pitch_mode(eng, mode)

        let inflScale = load("inflectionScale", 58) / 100.0
        tgsb_set_legacy_pitch_inflection_scale(eng, inflScale)

        let infl = load("inflection", 50) / 100.0
        tgsb_set_inflection(eng, infl)

        // Pause mode stays global (not voice-specific)
        let pauseMode = d?.object(forKey: "adv_pauseMode") != nil
            ? d!.integer(forKey: "adv_pauseMode") : 1  // default: short
        tgsb_set_pause_mode(eng, Int32(pauseMode))
    }

    // MARK: - Pack setting overrides from AppGroup

    private func applyStoredOverrides(_ tgsbLang: String) {
        guard let eng = engine else { return }
        let d = UserDefaults(suiteName: "group.com.tgspeechbox.app")
        guard let json = d?.string(forKey: "pack_overrides_\(tgsbLang)"),
              let data = json.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: String],
              !obj.isEmpty
        else { return }
        for (k, v) in obj {
            tgsb_set_data(eng, TGSB_DATA_SETTINGS, tgsbLang, k, v)
        }
    }

    private func applyDictOverrides(_ tgsbLang: String) {
        guard let eng = engine else { return }
        let d = UserDefaults(suiteName: "group.com.tgspeechbox.app")
        guard let json = d?.string(forKey: "dict_overrides_\(tgsbLang)"),
              let data = json.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: String],
              !obj.isEmpty
        else { return }
        for (k, v) in obj {
            tgsb_set_data(eng, TGSB_DATA_DICTIONARY, tgsbLang, k, v)
        }
    }

    private func applyDictDisabled(_ tgsbLang: String) {
        guard let eng = engine else { return }
        let d = UserDefaults(suiteName: "group.com.tgspeechbox.app")
        guard let json = d?.string(forKey: "dict_disabled_\(tgsbLang)"),
              let data = json.data(using: .utf8),
              let arr = try? JSONSerialization.jsonObject(with: data) as? [String],
              !arr.isEmpty
        else { return }
        for type in arr {
            tgsb_set_data(eng, TGSB_DATA_DICTIONARY, "config:\(tgsbLang)", type, "false")
        }
    }

    private func applyPhonemeOverrides() {
        guard let eng = engine else { return }
        let d = UserDefaults(suiteName: "group.com.tgspeechbox.app")
        guard let json = d?.string(forKey: "phoneme_overrides"),
              let data = json.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: String],
              !obj.isEmpty
        else { return }
        for (k, v) in obj {
            tgsb_set_data(eng, TGSB_DATA_PHONEMES, "", k, v)
        }
    }

    // MARK: - Resampling

    /// Linear-interpolation resample from one rate to another.
    private func resample(_ input: [Float32], from srcRate: Int, to dstRate: Int) -> [Float32] {
        guard let srcFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                         sampleRate: Double(srcRate),
                                         channels: 1, interleaved: false),
              let dstFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                         sampleRate: Double(dstRate),
                                         channels: 1, interleaved: false),
              let converter = AVAudioConverter(from: srcFmt, to: dstFmt)
        else { return input }

        let frameCount = AVAudioFrameCount(input.count)
        guard let srcBuf = AVAudioPCMBuffer(pcmFormat: srcFmt,
                                             frameCapacity: frameCount)
        else { return input }
        srcBuf.frameLength = frameCount
        memcpy(srcBuf.floatChannelData![0], input,
               input.count * MemoryLayout<Float32>.size)

        let ratio = Double(dstRate) / Double(srcRate)
        // Extra capacity for sinc filter tail flushed on endOfStream
        let outFrames = AVAudioFrameCount(ceil(Double(input.count) * ratio)) + 256
        guard let dstBuf = AVAudioPCMBuffer(pcmFormat: dstFmt,
                                             frameCapacity: outFrames)
        else { return input }

        var error: NSError?
        var consumed = false
        converter.convert(to: dstBuf, error: &error) { _, outStatus in
            if consumed {
                outStatus.pointee = .endOfStream
                return nil
            }
            consumed = true
            outStatus.pointee = .haveData
            return srcBuf
        }
        if error != nil { return input }

        let count = Int(dstBuf.frameLength)
        return Array(UnsafeBufferPointer(start: dstBuf.floatChannelData![0],
                                         count: count))
    }

    // MARK: - Helpers

    private struct SpeechSegment {
        var text: String
        var pauseAfterMs: Int
    }

    // SSML break strength → milliseconds (SSML spec default is "medium").
    private static let breakStrengths: [String: Int] = [
        "none": 0, "x-weak": 100, "weak": 200,
        "medium": 350, "strong": 500, "x-strong": 800,
    ]
    private static let maxBreakMs = 2000

    private func firstCapture(_ pattern: String, in s: String) -> String? {
        guard let re = try? NSRegularExpression(pattern: pattern,
                                                options: [.caseInsensitive]),
              let m = re.firstMatch(in: s,
                                    range: NSRange(s.startIndex..., in: s)),
              m.numberOfRanges > 1,
              let r = Range(m.range(at: 1), in: s) else { return nil }
        return String(s[r])
    }

    // DoubleTalk-style pause scaling: 0% drops a break entirely; any
    // nonzero result is floored at 30 ms so an aggressive setting
    // compresses semantic boundaries instead of erasing them.
    private func scaledBreakMs(fromTag tag: String, scalePercent: Int) -> Int {
        var ms = Self.breakStrengths["medium"]!
        if let v = firstCapture(#"time\s*=\s*"([^"]+)""#, in: tag)?
            .lowercased().trimmingCharacters(in: .whitespaces) {
            if v.hasSuffix("ms"),
               let d = Double(v.dropLast(2)
                   .trimmingCharacters(in: .whitespaces)) {
                ms = Int(d.rounded())
            } else if v.hasSuffix("s"),
                      let d = Double(v.dropLast(1)
                          .trimmingCharacters(in: .whitespaces)) {
                ms = Int((d * 1000).rounded())
            }
        } else if let v = firstCapture(#"strength\s*=\s*"([^"]+)""#, in: tag)?
            .lowercased(), let s = Self.breakStrengths[v] {
            ms = s
        }
        ms = max(0, min(ms, Self.maxBreakMs))
        let pct = max(0, min(scalePercent, 200))
        guard pct > 0, ms > 0 else { return 0 }
        return min(max(ms * pct / 100, min(ms, 30)), Self.maxBreakMs)
    }

    // Split the SSML on <break> tags so each break renders as real
    // silence at its (scaled) requested length. The old behavior
    // replaced breaks with ". ", which both collapsed every VoiceOver
    // pause into a 35-60 ms clause gap — making the pause-mode setting
    // inaudible — and forced a spurious sentence-final intonation
    // contour mid-utterance.
    private func extractSegments(from ssml: String,
                                 pauseScalePercent: Int) -> [SpeechSegment] {
        guard let re = try? NSRegularExpression(
            pattern: #"<break\b[^>]*/?\s*>"#, options: [.caseInsensitive])
        else {
            return [SpeechSegment(text: extractPlainText(from: ssml),
                                  pauseAfterMs: 0)]
        }
        let ns = ssml as NSString
        var out: [SpeechSegment] = []
        var cursor = 0
        for m in re.matches(in: ssml,
                            range: NSRange(location: 0, length: ns.length)) {
            let text = ns.substring(
                with: NSRange(location: cursor,
                              length: m.range.location - cursor))
            out.append(SpeechSegment(
                text: extractPlainText(from: text),
                pauseAfterMs: scaledBreakMs(
                    fromTag: ns.substring(with: m.range),
                    scalePercent: pauseScalePercent)))
            cursor = m.range.location + m.range.length
        }
        out.append(SpeechSegment(
            text: extractPlainText(from: ns.substring(from: cursor)),
            pauseAfterMs: 0))
        return out
    }

    private func extractPlainText(from ssml: String) -> String {
        // Strip SSML tags. (<break> tags are consumed by extractSegments
        // before this runs; a stray one here just becomes a space.)
        var text = ssml.replacingOccurrences(of: "<[^>]+>", with: " ",
                                          options: .regularExpression)
        text = text.replacingOccurrences(of: "&apos;", with: "'")
        text = text.replacingOccurrences(of: "&quot;", with: "\"")
        text = text.replacingOccurrences(of: "&amp;",  with: "&")
        text = text.replacingOccurrences(of: "&lt;",   with: "<")
        text = text.replacingOccurrences(of: "&gt;",   with: ">")
        text = text.replacingOccurrences(of: "&#39;",  with: "'")
        text = text.replacingOccurrences(of: "&#34;",  with: "\"")
        text = text.replacingOccurrences(of: "\\s+", with: " ",
                                          options: .regularExpression)
        return text.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private func extractRate(from ssml: String) -> Double {
        guard let match = ssml.range(
            of: #"<prosody[^>]*\brate="([^"]+)""#,
            options: .regularExpression
        ) else { return 1.0 }

        let tag = String(ssml[match])
        guard let valRange = tag.range(
            of: #"rate="([^"]+)""#, options: .regularExpression
        ) else { return 1.0 }

        var val = String(tag[valRange])
            .replacingOccurrences(of: "rate=\"", with: "")
            .replacingOccurrences(of: "\"", with: "")
            .trimmingCharacters(in: .whitespaces)

        switch val {
        case "x-slow":  return 0.3
        case "slow":    return 0.6
        case "medium":  return 1.0
        case "fast":    return 2.0
        case "x-fast":  return 3.5
        default: break
        }

        if val.hasSuffix("%") {
            val.removeLast()
            if let pct = Double(val) { return max(0.1, pct / 100.0) }
        }
        if let num = Double(val) { return max(0.1, num) }
        return 1.0
    }

    private func extractPitch(from ssml: String) -> Double {
        let defaultPitch = 110.0
        guard let match = ssml.range(
            of: #"<prosody[^>]*\bpitch="([^"]+)""#,
            options: .regularExpression
        ) else { return defaultPitch }

        let tag = String(ssml[match])
        guard let valRange = tag.range(
            of: #"pitch="([^"]+)""#, options: .regularExpression
        ) else { return defaultPitch }

        var val = String(tag[valRange])
            .replacingOccurrences(of: "pitch=\"", with: "")
            .replacingOccurrences(of: "\"", with: "")
            .trimmingCharacters(in: .whitespaces)

        switch val {
        case "x-low":  return 70.0
        case "low":    return 90.0
        case "medium": return 120.0
        case "high":   return 160.0
        case "x-high": return 200.0
        default: break
        }

        if val.lowercased().hasSuffix("hz") {
            val = String(val.dropLast(2))
            if let hz = Double(val) { return max(40.0, min(hz, 500.0)) }
        }
        if val.hasSuffix("%") {
            val.removeLast()
            if let pct = Double(val) {
                return max(40.0, min(defaultPitch * (1.0 + pct / 100.0), 500.0))
            }
        }
        if let num = Double(val) { return max(40.0, min(num, 500.0)) }
        return defaultPitch
    }

    private func extractVolume(from ssml: String) -> Double {
        guard let match = ssml.range(
            of: #"<prosody[^>]*\bvolume="([^"]+)""#,
            options: .regularExpression
        ) else { return 1.0 }

        let tag = String(ssml[match])
        guard let valRange = tag.range(
            of: #"volume="([^"]+)""#, options: .regularExpression
        ) else { return 1.0 }

        var val = String(tag[valRange])
            .replacingOccurrences(of: "volume=\"", with: "")
            .replacingOccurrences(of: "\"", with: "")
            .trimmingCharacters(in: .whitespaces)

        switch val {
        case "silent": return 0.0
        case "x-soft": return 0.25
        case "soft":   return 0.5
        case "medium": return 1.0
        case "loud":   return 1.5
        case "x-loud": return 2.0
        default: break
        }

        if val.hasSuffix("%") {
            val.removeLast()
            if let pct = Double(val) { return max(0.0, min(pct / 100.0, 2.0)) }
        }
        if let num = Double(val) { return max(0.0, min(num, 2.0)) }
        return 1.0
    }
}
