/*
 * PhonemeEditorView — Phoneme acoustic parameter editor.
 *
 * List view with language filter, detail view with sliders and live preview,
 * add-field picker, and reset functionality.
 *
 * License: GPL-3.0
 */

import SwiftUI
import UniformTypeIdentifiers

// MARK: - Phoneme List

struct PhonemeListView: View {
    @ObservedObject var engine: TgsbEngine
    @Binding var engineStarted: Bool
    @Binding var langFilter: String
    @State private var showResetAllPhonemes = false
    @State private var showExportPicker = false
    @State private var statusMessage: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            // Language filter
            HStack {
                Menu {
                    Button("All phonemes") { langFilter = "" }
                    ForEach(engine.editorLanguages, id: \.self) { lang in
                        Button(lang) { langFilter = lang }
                    }
                } label: {
                    Text(langFilter.isEmpty ? "All phonemes" : langFilter)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 6)
                        .background(Color.secondary.opacity(0.15))
                        .cornerRadius(8)
                }
                .accessibilityLabel("Filter by language, \(langFilter.isEmpty ? "all phonemes" : langFilter)")

                Spacer()

                Text("\(engine.phonemeList.count) phonemes")
                    .font(.caption)
                    .foregroundColor(.secondary)

                Menu {
                    Button(action: {
                        showExportPicker = true
                    }) {
                        Label("Export Phonemes", systemImage: "square.and.arrow.up")
                    }
                    if let url = engine.exportPhonemeYamlToTempFile() {
                        ShareLink("Share Phonemes", item: url)
                    }
                    Button(role: .destructive, action: {
                        showResetAllPhonemes = true
                    }) {
                        Label("Reset All Phonemes", systemImage: "arrow.counterclockwise")
                    }
                } label: {
                    Image(systemName: "ellipsis.circle")
                        .font(.body)
                }
                .accessibilityLabel("More options")
#if os(iOS)
                .accessibilityElement()
                .accessibilityAddTraits(.isButton)
                .accessibilityHint("Double tap to show actions")
#endif
            }
            .padding(.horizontal)
            .padding(.top, 8)

            if engine.phonemeList.isEmpty {
                Text("No phonemes loaded")
                    .foregroundColor(.secondary)
                    .padding()
                Spacer()
            } else {
                List(engine.phonemeList) { entry in
                    NavigationLink(value: PhonemeNav(key: entry.key)) {
                        HStack {
                            Text(entry.key)
                                .font(.title3)
                                .frame(width: 50, alignment: .leading)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(entry.phonemeClass)
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                                if !entry.mappingFrom.isEmpty {
                                    Text("from: \(entry.mappingFrom)")
                                        .font(.caption2)
                                        .foregroundColor(.accentColor)
                                }
                            }
                            Spacer()
                        }
                    }
                    .accessibilityLabel({
                        var desc = "\(entry.key), \(entry.phonemeClass)"
                        if !entry.mappingFrom.isEmpty {
                            desc += ", mapped from \(entry.mappingFrom)"
                        }
                        return desc
                    }())
                    .accessibilityAction(named: "Play phoneme") {
                        engine.previewPhoneme(entry.key)
                    }
                    .accessibilityAction(named: "Copy phoneme") {
#if os(iOS)
                        UIPasteboard.general.string = entry.key
#elseif os(macOS)
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(entry.key, forType: .string)
#endif
                    }
                    .contextMenu {
                        Button(action: { engine.previewPhoneme(entry.key) }) {
                            Label("Play phoneme", systemImage: "play.fill")
                        }
                        Button(action: {
#if os(iOS)
                            UIPasteboard.general.string = entry.key
#elseif os(macOS)
                            NSPasteboard.general.clearContents()
                            NSPasteboard.general.setString(entry.key, forType: .string)
#endif
                        }) {
                            Label("Copy phoneme", systemImage: "doc.on.doc")
                        }
                    }
                }
                .listStyle(.plain)
            }
        }
        .onAppear {
            if !engineStarted {
                if engine.start() { engineStarted = true }
            }
            if engineStarted {
                engine.loadEditorLanguages()
                engine.loadPhonemeList(langTag: langFilter)
            }
        }
        .onChange(of: langFilter) { _ in
            engine.loadPhonemeList(langTag: langFilter)
        }
        .alert("Reset All Phonemes", isPresented: $showResetAllPhonemes) {
            Button("Reset", role: .destructive) {
                engine.resetAllPhonemeOverrides(langFilter: langFilter)
                engine.loadPhonemeList(langTag: langFilter)
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            let scope = langFilter.isEmpty ? "all languages" : langFilter
            Text("Reset all phoneme overrides for \(scope) back to defaults?")
        }
        .fileExporter(
            isPresented: $showExportPicker,
            document: PhonemeYamlDocument(engine: engine),
            contentType: .yaml,
            defaultFilename: "phonemes.yaml"
        ) { result in
            if case .success = result {
                statusMessage = "Exported phoneme overrides"
            } else if case .failure(let error) = result {
                statusMessage = "Export failed: \(error.localizedDescription)"
            }
        }
        .alert("Result", isPresented: Binding(
            get: { statusMessage != nil },
            set: { if !$0 { statusMessage = nil } }
        )) {
            Button("OK") { statusMessage = nil }
        } message: {
            Text(statusMessage ?? "")
        }
    }
}

// MARK: - Phoneme Overrides Document

struct PhonemeYamlDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.yaml, .plainText] }

    let content: String

    @MainActor init(engine: TgsbEngine) {
        content = engine.phonemeYamlContent() ?? ""
    }

    init(configuration: ReadConfiguration) throws {
        if let data = configuration.file.regularFileContents {
            content = String(data: data, encoding: .utf8) ?? "{}"
        } else {
            content = "{}"
        }
    }

    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        let data = content.data(using: .utf8) ?? Data()
        return FileWrapper(regularFileWithContents: data)
    }
}

// MARK: - Phoneme Detail

struct PhonemeDetailView: View {
    @ObservedObject var engine: TgsbEngine
    let phonemeKey: String
    @Environment(\.dismiss) private var dismiss

    @State private var editingKey: String?
    @State private var editingValue: String = ""
    @State private var showResetAll = false
    @State private var showAddField = false
    @State private var addingFieldName: String?
    @State private var addingFieldDisplay: String = ""
    @State private var addingFieldValue: String = ""
    @State private var scrollToId: String?

    var body: some View {
        VStack(spacing: 0) {
            if engine.phonemeFields.isEmpty {
                Text("No fields")
                    .foregroundColor(.secondary)
                    .padding()
                Spacer()
            } else {
                ScrollViewReader { proxy in
                    List(engine.phonemeFields) { field in
                        PhonemeFieldRowView(
                            field: field,
                            onValueChanged: { newVal in
                                engine.setPhonemeOverride(fullKey: field.key, value: newVal)
                            },
                            onEdit: {
                                editingKey = field.key
                                editingValue = field.value
                            },
                            onToggle: { newVal in
                                engine.setPhonemeOverride(fullKey: field.key, value: newVal)
                                engine.loadPhonemeFields(phonemeKey: phonemeKey)
                            },
                            onReset: {
                                engine.removePhonemeOverride(fullKey: field.key)
                                engine.loadPhonemeFields(phonemeKey: phonemeKey)
                            },
                            onPreview: { engine.previewPhoneme(phonemeKey) }
                        )
                    }
                    .listStyle(.plain)
                    .onChange(of: scrollToId) { target in
                        if let id = target {
                            withAnimation { proxy.scrollTo(id, anchor: .bottom) }
                            scrollToId = nil
                        }
                    }
                }
            }
        }
        .onAppear {
            engine.loadPhonemeFields(phonemeKey: phonemeKey)
#if os(iOS)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                UIAccessibility.post(notification: .screenChanged, argument: nil)
            }
#endif
        }
        .navigationTitle(phonemeKey)
        .toolbar {
            ToolbarItemGroup(placement: .automatic) {
                Button(action: { showAddField = true }) {
                    Image(systemName: "plus")
                }
                .accessibilityLabel("Add field to \(phonemeKey)")
                Button(action: { engine.previewPhoneme(phonemeKey) }) {
                    Image(systemName: "play.fill")
                }
                .accessibilityLabel("Preview \(phonemeKey)")
                Button("Reset all") { showResetAll = true }
            }
        }
        .alert("Edit Value", isPresented: Binding(
            get: { editingKey != nil },
            set: { if !$0 { editingKey = nil } }
        )) {
            TextField("Value", text: $editingValue)
#if os(iOS)
                .keyboardType(.decimalPad)
#endif
            Button("OK") {
                if let key = editingKey {
                    engine.setPhonemeOverride(fullKey: key, value: editingValue)
                    engine.previewPhoneme(phonemeKey)
                    engine.loadPhonemeFields(phonemeKey: phonemeKey)
                }
                editingKey = nil
            }
            Button("Cancel", role: .cancel) { editingKey = nil }
        } message: {
            if let key = editingKey {
                Text(String(key.split(separator: ".").last ?? ""))
            }
        }
        .alert("Reset \(phonemeKey)", isPresented: $showResetAll) {
            Button("Reset", role: .destructive) {
                engine.resetPhonemeOverrides(phonemeKey: phonemeKey)
                engine.loadPhonemeFields(phonemeKey: phonemeKey)
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Reset all overrides on this phoneme back to the original values?")
        }
        .sheet(isPresented: $showAddField) {
            AddFieldPickerView(engine: engine, phonemeKey: phonemeKey) { fieldName, displayName in
                addingFieldName = fieldName
                addingFieldDisplay = displayName
                addingFieldValue = fieldName.hasPrefix("_is") ? "false" : "0"
                showAddField = false
            }
        }
        .alert("Add \(addingFieldDisplay)", isPresented: Binding(
            get: { addingFieldName != nil && !(addingFieldName?.hasPrefix("_is") ?? false) && !(addingFieldName?.hasPrefix("_copy") ?? false) },
            set: { if !$0 { addingFieldName = nil } }
        )) {
            TextField("Value", text: $addingFieldValue)
#if os(iOS)
                .keyboardType(.decimalPad)
#endif
            Button("Add") {
                if let name = addingFieldName {
                    let fullKey = "\(phonemeKey).\(name)"
                    engine.setPhonemeOverride(fullKey: fullKey, value: addingFieldValue)
                    engine.loadPhonemeFields(phonemeKey: phonemeKey)
                    scrollToId = fullKey
                }
                addingFieldName = nil
            }
            Button("Cancel", role: .cancel) { addingFieldName = nil }
        } message: {
            Text("Enter value for \(addingFieldDisplay)")
        }
        .alert("Add \(addingFieldDisplay)", isPresented: Binding(
            get: { addingFieldName != nil && (addingFieldName?.hasPrefix("_is") ?? false || addingFieldName?.hasPrefix("_copy") ?? false) },
            set: { if !$0 { addingFieldName = nil } }
        )) {
            Button("Set True") {
                if let name = addingFieldName {
                    let fullKey = "\(phonemeKey).\(name)"
                    engine.setPhonemeOverride(fullKey: fullKey, value: "true")
                    engine.loadPhonemeFields(phonemeKey: phonemeKey)
                    scrollToId = fullKey
                }
                addingFieldName = nil
            }
            Button("Set False") {
                if let name = addingFieldName {
                    let fullKey = "\(phonemeKey).\(name)"
                    engine.setPhonemeOverride(fullKey: fullKey, value: "false")
                    engine.loadPhonemeFields(phonemeKey: phonemeKey)
                    scrollToId = fullKey
                }
                addingFieldName = nil
            }
            Button("Cancel", role: .cancel) { addingFieldName = nil }
        } message: {
            Text("Choose value for \(addingFieldDisplay)")
        }
    }
}

/// Sheet view for picking a field to add.
private struct AddFieldPickerView: View {
    @ObservedObject var engine: TgsbEngine
    let phonemeKey: String
    var onFieldSelected: (String, String) -> Void
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            let available = engine.getAvailableFieldsToAdd()
            if available.isEmpty {
                Text("All fields are already present.")
                    .foregroundColor(.secondary)
                    .padding()
            } else {
                List(available, id: \.0) { fieldName, displayName in
                    Button(displayName) {
                        onFieldSelected(fieldName, displayName)
                    }
                }
                .listStyle(.plain)
            }
        }
        .presentationDetents([.medium, .large])
    }
}

// MARK: - Phoneme Field Row

/// Slider range for a phoneme field based on its name and current value.
func phonemeFieldRange(_ fieldName: String, _ value: Float) -> ClosedRange<Float> {
    switch true {
    case fieldName.range(of: #"^[cp]f[1-6]$"#, options: .regularExpression) != nil:
        return 0...8000
    case fieldName == "cfNP" || fieldName == "cfN" || fieldName == "cfTP":
        return 0...5000
    case fieldName.range(of: #"^end[CP]f[1-6]$"#, options: .regularExpression) != nil:
        return 0...8000
    case fieldName.range(of: #"^[cp]b[1-6]$"#, options: .regularExpression) != nil:
        return 0...1000
    case fieldName.range(of: #"^end[CP]b[1-6]$"#, options: .regularExpression) != nil:
        return 0...1000
    case fieldName.range(of: #"^pa[1-6]$"#, options: .regularExpression) != nil:
        return 0...1.5
    case fieldName.contains("Amplitude") || fieldName.contains("amplitude"):
        return 0...1.5
    case fieldName.contains("Pitch") || fieldName.contains("pitch"):
        return 40...500
    case ["preFormantGain", "parallelBypass", "glottalOpenQuotient",
          "breathiness", "creakiness", "jitter", "shimmer", "sharpness"].contains(fieldName):
        return 0...1
    case fieldName == "fricationTiltDb":
        return -15...15
    case fieldName == "closureGapMs":
        return 0...60
    case value >= 0 && value <= 1:
        return 0...1.5
    case value > 1:
        return 0...max(value * 2.5, 100)
    default:
        return 0...max(value * 2.5, 100)
    }
}

/// Step size for a phoneme field slider (controls VoiceOver increment granularity).
func phonemeFieldStep(_ fieldName: String) -> Float {
    switch true {
    // Formant frequencies: 100 Hz steps
    case fieldName.range(of: #"^[cp]f[1-6]$"#, options: .regularExpression) != nil,
         fieldName == "cfNP" || fieldName == "cfN" || fieldName == "cfTP",
         fieldName.range(of: #"^end[CP]f[1-6]$"#, options: .regularExpression) != nil:
        return 100
    // Bandwidths: 25 Hz steps
    case fieldName.range(of: #"^[cp]b[1-6]$"#, options: .regularExpression) != nil,
         fieldName.range(of: #"^end[CP]b[1-6]$"#, options: .regularExpression) != nil:
        return 25
    // Pitch: 1 Hz steps
    case fieldName.contains("Pitch") || fieldName.contains("pitch"):
        return 1
    // Amplitudes, gains, ratios: 0.1 steps
    case fieldName.range(of: #"^pa[1-6]$"#, options: .regularExpression) != nil,
         fieldName.contains("Amplitude") || fieldName.contains("amplitude"),
         ["preFormantGain", "parallelBypass", "glottalOpenQuotient",
          "breathiness", "creakiness", "jitter", "shimmer", "sharpness",
          "outputGain", "burstDecayRate", "burstSpectralTilt",
          "durationScale", "fricationTiltDb"].contains(fieldName):
        return 0.1
    // Duration in ms: 1 ms steps
    case fieldName.contains("Ms"):
        return 1
    // Nasal pole amplitude
    case fieldName == "caNP":
        return 0.1
    default:
        return 1
    }
}

/// Format a slider value for display — drop trailing zeros.
func fmtVal(_ v: Float) -> String {
    if v == Float(Int(v)) { return "\(Int(v))" }
    let s = String(format: "%.2f", v)
    return s.replacingOccurrences(of: "0+$", with: "", options: .regularExpression)
            .replacingOccurrences(of: "\\.$", with: "", options: .regularExpression)
}

private struct PhonemeFieldRowView: View {
    let field: TgsbEngine.PhonemeField
    var onValueChanged: (String) -> Void
    var onEdit: () -> Void
    var onToggle: (String) -> Void
    var onReset: () -> Void
    var onPreview: () -> Void

    var body: some View {
        let isBool = field.type == .bool_
        let isNumber = field.type == .number

        VStack(alignment: .leading, spacing: 4) {
            if isBool {
                boolRow
            } else if isNumber {
                numericRow
            } else {
                textRow
            }

            if field.isOverridden {
                Button(field.isUserAdded ? "Remove \(field.displayName)" : "Reset \(field.displayName)") {
                    onReset()
                }
                .font(.caption)
            }
        }
        .padding(.vertical, 4)
    }

    private var boolRow: some View {
        let checked = field.value == "true"
        let overriddenSuffix = field.isUserAdded ? ", added" : field.isOverridden ? ", overridden" : ""
        return Toggle(isOn: Binding(
            get: { checked },
            set: { onToggle($0 ? "true" : "false") }
        )) {
            Text(field.displayName)
        }
        .accessibilityLabel("\(field.displayName), \(checked ? "on" : "off")\(overriddenSuffix)")
    }

    private var numericRow: some View {
        let baseValue = Float(field.value) ?? 0
        let range = phonemeFieldRange(field.fieldName, baseValue)
        let step = phonemeFieldStep(field.fieldName)
        let overriddenSuffix = field.isUserAdded ? ", added" : field.isOverridden ? ", overridden" : ""
        return PhonemeSliderRow(
            displayName: field.displayName,
            baseValue: baseValue,
            range: range,
            step: step,
            isOverridden: field.isOverridden,
            accessibilitySuffix: overriddenSuffix,
            onValueChanged: onValueChanged,
            onEdit: onEdit,
            onPreview: onPreview
        )
    }

    private var textRow: some View {
        let overriddenSuffix = field.isUserAdded ? ", added" : field.isOverridden ? ", overridden" : ""
        return HStack {
            Text(field.displayName)
                .font(.body)
            Spacer()
            Text(field.value)
                .foregroundColor(.secondary)
                .font(.body)
        }
        .contentShape(Rectangle())
        .onTapGesture { onEdit() }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(field.displayName), \(field.value)\(overriddenSuffix)")
        .accessibilityAddTraits(.isButton)
        .accessibilityHint("Double tap for exact input")
    }
}

/// Separate struct to hold slider @State properly.
private struct PhonemeSliderRow: View {
    let displayName: String
    let baseValue: Float
    let range: ClosedRange<Float>
    let step: Float
    let isOverridden: Bool
    let accessibilitySuffix: String
    var onValueChanged: (String) -> Void
    var onEdit: () -> Void
    var onPreview: () -> Void

    @State private var sliderValue: Float = 0

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            // Label with current value (visual only)
            HStack {
                Text(displayName)
                    .font(.body)
                Spacer()
                Text(fmtVal(sliderValue))
                    .font(.body)
                    .foregroundColor(isOverridden ? .accentColor : .secondary)
            }
            .accessibilityHidden(true)

            // Edit icon + slider on one line
            HStack(spacing: 8) {
                Button(action: onEdit) {
                    Image(systemName: "number")
                        .font(.body)
                        .frame(width: 44, height: 44)
                }
                .frame(width: 44, height: 44)
                .accessibilityLabel("Enter exact value for \(displayName)")

                Slider(
                    value: $sliderValue,
                    in: range,
                    step: step
                ) { editing in
                    if !editing {
                        onValueChanged(fmtVal(sliderValue))
                        onPreview()
                    }
                }
                .accessibilityLabel("\(displayName)\(accessibilitySuffix)")
                .accessibilityValue(fmtVal(sliderValue))
            }
        }
        .onAppear { sliderValue = baseValue.clamped(to: range) }
        .onChange(of: baseValue) { newVal in
            sliderValue = newVal.clamped(to: range)
        }
    }
}

private extension Float {
    func clamped(to range: ClosedRange<Float>) -> Float {
        return Swift.min(Swift.max(self, range.lowerBound), range.upperBound)
    }
}
