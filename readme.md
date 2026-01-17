# NV Speech Player

A Klatt-based speech synthesis engine written in C++.

Author: NV Access Limited, with contributions and improvements by Tamas Geczy

## Maintenance Note
NV Access is no longer maintaining this project. If you make use of this project or find it interesting, and you have the time and expertise to maintain it, please feel free to fork it and let us know you are interested in taking it on.

This includes the SpeechPlayer core itself, plus the nvSpeechPlayer NVDA add-on also in this repository.

Note that the eSpeak-ng/espeak-ng project also includes a copy of the SpeechPlayer code as an alternative Klatt implementation.

## Overview
NV Speech Player is a free and open-source prototype speech synthesizer that can be used by NVDA. It generates speech using Klatt synthesis, making it somewhat similar to speech synthesizers such as Dectalk and Eloquence.

Historically, NV Speech Player relied on Python (`ipa.py`) to:
- normalize phonemes (mostly from eSpeak IPA),
- apply language/dialect rules,
- generate timed frame tracks and intonation,
- and feed frames into the C++ DSP engine.

This repo has now transitioned to a new **frontend + YAML packs** model that replaces the Python IPA pipeline for runtime use.

## License and copyright
NV Speech Player is Copyright (c) 2014 NV Speech Player contributors.

NV Speech Player is covered by the GNU General Public License (Version 2).

You are free to share or change this software in any way you like as long as it is accompanied by the license and you make all source code available to anyone who wants it. This applies to both original and modified copies of this software, plus any derivative works.

For further details, you can view the license online at:
http://www.gnu.org/licenses/old-licenses/gpl-2.0.html

## Background
The 70s and 80s saw much research in speech synthesis. One of the most prominent synthesis models that appeared was a formant-frequency synthesis known as Klatt synthesis. Some well-known Klatt synthesizers are Dectalk and Eloquence. They are well suited for use by the blind because they are responsive, predictable, and small in memory footprint.

Research later moved toward other synthesis approaches because they can sound closer to a human voice, but they often trade away responsiveness and predictability.

NV Speech Player exists as a modern prototype of a Klatt synthesizer:
- to explore the “classic” fast-and-stable sound profile,
- and to keep conversation and experimentation alive around this synthesis method.

## Repository layout
At a high level, the project is split into two layers:

1. **DSP engine (C++)**: `speechPlayer.dll`
2. **Frontend (C++)**: `nvspFrontend.dll` + YAML packs in `./packs`

The NVDA add-on loads both DLLs and uses eSpeak for text → IPA conversion.

### packs directory (important)
Language packs live in this repository at:

`./packs`

This folder is meant to be included in the NVDA add-on distribution as well.

Current structure:
- `packs/phonemes.yaml`
- `packs/lang/default.yaml`
- `packs/lang/en.yaml`, `packs/lang/en-us.yaml`, etc.

## DSP pipeline internals (speechPlayer.cpp + speechWaveGenerator.cpp)
At the highest level, `speechPlayer.cpp` wires together the frame queue and the DSP generator:
- `speechPlayer_initialize()` builds a `FrameManager` plus a `SpeechWaveGenerator` and connects them so the generator can pull the current frame data as it produces audio samples.
- `speechPlayer_queueFrame()` pushes time-aligned frame data into the `FrameManager`, including minimum frame duration, fade time, and a user index for tracking.
- `speechPlayer_synthesize()` asks the wave generator for the next block of samples, which is where all the DSP happens.
- `speechPlayer_getLastIndex()` lets the caller know which queued frame index was last consumed by the renderer.

The DSP pipeline lives in `speechWaveGenerator.cpp` and is executed once per output sample:
1. **Frame selection and interpolation:** `FrameManager::getCurrentFrame()` returns the current frame, or interpolates between frames using the configured fade time. This is how crossfades, pitch glides, and silence frames work.
2. **Source generation (voicing + aspiration):**
   - `VoiceGenerator` turns `voicePitch` into a periodic waveform, applies vibrato, and mixes in turbulence.
   - `aspirationAmplitude` adds breath noise.
3. **Cascade formant path:** The voiced source is shaped by a cascade of resonators (`cf1..cf6` with `cb1..cb6`), with optional nasal coupling (`cfN0/cfNP`, `cbN0/cbNP`, `caNP`).
4. **Parallel frication path:** A separate noise source (`fricationAmplitude`) is passed through parallel resonators (`pf1..pf6`, `pb1..pb6`, `pa1..pa6`). `parallelBypass` mixes raw noise against the resonated output.
5. **Mix and scale:** Cascade + parallel outputs are mixed, scaled by `preFormantGain` and `outputGain`, and clipped to 16-bit range before being returned to the caller.

This structure keeps the time-domain synthesis logic entirely in C++: callers provide timed frame tracks, while the engine interpolates and renders them into audio.

## The new frontend model (nvspFrontend.dll + YAML packs)
The new frontend replaces the Python IPA runtime pipeline. It is designed so that language changes can happen as data (YAML) rather than code.

### What the frontend does
Given an IPA string and a language tag, the frontend:
- normalizes IPA using YAML rules (`packs/lang/*.yaml`)
- tokenizes IPA (stress marks, length marks, tie bars, etc.)
- applies timing rules (vowels vs stops vs affricates, stress scaling, length marks)
- applies intonation and pitch shaping
- emits timed frames compatible with `speechPlayer_queueFrame()`

### Why YAML packs matter
YAML packs are intended to be community-friendly:
- easier to read and review than embedded code,
- allows language/dialect refinements without recompiling the DLL,
- keeps “phoneme sound definitions” separate from “language mapping rules”.

This also makes it easier to share improvements:
- phoneme tuning in `phonemes.yaml`,
- language and dialect behavior in `packs/lang/<lang>.yaml` and `packs/lang/<lang-region>.yaml`.

### Language pack loading and inheritance
The loader merges packs in this order:
1. `packs/lang/default.yaml`
2. `packs/lang/<lang>.yaml` (example: `en.yaml`)
3. `packs/lang/<lang-region>.yaml` (example: `en-us.yaml`)
4. optional deeper variants (example: `en-us-nyc.yaml`)

This is how dialect differences can be expressed even when upstream IPA does not mark them clearly.

### phonemes.yaml
`packs/phonemes.yaml` defines how each phoneme maps to Klatt-style frame parameters. Keys are IPA symbols or internal symbols (we use some private keys like `ᴇ`, `ᴒ`, etc. for language-specific tuning).

Example (shortened):
```yaml
phonemes:
  "0":
    _isVowel: true
    _isVoiced: true
    voiceAmplitude: 0.9
    cf1: 500
    cf2: 1400
    cf3: 2300
    cb1: 110
    cb2: 45
    cb3: 82.5
```

### Language YAML example (en.yaml)
Example (shortened):
```yaml
settings:
  stopClosureMode: always
  postStopAspirationEnabled: true

normalization:
  classes:
    BATH_FOLLOW: ["s", "f", "θ"]
  replacements:
    - from: "a"
      to: "æ"
    - from: "æ"
      to: "ɑː"
      when:
        beforeClass: BATH_FOLLOW
```

## Legacy Python files (kept for reference)
`ipa.py` and `data.py` still live alongside the repo for now, but they are no longer the runtime path for NVDA.

They are retained for:
- historical reference,
- comparing behavior during development,
- and as a source for generating YAML.

If you want to tune phonemes or language rules going forward, update YAML packs instead.

## How to add or tune phonemes (data.py reference + YAML packs)
`data.py` is a dictionary: keys are IPA symbols (like `a`, `ɚ`, `t͡ʃ`, `ᴒ`, etc.) and values are parameter sets that describe how the formant synthesizer should shape that sound. While the runtime path is now YAML-based, the same concepts apply. Be sure to match any tuning with the equivalent entries in `packs/phonemes.yaml`, and map usage in `packs/lang/*.yaml` so your changes are actually used.

### Adding a new phoneme (recommended workflow)
1. **Pick a key**
   - Use a real IPA symbol if possible (`ɲ`, `ʎ`, `ɨ`, …).
   - If you need a language-specific variant, use a private/internal key (we use things like `ᴒ`, `ᴇ`, `ᴀ`, `ᴐ`).
2. **Clone the closest existing phoneme**
   - Copy an existing entry and adjust it.
   - This is important: the engine expects most fields to exist. A “minimal” entry can crash.
3. **Tune it**
   - Start by adjusting formant center frequencies (`cf1`, `cf2`, `cf3`).
   - Then adjust bandwidths (`cb1`, `cb2`, `cb3`) if it sounds “boxy/ringy”.
   - Only then adjust frication/aspiration settings.
4. **Wire it up in language-specific YAML**
   - Make sure `normalizeIPA()` (legacy path) or the YAML normalization rules output your new key for the right language/case.
   - If you don’t map it, the phoneme will never be used.

### Parameter reference (what the fields mean)
#### Phoneme type flags (metadata)
These fields are used by timing rules and by a few special cases:
- `_isVowel`: This is a vowel (timed longer, can be lengthened with `ː`).
- `_isVoiced`: Voiced (uses `voiceAmplitude`).
- `_isStop`: Stop consonant (very short; may get a silence gap).
- `_isNasal`: Nasal consonant or nasal vowel coupling.
- `_isLiquid`: l/r-like sounds (often get longer fades).
- `_isSemivowel`: Glides like j/w.
- `_isTap`, `_isTrill`: Very short rhotic types.
- `_isAfricate`: Affricate (timed like a stop+fricative).

#### Core formant synthesizer knobs
Think of a vowel as resonances (formants). The important ones are F1–F3.

**Formant center frequencies** define where the resonances are (in Hz-ish units):
- `cf1`, `cf2`, `cf3`, `cf4`, `cf5`, `cf6` — “Cascade” formant frequencies. F1–F3 matter most for vowel identity.
- `pf1`, `pf2`, `pf3`, `pf4`, `pf5`, `pf6` — “Parallel” formant frequencies. Usually matched to the `cf*` values.

Quick intuition:
- Higher `cf1` → more open mouth (e.g. “ah”)
- Higher `cf2` → more front / brighter (e.g. “ee”)
- Lower `cf2` → more back / rounder (“oo”)
- Lower `cf3` → more “r-colored” (rhotic vowels)

**Bandwidths** define how wide each resonance is:
- `cb1..cb6` and `pb1..pb6`

Quick intuition:
- Narrow bandwidth (small numbers) → very “ringy / boxy / hollow”
- Wider bandwidth → smoother / less resonant / less “plastic box”

If something sounds “boxy”, widening `cb2`/`cb3` (and matching `pb2`/`pb3`) is often the first fix.

#### Amplitude / mixing controls
- `voiceAmplitude`: Strength of voicing. Lower it slightly if vowels feel “over-held” or harsh.
- `fricationAmplitude`: Noise level for fricatives (`s`, `ʃ`, `f`, `x`, etc.). If “s” is too hissy, reduce this.
- `aspirationAmplitude`: Breath noise used for aspirated/“h-like” behavior. Usually 0 for vowels.
- `parallelBypass`: Mix control between cascade and parallel paths. Most phonemes keep this at 0.0 unless you know you need it.
- `pa1..pa6`: Per-formant amplitude in the parallel path. Most entries keep these at 0.0. If a diphthong glide is too weak, a tiny `pa2`/`pa3` boost can help.

#### Nasal coupling (optional)
Some entries include:
- `cfN0`, `cfNP`, `cbN0`, `cbNP`, `caNP`

These relate to nasal resonance and coupling. We currently treat nasality conservatively; if you don’t know what to do, clone from an existing nasal vowel/consonant entry.

### Practical tuning tips (fast wins)
“This vowel sounds too much like another vowel”
- Adjust `cf1` and `cf2` first.
- Example: Hungarian short `a` vs long `á`: make short `a` lower `cf1` and lower `cf2` compared to `á`.

“This vowel is boxy / hollow / plastic”
- Widen `cb2`/`cb3` (and `pb2`/`pb3`) a bit.

“This sound is too sharp/hissy”
- Lower `fricationAmplitude`.

“This rhotic vowel (`ɚ`/`ɝ`) is too thick”
- Raise `cf3` slightly (less r-color) or widen `cb3`.

## Generating phonemes.yaml from Python data
If you are migrating from the legacy data dictionary, use the conversion tool in `tools/` to regenerate `packs/phonemes.yaml`.

(Exact script name may vary depending on the branch; check `tools/` for the converter.)

## Adding or tuning phonemes (recommended workflow)
1. Add or adjust a phoneme in `packs/phonemes.yaml`
   - Clone the closest phoneme and tweak it.
   - Keep type flags consistent (`_isVowel`, `_isStop`, `_isVoiced`, etc.) because timing rules rely on them.
2. Map it into use via language normalization
   - Add replacements/aliases in `packs/lang/<lang>.yaml` so the phoneme appears in the output stream.
3. Test
   - Compare a few representative words/phrases in NVDA across languages/dialects.

## Adding a new language
1. Create `packs/lang/<lang>.yaml`
2. Add any IPA normalizations needed for that language
3. Add or tune phonemes in `packs/phonemes.yaml` (or add internal phoneme keys)
4. If the language has special prosody needs:
   - start by adjusting settings (stress scaling, closure behavior)
   - only add new engine behavior if the data model truly can’t express it

## Tonal languages (Chinese, Vietnamese, etc.)
Tonal languages are supported by treating tone as an optional overlay on top of the existing pitch model:
- enable `settings.tonal: true`
- define tone contours (digits or tone letters) in the language pack

This lets new tonal behavior be added mostly as YAML, while still allowing deeper future work (tone sandhi) if needed.

## Building
This repo can be built with CMake to produce:
- `speechPlayer.dll`
- `nvspFrontend.dll`

Example:
```bat
cmake -S . -B build-win32
cmake --build build-win32 --config Release
```

The NVDA add-on build process packages:
- the DLLs,
- and the `packs/` directory.

## NVDA add-on
The NVDA driver loads:
- `speechPlayer.dll` (DSP engine)
- `nvspFrontend.dll` (IPA → timed frames)
- `packs/` (YAML configuration)
- and uses eSpeak for text → IPA conversion.

This keeps the DSP core small and stable while making language updates fast and community-editable.

If you want, I can also:
- add a short “Troubleshooting” section (common gotchas: pack path, YAML casing, dialect inheritance),
- and add a “Contributing language fixes” section with a simple checklist (so PRs are consistent).
