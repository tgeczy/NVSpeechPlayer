# NVSP Phoneme Editor (Win32)

A simple Win32 GUI for editing:

- `packs/phonemes.yaml` (global phoneme table)
- `packs/lang/*.yaml` (language normalization mappings)

This tool is meant for community editing: no fancy UI frameworks, just standard Win32 controls.

## Build

This repo already uses CMake.

On Windows:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The executable target is:

- `nvspPhonemeEditor`

## First run

1. **File > Open pack root...**
   - Pick the folder that contains the `packs` directory.
2. **Settings > Set DLL directory...**
   - Pick the folder that contains `speechPlayer.dll` and `nvspFrontend.dll`.
3. (Optional) **Settings > Set eSpeak directory...**
   - Needed if you want "Convert to IPA" for plain text.

## CLI phonemizer configuration (optional)

"Convert to IPA" uses a **CLI phonemizer** and captures its stdout. By default it uses
`espeak-ng.exe` (or `espeak.exe`) from the configured eSpeak directory.

Advanced users can override the phonemizer in `nvspPhonemeEditor.ini`:

```ini
[phonemizer]
; If empty, the tool uses espeak-ng.exe in [paths].espeakDir.
exe=

; Prefer feeding text via stdin (safer for long input) and fall back to passing
; the text as a command-line argument.
preferStdin=1

; Soft limit used for sentence-aware chunking before calling the phonemizer.
maxChunkChars=420

; Templates. Supported placeholders:
;   {lang} {qlang} {text} {qtext} {espeakDir} {qespeakDir} {dataDir} {qdataDir} {pathArg}
;
; When using eSpeak defaults, {pathArg} expands to "--path=... " if the data dir
; can be found, otherwise it is empty.
;
; For eSpeak (defaults):
; argsStdin=-q {pathArg}--ipa=3 -b 1 -v {qlang} --stdin
; argsCli=-q {pathArg}--ipa=3 -b 1 -v {qlang} {qtext}
argsStdin=
argsCli=
```

The tool stores these paths in `nvspPhonemeEditor.ini` next to the exe.

## What it can do

- Browse all phonemes from `phonemes.yaml`.
- Clone a phoneme entry to a new key.
- Edit a phoneme's scalar fields.
- Load a language YAML and edit its `normalization.replacements` rules.
- Edit language-level `settings:` key/value pairs (existing ones or add new knobs).
- Preview a phoneme (synthesizes a short clip and plays it).
- Speak / save WAV from IPA via `nvspFrontend.dll`.

## Notes

- YAML comments and original formatting are not preserved when saving.
  Use git to review diffs.
- For the frontend DLL to work, your pack root must contain `packs/phonemes.yaml`.
