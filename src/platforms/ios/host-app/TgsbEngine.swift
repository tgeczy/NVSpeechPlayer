/*
 * TgsbEngine.swift — Swift wrapper around the C bridge.
 *
 * Manages the TGSpeechBox pipeline lifecycle and audio playback
 * via AVAudioPlayer (output-only, no microphone permission needed).
 */

import AVFoundation

/// Supported languages with their eSpeak and TGSB language tags.
struct TgsbLanguage: Identifiable, Hashable {
    let id: String          // display key, e.g. "en-us"
    let displayName: String
    let espeakTag: String
    let tgsbTag: String
}

let kLanguages: [TgsbLanguage] = [
    TgsbLanguage(id: "en-us", displayName: "English (US)",      espeakTag: "en-us", tgsbTag: "en-us"),
    TgsbLanguage(id: "en-gb", displayName: "English (GB)",      espeakTag: "en-gb", tgsbTag: "en-gb"),
    TgsbLanguage(id: "en-au", displayName: "English (AU)",      espeakTag: "en",    tgsbTag: "en-au"),
    TgsbLanguage(id: "en-ca", displayName: "English (CA)",      espeakTag: "en-us", tgsbTag: "en-ca"),
    TgsbLanguage(id: "fr",    displayName: "French",            espeakTag: "fr",    tgsbTag: "fr"),
    TgsbLanguage(id: "es",    displayName: "Spanish",           espeakTag: "es",    tgsbTag: "es"),
    TgsbLanguage(id: "es-mx", displayName: "Spanish (Mexico)",  espeakTag: "es-419",tgsbTag: "es-mx"),
    TgsbLanguage(id: "it",    displayName: "Italian",           espeakTag: "it",    tgsbTag: "it"),
    TgsbLanguage(id: "pt-br", displayName: "Portuguese (BR)",   espeakTag: "pt-br", tgsbTag: "pt-br"),
    TgsbLanguage(id: "pt",    displayName: "Portuguese (PT)",   espeakTag: "pt",    tgsbTag: "pt"),
    TgsbLanguage(id: "ro",    displayName: "Romanian",          espeakTag: "ro",    tgsbTag: "ro"),
    TgsbLanguage(id: "de",    displayName: "German",            espeakTag: "de",    tgsbTag: "de"),
    TgsbLanguage(id: "nl",    displayName: "Dutch",             espeakTag: "nl",    tgsbTag: "nl"),
    TgsbLanguage(id: "da",    displayName: "Danish",            espeakTag: "da",    tgsbTag: "da"),
    TgsbLanguage(id: "sv",    displayName: "Swedish",           espeakTag: "sv",    tgsbTag: "sv"),
    TgsbLanguage(id: "pl",    displayName: "Polish",            espeakTag: "pl",    tgsbTag: "pl"),
    TgsbLanguage(id: "cs",    displayName: "Czech",             espeakTag: "cs",    tgsbTag: "cs"),
    TgsbLanguage(id: "sk",    displayName: "Slovak",            espeakTag: "sk",    tgsbTag: "sk"),
    TgsbLanguage(id: "bg",    displayName: "Bulgarian",         espeakTag: "bg",    tgsbTag: "bg"),
    TgsbLanguage(id: "hr",    displayName: "Croatian",          espeakTag: "hr",    tgsbTag: "hr"),
    TgsbLanguage(id: "ru",    displayName: "Russian",           espeakTag: "ru",    tgsbTag: "ru"),
    TgsbLanguage(id: "uk",    displayName: "Ukrainian",         espeakTag: "uk",    tgsbTag: "uk"),
    TgsbLanguage(id: "hu",    displayName: "Hungarian",         espeakTag: "hu",    tgsbTag: "hu"),
    TgsbLanguage(id: "fi",    displayName: "Finnish",           espeakTag: "fi",    tgsbTag: "fi"),
    TgsbLanguage(id: "tr",    displayName: "Turkish",           espeakTag: "tr",    tgsbTag: "tr"),
    TgsbLanguage(id: "zh",    displayName: "Chinese (Mandarin)",espeakTag: "cmn",   tgsbTag: "zh"),
]

/// Voice preset names (from C bridge + YAML voice profiles).
struct TgsbVoice: Identifiable, Hashable {
    let id: String
    let displayName: String
    let isProfile: Bool  // true = YAML voice profile (Beth, Bobby)

    init(id: String, displayName: String, isProfile: Bool = false) {
        self.id = id
        self.displayName = displayName
        self.isProfile = isProfile
    }
}

@MainActor
class TgsbEngine: ObservableObject {
    @Published var isSpeaking = false
    @Published var selectedLanguage: TgsbLanguage {
        didSet {
            UserDefaults(suiteName: kAppGroupId)?
                .set(selectedLanguage.tgsbTag, forKey: "tgsb_speak_lang")
        }
    }
    @Published var selectedVoice: TgsbVoice
    @Published var speed: Double = 1.0
    @Published var rateBoost: Bool = UserDefaults(suiteName: kAppGroupId)?
        .bool(forKey: "rateBoost") ?? false {
        didSet {
            UserDefaults(suiteName: kAppGroupId)?.set(rateBoost, forKey: "rateBoost")
        }
    }
    @Published var pitch: Double = 110.0
    @Published var inflectionValue: Double = 50.0  // 0–100 slider

    @Published var voices: [TgsbVoice]

    private var engine: OpaquePointer?
    private var sampleRate: Int

    private static func loadSampleRate() -> Int {
        let d = UserDefaults(suiteName: kAppGroupId)
        let valid = [11025, 16000, 22050, 44100]
        if let d = d, d.object(forKey: "adv_sampleRate") != nil {
            let saved = d.integer(forKey: "adv_sampleRate")
            if valid.contains(saved) { return saved }
        }
        return 22050
    }

    // Audio playback (AVAudioPlayer — output only, no mic permission)
    private var audioPlayer: AVAudioPlayer?
    private var synthQueue = DispatchQueue(label: "com.tgspeechbox.synth",
                                           qos: .userInitiated)

    init() {
        self.sampleRate = Self.loadSampleRate()

        // Gather voice names: DSP presets from C bridge + YAML profiles
        let numVoices = tgsb_get_num_voices()
        var v: [TgsbVoice] = []
        for i in 0..<numVoices {
            if let namePtr = tgsb_get_voice_name(Int32(i)) {
                let name = String(cString: namePtr)
                v.append(TgsbVoice(id: name,
                                   displayName: name.capitalized))
            }
        }
        // Add known YAML voice profiles as fallback (replaced by
        // dynamic discovery in start() if engine finds more).
        v.append(TgsbVoice(id: "beth", displayName: "Beth", isProfile: true))
        v.append(TgsbVoice(id: "bobby", displayName: "Bobby", isProfile: true))
        self.voices = v
        self.selectedVoice = v.first ?? TgsbVoice(id: "adam",
                                                   displayName: "Adam")
        // Restore saved Speak-tab language, fall back to en-us.
        if let savedTag = UserDefaults(suiteName: kAppGroupId)?
                            .string(forKey: "tgsb_speak_lang"),
           let match = kLanguages.first(where: { $0.tgsbTag == savedTag }) {
            self.selectedLanguage = match
        } else {
            self.selectedLanguage = kLanguages[0] // en-us
        }
    }

    func start() -> Bool {
        guard engine == nil else { return true }

        guard let espeakDataDir = Self.resourcePath(for: "espeak-ng-data"),
              let packDir = Self.resourcePath(for: "packs") else {
            print("[TgsbEngine] Runtime data not found in bundle")
            return false
        }

        // espeak_Initialize accepts the espeak-ng-data directory directly
        print("[TgsbEngine] espeakData: \(espeakDataDir)")
        print("[TgsbEngine] packDir: \(packDir)")

        engine = tgsb_create(espeakDataDir, packDir, Int32(sampleRate))
        guard engine != nil else {
            print("[TgsbEngine] tgsb_create failed")
            return false
        }

        // Point override directory at app group container so user-imported
        // language packs shadow the built-in ones.
        if let containerURL = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: kAppGroupId) {
            let overrideDir = containerURL.path
            tgsb_set_override_directory(engine, overrideDir)
            print("[TgsbEngine] overrideDir: \(overrideDir)")
        }

        // Set initial language and voice
        tgsb_set_language(engine,
                          selectedLanguage.espeakTag,
                          selectedLanguage.tgsbTag)
        // Discover YAML voice profiles now that engine is available
        if let namesPtr = tgsb_get_voice_profile_names(engine!) {
            let names = String(cString: namesPtr)
            free(namesPtr)
            var updatedVoices = self.voices
            for name in names.split(separator: "\n") where !name.isEmpty {
                let n = String(name)
                if !updatedVoices.contains(where: { $0.id == n.lowercased() }) {
                    updatedVoices.append(TgsbVoice(id: n.lowercased(),
                                                    displayName: n,
                                                    isProfile: true))
                }
            }
            self.voices = updatedVoices
        }

        applySelectedVoice(engine!)

        print("[TgsbEngine] Engine ready")
        return true
    }

    /// Apply the selected voice — either a DSP preset or a YAML profile.
    private func applySelectedVoice(_ eng: OpaquePointer) {
        if selectedVoice.isProfile {
            tgsb_set_voice(eng, "adam")
            tgsb_set_voice_profile(eng, selectedVoice.displayName)
        } else {
            tgsb_set_voice(eng, selectedVoice.id)
        }
    }

    func shutdown() {
        stopSpeaking()
        if let e = engine {
            tgsb_destroy(e)
            engine = nil
        }
    }

    /// Change DSP sample rate without tearing down the whole engine.
    func changeSampleRate(_ rate: Int) {
        sampleRate = rate
        if let eng = engine {
            tgsb_set_sample_rate(eng, Int32(rate))
        }
    }

    // MARK: - Engine settings (voice quality sliders)

    /// Apply VoicingTone from 0–100 slider values.
    func applyVoicingToneFromSliders(
        voiceTilt: Double, speedQuotient sq: Double,
        aspirationTilt: Double, cascadeBwScale bw: Double,
        noiseGlottalMod: Double, pitchSyncF1: Double,
        pitchSyncB1: Double, voiceTremor: Double,
        headSize: Double,
        chorusDepth: Double, chorusDetuneHz: Double
    ) {
        guard let eng = engine else { return }

        let tilt     = (voiceTilt - 50.0) * (24.0 / 50.0)
        let noiseMod = noiseGlottalMod / 100.0
        let psF1     = (pitchSyncF1 - 50.0) * 1.2
        let psB1     = (pitchSyncB1 - 50.0) * 1.0
        let sqVal    = sq <= 50.0
            ? 0.5 + (sq / 50.0) * 1.5
            : 2.0 + ((sq - 50.0) / 50.0) * 2.0
        let aspTilt  = (aspirationTilt - 50.0) * 0.24
        let bwVal    = bw <= 50.0
            ? 2.0 - (bw / 50.0) * 1.0
            : 1.0 - ((bw - 50.0) / 50.0) * 0.7
        let tremor   = (voiceTremor / 100.0) * 0.4
        let hs       = headSize <= 50.0
            ? 1.25 - (headSize / 50.0) * 0.25
            : 1.0 - ((headSize - 50.0) / 50.0) * 0.15
        let chDepth  = chorusDepth / 100.0
        let chDetune = 0.5 + (chorusDetuneHz / 100.0) * 4.5

        tgsb_set_voicing_tone(eng, tilt, noiseMod, psF1, psB1,
                              sqVal, aspTilt, bwVal, tremor,
                              1.0, hs, 1.0,
                              chDepth, chDetune)
    }

    /// Apply FrameEx defaults from 0–100 slider values.
    func applyFrameExFromSliders(
        creakiness: Double, breathiness: Double,
        jitter: Double, shimmer: Double,
        glottalSharpness: Double
    ) {
        guard let eng = engine else { return }
        tgsb_set_frame_ex_defaults(eng,
            creakiness / 100.0,
            breathiness / 100.0,
            jitter / 100.0,
            shimmer / 100.0,
            glottalSharpness / 50.0)
    }

    /// Set pitch intonation model.
    func setPitchMode(_ mode: String) {
        guard let eng = engine else { return }
        tgsb_set_pitch_mode(eng, mode)
    }

    /// Set legacy pitch inflection scale (0.0–2.0).
    func setInflectionScale(_ scale: Double) {
        guard let eng = engine else { return }
        tgsb_set_legacy_pitch_inflection_scale(eng, scale)
    }

    /// Set voice inflection / pitch range (0.0–1.0).
    func setInflection(_ value: Double) {
        guard let eng = engine else { return }
        tgsb_set_inflection(eng, value)
    }

    /// Set pause mode (0=off, 1=short, 2=long).
    func setPauseMode(_ mode: Int) {
        guard let eng = engine else { return }
        tgsb_set_pause_mode(eng, Int32(mode))
    }

    /// Read all adv_* settings from AppGroup UserDefaults and apply.
    func applyEngineSettings() {
        guard engine != nil else { return }
        let d = UserDefaults(suiteName: kAppGroupId)
        let voice = d?.string(forKey: "adv_selectedVoice") ?? "adam"

        func load(_ key: String, _ dflt: Double) -> Double {
            guard let d = d else { return dflt }
            // Try per-voice key first, then global fallback.
            let voiceKey = "adv_\(key).\(voice)"
            if d.object(forKey: voiceKey) != nil { return d.double(forKey: voiceKey) }
            let globalKey = "adv_\(key)"
            if d.object(forKey: globalKey) != nil { return d.double(forKey: globalKey) }
            return dflt
        }

        applyVoicingToneFromSliders(
            voiceTilt: load("voiceTilt", 50),
            speedQuotient: load("speedQuotient", 50),
            aspirationTilt: load("aspirationTilt", 50),
            cascadeBwScale: load("cascadeBwScale", 50),
            noiseGlottalMod: load("noiseGlottalMod", 0),
            pitchSyncF1: load("pitchSyncF1", 50),
            pitchSyncB1: load("pitchSyncB1", 50),
            voiceTremor: load("voiceTremor", 0),
            headSize: load("headSize", 50),
            chorusDepth: load("chorusDepth", 0),
            chorusDetuneHz: load("chorusDetuneHz", 33))

        applyFrameExFromSliders(
            creakiness: load("creakiness", 0),
            breathiness: load("breathiness", 0),
            jitter: load("jitter", 0),
            shimmer: load("shimmer", 0),
            glottalSharpness: load("glottalSharpness", 50))

        let mode = d?.string(forKey: "adv_pitchMode") ?? "espeak_style"
        setPitchMode(mode)
        setInflectionScale(load("inflectionScale", 58) / 100.0)
        setInflection(load("inflection", 50) / 100.0)

        let pauseMode = d?.object(forKey: "adv_pauseMode") != nil
            ? d!.integer(forKey: "adv_pauseMode") : 1  // default: short
        setPauseMode(pauseMode)
    }

    // MARK: - Pack settings editor

    /// Settings managed by Engine Settings sliders — hidden from editor.
    private static let hiddenKeys: Set<String> = [
        "legacyPitchMode", "legacyPitchInflectionScale"
    ]

    enum SettingType { case bool_, number, text }

    struct PackSetting: Identifiable {
        let id: String  // key
        let key: String
        let displayName: String
        let value: String
        let isOverridden: Bool
        let type: SettingType
        let options: [String]?  // dropdown options for enum-like strings
    }

    @Published var editorLanguages: [String] = []
    @Published var editorSettings: [PackSetting] = []
    private var editorLangTag: String = ""

    func loadEditorLanguages() {
        guard let eng = engine else { return }
        guard let ptr = tgsb_get_available_languages(eng) else { return }
        let raw = String(cString: ptr)
        tgsb_free_string(ptr)
        editorLanguages = raw.split(separator: "\n").map(String.init).sorted()
    }

    func loadEditorSettings(langTag: String) {
        guard let eng = engine else { return }
        editorLangTag = langTag

        // Query settings directly by langTag — no temp language switch needed.
        guard let ptr = tgsb_query_data(eng, TGSB_DATA_SETTINGS, langTag, 0, 0) else { return }
        let jsonStr = String(cString: ptr)
        tgsb_free_string(ptr)

        let overrides = loadOverrides(langTag)

        guard let data = jsonStr.data(using: .utf8),
              let arr = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]]
        else { return }

        var settings: [PackSetting] = []
        for obj in arr {
            guard let key = obj["key"] as? String else { continue }
            if Self.hiddenKeys.contains(key) { continue }
            let jsonType = obj["type"] as? String ?? "string"
            let baseValue: String
            if jsonType == "bool", let b = obj["value"] as? Bool {
                baseValue = b ? "true" : "false"
            } else {
                baseValue = "\(obj["value"] ?? "")"
            }
            let effectiveValue = overrides[key] ?? baseValue
            let type: SettingType = jsonType == "bool" ? .bool_ :
                                    jsonType == "float" ? .number : .text
            let options = (obj["options"] as? [String])
            settings.append(PackSetting(
                id: key, key: key,
                displayName: camelToDisplay(key),
                value: effectiveValue,
                isOverridden: overrides[key] != nil,
                type: type,
                options: options))
        }
        editorSettings = settings

        // Apply overrides to the active language if it matches.
        if !overrides.isEmpty {
            for (k, v) in overrides {
                tgsb_set_data(eng, TGSB_DATA_SETTINGS, langTag, k, v)
            }
        }
    }

    func setEditorOverride(langTag: String, key: String, value: String) {
        let baseValues = getBaseValues(langTag)
        var overrides = loadOverrides(langTag)
        if baseValues[key] == value {
            overrides.removeValue(forKey: key)
        } else {
            overrides[key] = value
        }
        saveOverrides(langTag, overrides)
        reloadCurrentLanguage()
        loadEditorSettings(langTag: langTag)
    }

    func removeEditorOverride(langTag: String, key: String) {
        var overrides = loadOverrides(langTag)
        overrides.removeValue(forKey: key)
        saveOverrides(langTag, overrides)
        reloadCurrentLanguage()
        loadEditorSettings(langTag: langTag)
    }

    func resetAllEditorOverrides(langTag: String) {
        let d = UserDefaults(suiteName: kAppGroupId)
        d?.removeObject(forKey: "pack_overrides_\(langTag)")
        // Bump version so AU extension reloads pack.
        let ver = (d?.integer(forKey: "adv_settingsVersion") ?? 0) + 1
        d?.set(ver, forKey: "adv_settingsVersion")
        d?.synchronize()
        reloadCurrentLanguage()
        loadEditorSettings(langTag: langTag)
    }

    // MARK: - Pack Import / Export

    /// Return merged YAML content for a language pack (base + overrides).
    func packYamlContent(langTag: String) -> String? {
        guard let eng = engine else { return nil }
        let overrides = loadOverrides(langTag)
        let json: String
        if overrides.isEmpty {
            json = "{}"
        } else if let data = try? JSONSerialization.data(withJSONObject: overrides),
                  let s = String(data: data, encoding: .utf8) {
            json = s
        } else {
            json = "{}"
        }
        guard let ptr = tgsb_export_data(eng, TGSB_DATA_SETTINGS, langTag, json) else { return nil }
        let result = String(cString: ptr)
        free(ptr)
        return result
    }

    /// Write pack YAML to a temp file for sharing via share sheet.
    func exportPackToTempFile(langTag: String) -> URL? {
        guard let content = packYamlContent(langTag: langTag) else { return nil }
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("\(langTag).yaml")
        guard let _ = try? content.write(to: url, atomically: true, encoding: .utf8) else {
            return nil
        }
        return url
    }

    /// Import a YAML file's settings into the language pack for `langTag`.
    /// Only the settings: block is imported as per-key overrides —
    /// normalization rules, allophone rules, etc. are skipped.
    /// Returns a status message string.
    func importPackYaml(langTag: String, from url: URL) -> String {
        let accessing = url.startAccessingSecurityScopedResource()
        defer { if accessing { url.stopAccessingSecurityScopedResource() } }

        guard let content = try? String(contentsOf: url, encoding: .utf8) else {
            return "Could not read file"
        }
        if content.isEmpty { return "File is empty" }

        // Reject phonemes files.
        if content.contains("_isVowel:") && content.contains("_isNasal:") {
            return "This looks like a phonemes file, not a language pack."
        }

        let settings = extractSettingsFromYaml(content)
        if settings.isEmpty {
            return "No settings found in file"
        }

        // Clear existing overrides and apply imported settings.
        guard let eng = engine else { return "Engine not started" }
        let d = UserDefaults(suiteName: kAppGroupId)
        d?.removeObject(forKey: "pack_overrides_\(langTag)")
        d?.synchronize()

        for (key, value) in settings {
            tgsb_set_data(eng, TGSB_DATA_SETTINGS, langTag, key, value)
        }
        saveOverrides(langTag: langTag, overrides: settings)

        reloadCurrentLanguage()
        loadEditorSettings(langTag: langTag)
        return "Imported \(settings.count) settings into \(langTag)"
    }

    /// Parse a YAML file and extract the settings: block as flat dot-notation
    /// key-value pairs. Handles up to 3 levels of nesting.
    private func extractSettingsFromYaml(_ yaml: String) -> [String: String] {
        var result: [String: String] = [:]
        let lines = yaml.components(separatedBy: .newlines)
        var inSettings = false
        var keyStack: [(Int, String)] = []  // (indent, key)

        for line in lines {
            let trimmed = line.replacingOccurrences(of: "\\s+$", with: "", options: .regularExpression)
            if trimmed.isEmpty || trimmed.trimmingCharacters(in: .whitespaces).hasPrefix("#") { continue }

            let stripped = trimmed.drop(while: { $0 == " " })
            let indent = trimmed.count - stripped.count

            // Detect top-level blocks.
            if indent == 0 && trimmed.contains(":") {
                let topKey = String(trimmed.prefix(while: { $0 != ":" })).trimmingCharacters(in: .whitespaces)
                inSettings = (topKey == "settings")
                keyStack.removeAll()
                continue
            }

            if !inSettings { continue }

            guard let colonRange = trimmed.range(of: ":") else { continue }
            let colonPos = trimmed.distance(from: trimmed.startIndex, to: colonRange.lowerBound)

            var key = String(trimmed[trimmed.index(trimmed.startIndex, offsetBy: indent)..<colonRange.lowerBound])
                .trimmingCharacters(in: .whitespaces)
            // Unquote.
            if key.hasPrefix("\"") && key.hasSuffix("\"") && key.count >= 2 {
                key = String(key.dropFirst().dropLast())
            }

            var afterColon = String(trimmed[trimmed.index(after: colonRange.lowerBound)...])
                .trimmingCharacters(in: .whitespaces)
            // Strip inline comments.
            if let hashRange = afterColon.range(of: " #") {
                afterColon = String(afterColon[..<hashRange.lowerBound])
                    .trimmingCharacters(in: .whitespaces)
            }

            // Pop keyStack to current indent level.
            while let last = keyStack.last, last.0 >= indent {
                keyStack.removeLast()
            }

            if afterColon.isEmpty {
                // Parent key — push to stack.
                keyStack.append((indent, key))
            } else {
                // Leaf value — build dot-notation key.
                let prefix = keyStack.map { $0.1 }.joined(separator: ".")
                let fullKey = prefix.isEmpty ? key : "\(prefix).\(key)"
                var value = afterColon
                if value.hasPrefix("\"") && value.hasSuffix("\"") && value.count >= 2 {
                    value = String(value.dropFirst().dropLast())
                }
                result[fullKey] = value
            }
        }
        return result
    }

    /// Save pack overrides for a language tag.
    private func saveOverrides(langTag: String, overrides: [String: String]) {
        let d = UserDefaults(suiteName: kAppGroupId)
        if overrides.isEmpty {
            d?.removeObject(forKey: "pack_overrides_\(langTag)")
        } else if let data = try? JSONSerialization.data(withJSONObject: overrides),
                  let json = String(data: data, encoding: .utf8) {
            d?.set(json, forKey: "pack_overrides_\(langTag)")
        }
        let ver = (d?.integer(forKey: "adv_settingsVersion") ?? 0) + 1
        d?.set(ver, forKey: "adv_settingsVersion")
        d?.synchronize()
    }

    /// Path to the pack YAML file — checks app group override first, falls back to bundle.
    private func packFilePath(langTag: String) -> String? {
        // Check writable app group location first (imported packs).
        if let containerURL = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: kAppGroupId) {
            let overridePath = containerURL
                .appendingPathComponent("packs/lang/\(langTag).yaml").path
            if FileManager.default.fileExists(atPath: overridePath) {
                return overridePath
            }
        }
        // Fall back to bundle.
        if let bundlePath = Bundle.main.path(forResource: "packs/lang/\(langTag)",
                                              ofType: "yaml") {
            return bundlePath
        }
        return nil
    }

    func applyStoredOverrides(_ tgsbLang: String) {
        guard let eng = engine else { return }
        let overrides = loadOverrides(tgsbLang)
        if overrides.isEmpty { return }
        for (k, v) in overrides {
            tgsb_set_data(eng, TGSB_DATA_SETTINGS, tgsbLang, k, v)
        }
    }

    private func reloadCurrentLanguage() {
        guard let eng = engine else { return }
        tgsb_set_language(eng, selectedLanguage.espeakTag, selectedLanguage.tgsbTag)
        applyStoredOverrides(selectedLanguage.tgsbTag)
        reapplyDictOverrides(selectedLanguage.tgsbTag)
    }

    private func getBaseValues(_ langTag: String) -> [String: String] {
        guard let eng = engine else { return [:] }
        guard let ptr = tgsb_query_data(eng, TGSB_DATA_SETTINGS, langTag, 0, 0) else { return [:] }
        let jsonStr = String(cString: ptr)
        tgsb_free_string(ptr)
        guard let data = jsonStr.data(using: .utf8),
              let arr = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]]
        else { return [:] }
        var map: [String: String] = [:]
        for obj in arr {
            guard let key = obj["key"] as? String else { continue }
            let jsonType = obj["type"] as? String ?? "string"
            if jsonType == "bool", let b = obj["value"] as? Bool {
                map[key] = b ? "true" : "false"
            } else {
                map[key] = "\(obj["value"] ?? "")"
            }
        }
        return map
    }

    private func loadOverrides(_ langTag: String) -> [String: String] {
        let d = UserDefaults(suiteName: kAppGroupId)
        guard let json = d?.string(forKey: "pack_overrides_\(langTag)"),
              let data = json.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: String]
        else { return [:] }
        return obj
    }

    private func saveOverrides(_ langTag: String, _ overrides: [String: String]) {
        let d = UserDefaults(suiteName: kAppGroupId)
        if overrides.isEmpty {
            d?.removeObject(forKey: "pack_overrides_\(langTag)")
        } else if let data = try? JSONSerialization.data(withJSONObject: overrides),
                  let json = String(data: data, encoding: .utf8) {
            d?.set(json, forKey: "pack_overrides_\(langTag)")
        }
        // Bump version so AU extension reloads pack with new overrides.
        let ver = (d?.integer(forKey: "adv_settingsVersion") ?? 0) + 1
        d?.set(ver, forKey: "adv_settingsVersion")
        d?.synchronize()
    }

    private func detectType(_ value: String) -> SettingType {
        if value == "true" || value == "false" { return .bool_ }
        if Double(value) != nil { return .number }
        return .text
    }

    private func camelToDisplay(_ key: String) -> String {
        let dotReplaced = key.replacingOccurrences(of: ".", with: ": ")
        var result = ""
        for c in dotReplaced {
            if c.isUppercase && !result.isEmpty && result.last?.isLowercase == true {
                result.append(" ")
            }
            result.append(c)
        }
        return result.prefix(1).uppercased() + result.dropFirst()
    }

    // MARK: - Phoneme editor

    struct PhonemeEntry: Identifiable {
        let id: String  // IPA key
        let key: String
        let phonemeClass: String // "vowel", "stop", etc.
        let mappingFrom: String  // non-empty in lang-filtered view
    }

    struct PhonemeField: Identifiable {
        let id: String       // full dot key e.g. "ɪ.cf2"
        let key: String
        let fieldName: String    // just the field part e.g. "cf2"
        let displayName: String
        let value: String
        let isOverridden: Bool
        let isUserAdded: Bool    // true if field only exists in user overrides, not in base phoneme
        let type: SettingType
    }

    /// Human-readable names and sort order for phoneme fields.
    private static let phonemeFieldInfo: [(String, String)] = [
        // Voicing
        ("voicePitch", "Voice Pitch (Hz)"),
        ("endVoicePitch", "End Voice Pitch (Hz)"),
        ("voiceAmplitude", "Voice Amplitude"),
        ("aspirationAmplitude", "Aspiration Amplitude"),
        ("glottalOpenQuotient", "Glottal Open Quotient"),
        ("voiceTurbulenceAmplitude", "Voice Turbulence"),
        ("vibratoPitchOffset", "Vibrato Pitch Offset"),
        ("vibratoSpeed", "Vibrato Speed (Hz)"),
        // Cascade formants
        ("cf1", "F1 Frequency (Hz)"), ("cf2", "F2 Frequency (Hz)"),
        ("cf3", "F3 Frequency (Hz)"), ("cf4", "F4 Frequency (Hz)"),
        ("cf5", "F5 Frequency (Hz)"), ("cf6", "F6 Frequency (Hz)"),
        ("cb1", "F1 Bandwidth (Hz)"), ("cb2", "F2 Bandwidth (Hz)"),
        ("cb3", "F3 Bandwidth (Hz)"), ("cb4", "F4 Bandwidth (Hz)"),
        ("cb5", "F5 Bandwidth (Hz)"), ("cb6", "F6 Bandwidth (Hz)"),
        // Nasal
        ("cfN0", "Nasal Zero Frequency"), ("cfNP", "Nasal Pole Frequency"),
        ("cbN0", "Nasal Zero Bandwidth"), ("cbNP", "Nasal Pole Bandwidth"),
        ("caNP", "Nasal Pole Amplitude"),
        // Frication
        ("fricationAmplitude", "Frication Amplitude"),
        ("preFormantGain", "Pre-Formant Gain"),
        // Parallel formants
        ("pf1", "Parallel F1 Frequency"), ("pf2", "Parallel F2 Frequency"),
        ("pf3", "Parallel F3 Frequency"), ("pf4", "Parallel F4 Frequency"),
        ("pf5", "Parallel F5 Frequency"), ("pf6", "Parallel F6 Frequency"),
        ("pb1", "Parallel F1 Bandwidth"), ("pb2", "Parallel F2 Bandwidth"),
        ("pb3", "Parallel F3 Bandwidth"), ("pb4", "Parallel F4 Bandwidth"),
        ("pb5", "Parallel F5 Bandwidth"), ("pb6", "Parallel F6 Bandwidth"),
        ("pa1", "Parallel F1 Amplitude"), ("pa2", "Parallel F2 Amplitude"),
        ("pa3", "Parallel F3 Amplitude"), ("pa4", "Parallel F4 Amplitude"),
        ("pa5", "Parallel F5 Amplitude"), ("pa6", "Parallel F6 Amplitude"),
        ("parallelBypass", "Parallel Bypass"),
        ("outputGain", "Output Gain"),
        // Flags
        ("_isVowel", "Is Vowel"), ("_isVoiced", "Is Voiced"),
        ("_isStop", "Is Stop"), ("_isNasal", "Is Nasal"),
        ("_isLiquid", "Is Liquid"), ("_isSemivowel", "Is Semivowel"),
        ("_isAffricate", "Is Affricate"), ("_isTap", "Is Tap"),
        ("_isTrill", "Is Trill"), ("_copyAdjacent", "Copy Adjacent"),
        // FrameEx
        ("frameEx.creakiness", "Creakiness"), ("frameEx.breathiness", "Breathiness"),
        ("frameEx.jitter", "Jitter"), ("frameEx.shimmer", "Shimmer"),
        ("frameEx.sharpness", "Glottal Sharpness"),
        ("frameEx.endCf1", "Diphthong End F1"), ("frameEx.endCf2", "Diphthong End F2"),
        ("frameEx.endCf3", "Diphthong End F3"),
        ("frameEx.endPf1", "Diphthong End Parallel F1"),
        ("frameEx.endPf2", "Diphthong End Parallel F2"),
        ("frameEx.endPf3", "Diphthong End Parallel F3"),
        ("frameEx.fricationTiltDb", "Frication Tilt (dB, neg=darker)"),
        // Micro-events
        ("burstDurationMs", "Burst Duration (ms)"), ("burstDecayRate", "Burst Decay Rate"),
        ("burstSpectralTilt", "Burst Spectral Tilt"),
        ("voiceBarAmplitude", "Voice Bar Amplitude"), ("voiceBarF1", "Voice Bar F1 (Hz)"),
        ("releaseSpreadMs", "Release Spread (ms)"),
        ("fricAttackMs", "Frication Attack (ms)"), ("fricDecayMs", "Frication Decay (ms)"),
        ("durationScale", "Duration Scale"),
        ("closureGapMs", "Closure Gap (ms, stops)"),
    ]

    private static let phonemeFieldOrder: [String: Int] = {
        var map: [String: Int] = [:]
        for (i, pair) in phonemeFieldInfo.enumerated() {
            map[pair.0] = i
        }
        return map
    }()

    private func phonemeDisplayName(_ fieldName: String) -> String {
        for (k, v) in Self.phonemeFieldInfo {
            if k == fieldName { return v }
        }
        return camelToDisplay(fieldName)
    }

    @Published var phonemeList: [PhonemeEntry] = []
    @Published var phonemeFields: [PhonemeField] = []

    func loadPhonemeList(langTag: String = "") {
        guard let eng = engine else { return }
        guard let ptr = tgsb_query_data(eng, TGSB_DATA_PHONEMES, langTag, 0, 0) else { return }
        let jsonStr = String(cString: ptr)
        tgsb_free_string(ptr)

        guard let data = jsonStr.data(using: .utf8),
              let arr = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]]
        else { return }

        var seen: [String: PhonemeEntry] = [:]
        var order: [String] = []
        for obj in arr {
            guard let group = obj["group"] as? String else { continue }
            if seen[group] == nil {
                order.append(group)
                seen[group] = PhonemeEntry(
                    id: group, key: group,
                    phonemeClass: obj["class"] as? String ?? "other",
                    mappingFrom: obj["mappingFrom"] as? String ?? "")
            }
        }
        phonemeList = order.compactMap { seen[$0] }
    }

    func loadPhonemeFields(phonemeKey: String) {
        guard let eng = engine else { return }
        // Always query all phonemes (base) then filter.
        guard let ptr = tgsb_query_data(eng, TGSB_DATA_PHONEMES, "", 0, 0) else { return }
        let jsonStr = String(cString: ptr)
        tgsb_free_string(ptr)

        let overrides = loadPhonemeOverrides()

        guard let data = jsonStr.data(using: .utf8),
              let arr = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]]
        else { return }

        var fields: [PhonemeField] = []
        var baseKeys = Set<String>()
        for obj in arr {
            guard let group = obj["group"] as? String, group == phonemeKey,
                  let fullKey = obj["key"] as? String else { continue }
            baseKeys.insert(fullKey)
            let fieldName = String(fullKey.dropFirst(phonemeKey.count + 1)) // remove "key."
            let jsonType = obj["type"] as? String ?? "string"
            let baseValue: String
            if jsonType == "bool", let b = obj["value"] as? Bool {
                baseValue = b ? "true" : "false"
            } else {
                baseValue = "\(obj["value"] ?? "")"
            }
            let effectiveValue = overrides[fullKey] ?? baseValue
            let type: SettingType = jsonType == "bool" ? .bool_ :
                                    jsonType == "float" ? .number : .text
            fields.append(PhonemeField(
                id: fullKey, key: fullKey,
                fieldName: fieldName,
                displayName: phonemeDisplayName(fieldName),
                value: effectiveValue,
                isOverridden: overrides[fullKey] != nil,
                isUserAdded: false,
                type: type))
        }

        // Append user-added fields (overrides that don't exist in the base phoneme).
        let prefix = "\(phonemeKey)."
        for (fullKey, value) in overrides {
            guard fullKey.hasPrefix(prefix), !baseKeys.contains(fullKey) else { continue }
            let fieldName = String(fullKey.dropFirst(prefix.count))
            // Determine type from field name, not stored value.
            let isBool = fieldName.hasPrefix("_is") || fieldName.hasPrefix("_copy")
            fields.append(PhonemeField(
                id: fullKey, key: fullKey,
                fieldName: fieldName,
                displayName: phonemeDisplayName(fieldName),
                value: value,
                isOverridden: true,
                isUserAdded: true,
                type: isBool ? .bool_ : .number))
        }

        let maxOrder = Self.phonemeFieldOrder.count
        phonemeFields = fields.sorted { a, b in
            let oa = Self.phonemeFieldOrder[a.fieldName] ?? maxOrder
            let ob = Self.phonemeFieldOrder[b.fieldName] ?? maxOrder
            return oa < ob
        }
    }

    /// Returns fields from phonemeFieldInfo that are not currently in the phoneme's field list.
    func getAvailableFieldsToAdd() -> [(String, String)] {
        let existing = Set(phonemeFields.map { $0.fieldName })
        return Self.phonemeFieldInfo.filter { !existing.contains($0.0) }
    }

    func setPhonemeOverride(fullKey: String, value: String) {
        guard let eng = engine else { return }
        // Apply in-memory immediately for live preview.
        tgsb_set_data(eng, TGSB_DATA_PHONEMES, "", fullKey, value)
        // Persist.
        var overrides = loadPhonemeOverrides()
        overrides[fullKey] = value
        savePhonemeOverrides(overrides)
    }

    func removePhonemeOverride(fullKey: String) {
        var overrides = loadPhonemeOverrides()
        overrides.removeValue(forKey: fullKey)
        savePhonemeOverrides(overrides)
        reloadCurrentLanguage()
        reapplyAllPhonemeOverrides()
    }

    // ── Phoneme overrides import / export ────────────────────────────

    /// Returns the phoneme overrides as a pretty-printed JSON string, or nil if empty.
    /// Return merged phonemes YAML content (base + overrides).
    func phonemeYamlContent() -> String? {
        guard let eng = engine else { return nil }
        let overrides = loadPhonemeOverrides()
        let json: String
        if overrides.isEmpty {
            json = "{}"
        } else if let data = try? JSONSerialization.data(withJSONObject: overrides),
                  let s = String(data: data, encoding: .utf8) {
            json = s
        } else {
            json = "{}"
        }
        guard let ptr = tgsb_export_data(eng, TGSB_DATA_PHONEMES, "", json) else { return nil }
        let result = String(cString: ptr)
        free(ptr)
        return result
    }

    /// Write merged phonemes YAML to a temp file for sharing.
    func exportPhonemeYamlToTempFile() -> URL? {
        guard let yaml = phonemeYamlContent() else { return nil }
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("phonemes.yaml")
        guard let _ = try? yaml.write(to: url, atomically: true, encoding: .utf8) else {
            return nil
        }
        return url
    }

    /// Import phoneme overrides from a JSON file. Returns a status message.
    func importPhonemeOverrides(from url: URL) -> String {
        let accessing = url.startAccessingSecurityScopedResource()
        defer { if accessing { url.stopAccessingSecurityScopedResource() } }

        guard let content = try? String(contentsOf: url, encoding: .utf8) else {
            return "Could not read file"
        }
        if content.isEmpty { return "File is empty" }

        guard let data = content.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: String]
        else {
            return "Invalid phoneme overrides file"
        }

        savePhonemeOverrides(obj)
        reloadCurrentLanguage()
        reapplyAllPhonemeOverrides()
        return "Imported \(obj.count) phoneme overrides"
    }

    func resetPhonemeOverrides(phonemeKey: String) {
        let prefix = "\(phonemeKey)."
        var overrides = loadPhonemeOverrides()
        overrides = overrides.filter { !$0.key.hasPrefix(prefix) }
        savePhonemeOverrides(overrides)
        reloadCurrentLanguage()
        reapplyAllPhonemeOverrides()
    }

    /// Remove all phoneme overrides, optionally filtered to a specific language's phoneme list.
    func resetAllPhonemeOverrides(langFilter: String = "") {
        if langFilter.isEmpty {
            savePhonemeOverrides([:])
        } else {
            let phonemeKeys = Set(phonemeList.map { $0.key })
            var overrides = loadPhonemeOverrides()
            overrides = overrides.filter { entry in
                !phonemeKeys.contains(where: { entry.key.hasPrefix("\($0).") })
            }
            savePhonemeOverrides(overrides)
        }
        reloadCurrentLanguage()
        reapplyAllPhonemeOverrides()
    }

    func previewPhoneme(_ phonemeKey: String) {
        guard let eng = engine else { return }
        stopSpeaking()

        isSpeaking = true
        let sr = self.sampleRate

        synthQueue.async { [weak self] in
            tgsb_preview_phoneme(eng, phonemeKey, 120.0, 300.0)

            let chunkSize = 4096
            var chunk = [Int16](repeating: 0, count: chunkSize)
            var allSamples = [Int16]()

            while true {
                let n = tgsb_pull_audio(eng, &chunk, Int32(chunkSize))
                if n <= 0 { break }
                allSamples.append(contentsOf: chunk.prefix(Int(n)))
            }

            guard !allSamples.isEmpty else {
                DispatchQueue.main.async { self?.isSpeaking = false }
                return
            }

            let wavData = Self.makeWAV(samples: allSamples, sampleRate: sr)
            DispatchQueue.main.async {
                self?.playWAV(wavData)
            }
        }
    }

    func reapplyAllPhonemeOverrides() {
        guard let eng = engine else { return }
        let overrides = loadPhonemeOverrides()
        for (k, v) in overrides {
            tgsb_set_data(eng, TGSB_DATA_PHONEMES, "", k, v)
        }
    }

    private func loadPhonemeOverrides() -> [String: String] {
        let d = UserDefaults(suiteName: kAppGroupId)
        guard let json = d?.string(forKey: "phoneme_overrides"),
              let data = json.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: String]
        else { return [:] }
        return obj
    }

    private func savePhonemeOverrides(_ overrides: [String: String]) {
        let d = UserDefaults(suiteName: kAppGroupId)
        if overrides.isEmpty {
            d?.removeObject(forKey: "phoneme_overrides")
        } else if let data = try? JSONSerialization.data(withJSONObject: overrides),
                  let json = String(data: data, encoding: .utf8) {
            d?.set(json, forKey: "phoneme_overrides")
        }
        d?.synchronize()
        bumpOverridesVersion()
    }

    // MARK: - Dictionary editor

    struct DictEntry: Identifiable, Hashable {
        let id: String  // fromText
        let fromText: String
        let toText: String
        let fromIpa: String
        let toIpa: String
        let category: String
        let source: String  // "main" or "user"
        let masked: Bool
    }

    struct DictType: Identifiable, Equatable {
        let id: String  // type name
        let type: String
        let count: Int
    }

    @Published var dictionaryEntries: [DictEntry] = []
    @Published var dictionaryTotalCount: Int = 0
    @Published var dictionaryCategories: [String] = []
    @Published var dictTypes: [DictType] = []
    private var dictLangTag: String = ""
    private var dictSubType: String = ""

    /// Returns the engine's current language tag (tgsb tag).
    func currentEngineLangTag() -> String {
        return selectedLanguage.tgsbTag
    }

    func loadDictTypes(langTag: String = "") {
        guard let eng = engine else { return }
        let tag = langTag.isEmpty ? "types" : "types:\(langTag)"
        guard let ptr = tgsb_query_data(eng, TGSB_DATA_DICTIONARY, tag, 0, 0) else {
            dictTypes = []
            return
        }
        let jsonStr = String(cString: ptr)
        tgsb_free_string(ptr)
        guard let data = jsonStr.data(using: .utf8),
              let arr = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]]
        else {
            dictTypes = []
            return
        }
        dictTypes = arr.compactMap { obj in
            guard let type = obj["type"] as? String,
                  let count = obj["count"] as? Int else { return nil }
            return DictType(id: type, type: type, count: count)
        }
    }

    /// Build prefixed langTag: "stress:en-us" or just "en-us" for pronounce.
    private func prefixedLangTag(_ subType: String, _ langTag: String) -> String {
        if subType.isEmpty || subType == "pronounce" { return langTag }
        return "\(subType):\(langTag)"
    }

    func loadDictionary(langTag: String, subType: String = "", offset: Int = 0,
                        limit: Int = 100, search: String = "", append: Bool = false) {
        guard let eng = engine else { return }
        dictLangTag = langTag
        dictSubType = subType

        var tag = prefixedLangTag(subType, langTag)
        if !search.isEmpty { tag += "?\(search)" }

        dictionaryTotalCount = Int(tgsb_get_data_count(eng, TGSB_DATA_DICTIONARY, tag))

        guard let ptr = tgsb_query_data(eng, TGSB_DATA_DICTIONARY, tag,
                                         Int32(offset), Int32(limit)) else {
            if !append { dictionaryEntries = [] }
            return
        }
        let jsonStr = String(cString: ptr)
        tgsb_free_string(ptr)

        guard let data = jsonStr.data(using: .utf8),
              let arr = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]]
        else { return }

        var entries: [DictEntry] = []
        var cats = Set<String>()
        for obj in arr {
            let fromText = obj["fromText"] as? String ?? obj["key"] as? String ?? ""
            let entry = DictEntry(
                id: fromText,
                fromText: fromText,
                toText: obj["toText"] as? String ?? "",
                fromIpa: obj["fromIpa"] as? String ?? "",
                toIpa: obj["toIpa"] as? String ?? "",
                category: obj["category"] as? String ?? "",
                source: obj["source"] as? String ?? "main",
                masked: obj["masked"] as? Bool ?? false
            )
            entries.append(entry)
            if !entry.category.isEmpty { cats.insert(entry.category) }
        }
        if append {
            dictionaryEntries.append(contentsOf: entries)
        } else {
            dictionaryEntries = entries
        }
        dictionaryCategories = cats.sorted()
    }

    func addDictEntry(fromText: String, toText: String, category: String = "",
                      fromIpa: String = "", toIpa: String = "") {
        guard let eng = engine, !fromText.isEmpty, !toText.isEmpty else { return }
        let tag = prefixedLangTag(dictSubType, dictLangTag)
        var dict: [String: Any] = ["toText": toText]
        if !category.isEmpty { dict["category"] = category }
        if !fromIpa.isEmpty { dict["fromIpa"] = fromIpa }
        if !toIpa.isEmpty { dict["toIpa"] = toIpa }
        if let data = try? JSONSerialization.data(withJSONObject: dict),
           let str = String(data: data, encoding: .utf8) {
            tgsb_set_data(eng, TGSB_DATA_DICTIONARY, tag, fromText, str)
            saveDictOverride(dictLangTag, key: fromText, value: str)
        }
        loadDictionary(langTag: dictLangTag, subType: dictSubType)
    }

    func maskDictEntry(fromText: String, masked: Bool) {
        guard let eng = engine else { return }
        let tag = prefixedLangTag(dictSubType, dictLangTag)
        let dict: [String: Any] = ["masked": masked]
        if let data = try? JSONSerialization.data(withJSONObject: dict),
           let str = String(data: data, encoding: .utf8) {
            tgsb_set_data(eng, TGSB_DATA_DICTIONARY, tag, fromText, str)
            saveDictOverride(dictLangTag, key: fromText, value: str)
        }
        loadDictionary(langTag: dictLangTag, subType: dictSubType)
    }

    func deleteDictEntry(fromText: String) {
        guard let eng = engine else { return }
        removeDictOverride(dictLangTag, key: fromText)
        // Reload the language to restore base dict entries from disk,
        // then reapply remaining user overrides. This ensures that
        // removing a modified base entry reverts to the original.
        tgsb_set_language(eng, dictLangTag, dictLangTag)
        reapplyDictOverrides(dictLangTag)
        loadDictionary(langTag: dictLangTag, subType: dictSubType)
    }

    func getPhonemeKeys() -> [(key: String, cls: String)] {
        guard let eng = engine else { return [] }
        let tag = dictLangTag.isEmpty ? "en-us" : dictLangTag
        guard let ptr = tgsb_query_data(eng, TGSB_DATA_PHONEMES, tag, 0, 0) else { return [] }
        let jsonStr = String(cString: ptr)
        free(ptr)
        guard let data = jsonStr.data(using: .utf8),
              let arr = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else { return [] }
        var seen = Set<String>()
        var result: [(key: String, cls: String)] = []
        for obj in arr {
            guard let group = obj["group"] as? String, !group.isEmpty, !seen.contains(group) else { continue }
            seen.insert(group)
            result.append((key: group, cls: (obj["class"] as? String) ?? ""))
        }
        return result
    }

    func maskDictCategory(category: String, masked: Bool) {
        guard let eng = engine else { return }
        let tag = prefixedLangTag(dictSubType, dictLangTag)
        let matching = dictionaryEntries.filter {
            $0.category.caseInsensitiveCompare(category) == .orderedSame && $0.masked != masked
        }
        for entry in matching {
            let dict: [String: Any] = ["masked": masked]
            if let data = try? JSONSerialization.data(withJSONObject: dict),
               let str = String(data: data, encoding: .utf8) {
                tgsb_set_data(eng, TGSB_DATA_DICTIONARY, tag, entry.fromText, str)
                saveDictOverride(dictLangTag, key: entry.fromText, value: str)
            }
        }
        if !matching.isEmpty {
            loadDictionary(langTag: dictLangTag, subType: dictSubType)
        }
    }

    func textToIpa(_ text: String) -> String {
        guard let eng = engine else { return "" }
        guard let ptr = tgsb_text_to_ipa(eng, text) else { return "" }
        let result = String(cString: ptr)
        free(ptr)
        return result
    }

    // ── Dictionary override persistence ────────────────────────────

    func reapplyDictOverrides(_ langTag: String) {
        guard let eng = engine else { return }
        let overrides = loadDictOverrides(langTag)
        for (k, v) in overrides {
            tgsb_set_data(eng, TGSB_DATA_DICTIONARY, langTag, k, v)
        }
        // Re-apply dict type disabled state
        let disabled = loadDictDisabled(langTag)
        for type in disabled {
            tgsb_set_data(eng, TGSB_DATA_DICTIONARY, "config:\(langTag)", type, "false")
        }
    }

    // ── Dict type exclusion ────────────────────────────────────────

    func loadDictConfig(langTag: String) -> [(type: String, enabled: Bool)] {
        guard let eng = engine else { return [] }
        guard let ptr = tgsb_query_data(eng, TGSB_DATA_DICTIONARY, "config:\(langTag)", 0, 0) else { return [] }
        let jsonStr = String(cString: ptr)
        tgsb_free_string(ptr)
        guard let data = jsonStr.data(using: .utf8),
              let arr = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]]
        else { return [] }
        return arr.compactMap { obj in
            guard let type = obj["type"] as? String,
                  let enabled = obj["enabled"] as? Bool else { return nil }
            return (type: type, enabled: enabled)
        }
    }

    func setDictTypeEnabled(langTag: String, type: String, enabled: Bool) {
        guard let eng = engine else { return }
        tgsb_set_data(eng, TGSB_DATA_DICTIONARY, "config:\(langTag)", type, enabled ? "true" : "false")
        saveDictDisabled(langTag, type: type, disabled: !enabled)
    }

    private func loadDictDisabled(_ langTag: String) -> Set<String> {
        let d = UserDefaults(suiteName: kAppGroupId)
        guard let json = d?.string(forKey: "dict_disabled_\(langTag)"),
              let data = json.data(using: .utf8),
              let arr = try? JSONSerialization.jsonObject(with: data) as? [String]
        else { return [] }
        return Set(arr)
    }

    private func saveDictDisabled(_ langTag: String, type: String, disabled: Bool) {
        var current = loadDictDisabled(langTag)
        if disabled {
            current.insert(type)
        } else {
            current.remove(type)
        }
        let d = UserDefaults(suiteName: kAppGroupId)
        if current.isEmpty {
            d?.removeObject(forKey: "dict_disabled_\(langTag)")
        } else {
            if let data = try? JSONSerialization.data(withJSONObject: Array(current)),
               let json = String(data: data, encoding: .utf8) {
                d?.set(json, forKey: "dict_disabled_\(langTag)")
            }
        }
        // Siblings (phoneme/dict overrides) bump on every save; this one
        // didn't — harmless while the AU re-applied overrides on every
        // utterance, load-bearing now that the AU gates on the version.
        bumpOverridesVersion()
        d?.synchronize()
    }

    private func loadDictOverrides(_ langTag: String) -> [String: String] {
        let d = UserDefaults(suiteName: kAppGroupId)
        guard let json = d?.string(forKey: "dict_overrides_\(langTag)"),
              let data = json.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: String]
        else { return [:] }
        return obj
    }

    private func bumpOverridesVersion() {
        let d = UserDefaults(suiteName: kAppGroupId)
        let ver = (d?.integer(forKey: "pack_overrides_version") ?? 0) + 1
        d?.set(ver, forKey: "pack_overrides_version")
        d?.synchronize()
    }

    private func saveDictOverride(_ langTag: String, key: String, value: String) {
        var overrides = loadDictOverrides(langTag)
        overrides[key] = value
        let d = UserDefaults(suiteName: kAppGroupId)
        if let data = try? JSONSerialization.data(withJSONObject: overrides),
           let json = String(data: data, encoding: .utf8) {
            d?.set(json, forKey: "dict_overrides_\(langTag)")
        }
        d?.synchronize()
        bumpOverridesVersion()
    }

    private func removeDictOverride(_ langTag: String, key: String) {
        var overrides = loadDictOverrides(langTag)
        overrides.removeValue(forKey: key)
        let d = UserDefaults(suiteName: kAppGroupId)
        if overrides.isEmpty {
            d?.removeObject(forKey: "dict_overrides_\(langTag)")
        } else if let data = try? JSONSerialization.data(withJSONObject: overrides),
                  let json = String(data: data, encoding: .utf8) {
            d?.set(json, forKey: "dict_overrides_\(langTag)")
        }
        d?.synchronize()
        bumpOverridesVersion()
    }

    /// Preview a dictionary entry: speaks the replacement text.
    func previewDictEntry(from: String, to: String, toIpa: String = "") {
        // Preview the replacement — speak the respelling text.
        // TODO: IPA preview requires a queueIPA bridge function.
        speak(to)
    }

    func speak(_ text: String) {
        guard let eng = engine else { return }
        stopSpeaking()

        // Apply current settings
        tgsb_set_language(eng,
                          selectedLanguage.espeakTag,
                          selectedLanguage.tgsbTag)
        applyStoredOverrides(selectedLanguage.tgsbTag)
        reapplyDictOverrides(selectedLanguage.tgsbTag)
        applySelectedVoice(eng)
        applyEngineSettings()
        tgsb_set_inflection(eng, inflectionValue / 100.0)

        isSpeaking = true

        // Use override rate if enabled, otherwise Speak tab slider
        let d = UserDefaults(suiteName: kAppGroupId)
        var speed = self.speed
        if d?.bool(forKey: "adv_overrideRate") == true {
            speed = d?.double(forKey: "adv_globalRate") ?? 1.0
        }
        if self.rateBoost { speed *= 2.0 }
        let pitch = self.pitch
        let sr = self.sampleRate

        synthQueue.async { [weak self] in
            // Queue text (fast, <10ms)
            tgsb_queue_text(eng, text, speed, pitch)

            // Pull all PCM into a buffer
            let chunkSize = 4096
            var chunk = [Int16](repeating: 0, count: chunkSize)
            var allSamples = [Int16]()

            while true {
                let n = tgsb_pull_audio(eng, &chunk, Int32(chunkSize))
                if n <= 0 { break }
                allSamples.append(contentsOf: chunk.prefix(Int(n)))
            }

            guard !allSamples.isEmpty else {
                DispatchQueue.main.async { self?.isSpeaking = false }
                return
            }

            // Build WAV data in memory
            let wavData = Self.makeWAV(samples: allSamples,
                                        sampleRate: sr)

            DispatchQueue.main.async {
                self?.playWAV(wavData)
            }
        }
    }

    func stopSpeaking() {
        if let eng = engine {
            tgsb_stop(eng)
        }
        audioPlayer?.stop()
        audioPlayer = nil
        isSpeaking = false
    }

    // MARK: - WAV playback

    private func playWAV(_ data: Data) {
        do {
            let player = try AVAudioPlayer(data: data)
            player.delegate = audioDelegate
            let savedVol = UserDefaults(suiteName: kAppGroupId)?.double(forKey: "systemVolume") ?? 0.0
            if savedVol > 0.0 {
                player.volume = Float(savedVol)
            }
            player.play()
            self.audioPlayer = player
        } catch {
            print("[TgsbEngine] AVAudioPlayer error: \(error)")
            isSpeaking = false
        }
    }

    // Delegate to detect playback completion
    private lazy var audioDelegate = AudioDelegate { [weak self] in
        DispatchQueue.main.async {
            self?.isSpeaking = false
        }
    }

    // MARK: - WAV builder

    private static func makeWAV(samples: [Int16], sampleRate: Int) -> Data {
        let numSamples = samples.count
        let dataSize = numSamples * 2
        let fileSize = 36 + dataSize

        var data = Data(capacity: 44 + dataSize)

        func writeU16(_ v: UInt16) {
            var le = v.littleEndian
            data.append(Data(bytes: &le, count: 2))
        }
        func writeU32(_ v: UInt32) {
            var le = v.littleEndian
            data.append(Data(bytes: &le, count: 4))
        }
        func writeTag(_ s: String) {
            data.append(Data(s.utf8))
        }

        // RIFF header
        writeTag("RIFF")
        writeU32(UInt32(fileSize))
        writeTag("WAVE")

        // fmt  chunk
        writeTag("fmt ")
        writeU32(16)                             // chunk size
        writeU16(1)                              // PCM format
        writeU16(1)                              // mono
        writeU32(UInt32(sampleRate))
        writeU32(UInt32(sampleRate * 2))         // byte rate
        writeU16(2)                              // block align
        writeU16(16)                             // bits per sample

        // data chunk
        writeTag("data")
        writeU32(UInt32(dataSize))

        // PCM samples
        samples.withUnsafeBytes { raw in
            data.append(contentsOf: raw)
        }

        return data
    }

    // MARK: - Resource paths

    private static func resourcePath(for name: String) -> String? {
        // Check app bundle first
        if let path = Bundle.main.resourcePath {
            let full = (path as NSString).appendingPathComponent(name)
            if FileManager.default.fileExists(atPath: full) {
                return full
            }
        }

        // Development fallback: look for data relative to the source tree.
        let fm = FileManager.default
        let sourceRoot = (#filePath as NSString)
            .deletingLastPathComponent          // TGSpeechBox/
            .appending("/..")                   // apple/
        let candidates: [String]
        switch name {
        case "espeak-ng-data":
            candidates = [
                (sourceRoot as NSString).appendingPathComponent("Resources/espeak-ng-data"),
                (sourceRoot as NSString).appendingPathComponent("../data/espeak-ng-data"),
            ]
        case "packs":
            candidates = [
                (sourceRoot as NSString).appendingPathComponent("Resources/packs"),
                (sourceRoot as NSString).appendingPathComponent("../../tgspeechbox/packs"),
            ]
        default:
            candidates = []
        }
        for path in candidates {
            let resolved = (path as NSString).standardizingPath
            if fm.fileExists(atPath: resolved) {
                return resolved
            }
        }
        return nil
    }
}

// MARK: - Helpers

private class AudioDelegate: NSObject, AVAudioPlayerDelegate {
    let onFinish: () -> Void
    init(onFinish: @escaping () -> Void) { self.onFinish = onFinish }
    func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer,
                                     successfully flag: Bool) {
        onFinish()
    }
}

private extension UInt16 {
    var littleEndianBytes: [UInt8] {
        let le = self.littleEndian
        return [UInt8(le & 0xFF), UInt8(le >> 8)]
    }
}

private extension UInt32 {
    var littleEndianBytes: [UInt8] {
        let le = self.littleEndian
        return [UInt8(le & 0xFF), UInt8((le >> 8) & 0xFF),
                UInt8((le >> 16) & 0xFF), UInt8((le >> 24) & 0xFF)]
    }
}
