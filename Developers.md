# TGSpeechBox – Developer Documentation

This document covers the full public API surface for TGSpeechBox: the DSP engine (`speechPlayer.dll`), the frontend (`nvspFrontend.dll`), platform bridge APIs, and supporting architecture.

## Architecture overview

TGSpeechBox is split into three layers:

1. **DSP engine** (`speechPlayer.dll` / `libspeechPlayer.so`) — Pure C++ formant synthesizer. Takes timed frame data, produces 16-bit PCM audio. No text processing, no phoneme knowledge. MIT licensed.

2. **Frontend** (`nvspFrontend.dll` / `libnvspFrontend.so`) — C++ library with a C API. Takes IPA strings + language tag, applies normalization, allophone rules, timing, pitch contouring, and emits timed frames for the DSP. Loads YAML language packs from `packs/`. MIT licensed.

3. **Phonemizer** (eSpeak-NG) — Converts text to IPA. GPL-3.0 licensed. Callers link against it separately; on iOS it runs in an XPC service process to maintain a clean GPL/MIT boundary.

Typical integration flow:
```
Text → eSpeak (text→IPA) → Frontend (IPA→frames) → DSP (frames→PCM)
```

On all platforms, the caller is responsible for feeding text to eSpeak and passing the resulting IPA to the frontend. The frontend emits frames via a callback, which the caller queues into the DSP engine.

---

## DSP engine API (`src/speechPlayer.h`)

### Core functions

```c
speechPlayer_handle_t speechPlayer_initialize(int sampleRate);

void speechPlayer_queueFrame(
    speechPlayer_handle_t handle,
    speechPlayer_frame_t* frame,
    unsigned int minFrameDuration,   // minimum samples before next frame
    unsigned int fadeDuration,       // crossfade samples
    int userIndex,                   // passed back via getLastIndex
    bool purgeQueue                  // true = clear queue first
);

void speechPlayer_queueFrameEx(
    speechPlayer_handle_t handle,
    speechPlayer_frame_t* frame,
    const speechPlayer_frameEx_t* frameEx,  // NULL = same as queueFrame
    unsigned int frameExSize,               // sizeof caller's frameEx struct
    unsigned int minFrameDuration,
    unsigned int fadeDuration,
    int userIndex,
    bool purgeQueue
);

int speechPlayer_synthesize(
    speechPlayer_handle_t handle,
    unsigned int sampleCount,
    sample* sampleBuf               // output buffer (16-bit signed)
);

int speechPlayer_getLastIndex(speechPlayer_handle_t handle);

void speechPlayer_terminate(speechPlayer_handle_t handle);
```

### Extended functions

These are safe ABI extensions — old drivers that never call them get identical behavior to before.

```c
// Voicing tone: global voice quality (glottal pulse shape, spectral tilt, EQ)
void speechPlayer_setVoicingTone(speechPlayer_handle_t handle,
                                  const speechPlayer_voicingTone_t* tone);
void speechPlayer_getVoicingTone(speechPlayer_handle_t handle,
                                  speechPlayer_voicingTone_t* tone);

// Output gain applied before the soft-knee limiter.
// Default 1.0. Typical: NVDA=1.0, iOS=1.7, Android=1.6.
void speechPlayer_setOutputGain(speechPlayer_handle_t handle, double gain);

// DSP version detection (currently returns 8)
unsigned int speechPlayer_getDspVersion(void);

// Pitch-synchronous time-stretch for rate boost beyond 2x.
// 1.0 = normal. 2.0 = skip every other glottal cycle. Clamped to 1.0–8.0.
void speechPlayer_setTimeStretch(speechPlayer_handle_t handle, double factor);
```

### DLL exports (`speechPlayer.def`)

```
speechPlayer_initialize
speechPlayer_queueFrame
speechPlayer_queueFrameEx
speechPlayer_synthesize
speechPlayer_getLastIndex
speechPlayer_terminate
speechPlayer_setVoicingTone
speechPlayer_getVoicingTone
speechPlayer_getDspVersion
speechPlayer_setOutputGain
speechPlayer_setTimeStretch
```

---

## DSP pipeline internals

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

---

## Frame struct (`src/frame.h`)

The core frame has **47 parameters** and has been ABI-stable since v1:

```c
typedef struct {
    double voicePitch;              // fundamental frequency (Hz)
    double vibratoPitchOffset;      // pitch offset in fraction of semitone
    double vibratoSpeed;            // vibrato rate (Hz)
    double voiceTurbulenceAmplitude; // voice breathiness (0–1)
    double glottalOpenQuotient;     // fraction of cycle glottis is open (0–1)
    double voiceAmplitude;          // voicing source amplitude (0–1)
    double aspirationAmplitude;     // aspiration source amplitude (0–1)

    double cf1, cf2, cf3, cf4, cf5, cf6, cfN0, cfNP; // cascade formant frequencies (Hz)
    double cb1, cb2, cb3, cb4, cb5, cb6, cbN0, cbNP; // cascade formant bandwidths (Hz)
    double caNP;                    // nasal pole coupling amplitude (0–1)

    double fricationAmplitude;      // frication noise amplitude (0–1)
    double pf1, pf2, pf3, pf4, pf5, pf6; // parallel formant frequencies (Hz)
    double pb1, pb2, pb3, pb4, pb5, pb6; // parallel formant bandwidths (Hz)
    double pa1, pa2, pa3, pa4, pa5, pa6; // parallel formant amplitudes (0–1)
    double parallelBypass;          // fraction bypassing parallel resonators (0–1)

    double preFormantGain;          // pre-resonator gain (0–1), 0 = silence
    double outputGain;              // master volume (0–1)
    double endVoicePitch;           // pitch target at end of frame (Hz)
} speechPlayer_frame_t;
```

---

## VoicingTone struct (`src/voicingTone.h`)

Global voice quality parameters that shape the glottal pulse, spectral character, and vocal tract geometry. These persist across frames and are typically set once per voice profile.

The struct contains **19 doubles** plus a version detection header. DSP version: **8**. Struct version: **5**.

### Version detection header

| Field | Type | Description |
|-------|------|-------------|
| `magic` | uint32 | Magic number `0x32544F56` ("VOT2" in little-endian) |
| `structSize` | uint32 | Size of the struct in bytes |
| `structVersion` | uint32 | Struct version (currently 5; bumps when fields are added — V1=1, V2=2, V3=3, V4=4, V5=5) |
| `dspVersion` | uint32 | DSP version (currently 8). See the version-history table at the end of FrameEx for what each bump introduced. |

When the DLL receives a `VoicingTone` struct, it checks the magic number:
- **If magic matches**: Reads up to `structSize` bytes, applying defaults for any trailing fields not present
- **If magic doesn't match**: Assumes legacy v1 layout (7 doubles) and applies defaults for new parameters

This allows older drivers to continue working with newer DLLs, and vice versa.

### Core parameters (V1, original 7)

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `voicingPeakPos` | double | 0.91 | 0.85–0.95 | Glottal pulse peak position. Lower = more pressed/tense; higher = more breathy/relaxed. |
| `voicedPreEmphA` | double | 0.92 | 0.0–0.97 | Pre-emphasis filter coefficient. Higher = more high-frequency boost before formant filtering. |
| `voicedPreEmphMix` | double | 0.35 | 0.0–1.0 | Mix between original and pre-emphasized signal. |
| `highShelfGainDb` | double | 5.5 | -12 to +12 | High-shelf EQ gain in dB. Positive = brighter; negative = darker. |
| `highShelfFcHz` | double | 2000.0 | 500–8000 | High-shelf corner frequency in Hz. |
| `highShelfQ` | double | 0.7 | 0.3–2.0 | High-shelf Q factor. Higher = more resonant shelf transition. |
| `voicedTiltDbPerOct` | double | 0.0 | -24 to +24 | Spectral tilt in dB/octave. Negative = darker (normal for speech); positive = brighter. |

### V2 parameters (DSP version 4+)

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `noiseGlottalModDepth` | double | 0.0 | 0.0–1.0 | Modulates noise sources by the glottal cycle (Klatt-style AM). |
| `pitchSyncF1DeltaHz` | double | 0.0 | -60 to +60 | Pitch-synchronous F1 modulation during glottal open phase. |
| `pitchSyncB1DeltaHz` | double | 0.0 | -50 to +50 | Pitch-synchronous B1 modulation during glottal open phase. |

### V3 parameters (DSP version 5+)

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `speedQuotient` | double | 2.0 | 0.5–4.0 | Glottal pulse asymmetry. Lower (0.5–1.5) = softer/female-like; higher (2.5–4.0) = sharper/male/pressed. |
| `aspirationTiltDbPerOct` | double | 0.0 | -12 to +12 | Spectral tilt for aspiration noise, independent of `voicedTiltDbPerOct`. |
| `cascadeBwScale` | double | 0.9 | 0.3–2.0 | Global cascade formant bandwidth multiplier. Lower = sharper formant peaks; higher = softer/more muffled. |
| `tremorDepth` | double | 0.0 | 0.0–0.5 | Vocal tremor depth. A ~5.5 Hz LFO modulates amplitude for elderly/shaky voice. Stacks with jitter/shimmer. |

### V4 parameters (DSP version 6+) — vocal tract shape

These model structural vocal tract differences (pharynx length, nasal passages) rather than source characteristics.

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `nasalBwScale` | double | 1.0 | 0.5–2.0 | Nasal resonator bandwidth multiplier. Higher = more damped nasal resonances (female character). |
| `f4FreqScale` | double | 1.0 | 0.7–1.5 | F4 frequency multiplier. Shorter pharynx (female/child) raises F4. One of the strongest cues for perceived vocal tract size. |
| `nasalGainScale` | double | 1.0 | 0.5–1.5 | Nasal pole coupling amplitude multiplier. Higher = more nasality. |

### V5 parameters (DSP version 8+) — dual-oscillator chorus

Models vocal fold asymmetry by running a second glottal phase accumulator at a slightly detuned pitch and blending it with the primary oscillator. Real vocal folds are not perfectly symmetric, producing cycle-to-cycle variation that this approximates.

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `chorusDepth` | double | 0.0 | 0.0–1.0 | Blend amount of the second oscillator. 0 = off (single oscillator), 1.0 = full 50/50 blend. |
| `chorusDetuneHz` | double | 2.0 | 0.5–5.0 | Pitch offset of the second oscillator in Hz. Lower = slower beating (warm); higher = faster beating. |

---

## FrameEx struct (DSP version 5+)

Optional per-frame extension for voice quality parameters that vary during speech (e.g., Danish stod, diphthong formant sweeps, Fujisaki pitch contours). This keeps the original 47-parameter frame ABI stable.

The struct is currently **29 doubles = 232 bytes**. The `speechPlayer_queueFrameEx()` function takes a `frameExSize` parameter; the DSP starts with defaults then overlays `min(frameExSize, sizeof(speechPlayer_frameEx_t))` bytes. This provides forward/backward ABI compatibility — callers with smaller structs simply don't override the trailing fields.

The five FrameEx mirrors (in C/C++ headers, render tool, and two Python ctypes copies) are codegen'd from `src/frame.h` via `tools/gen_frame_ex.py`. CMake runs `--check` on every build; drift fails the build with a regen hint. Add a field by editing `src/frame.h` and running the script.

### Voice quality (DSP v5+)

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `creakiness` | double | 0.0 | 0.0–1.0 | Laryngealization / creaky voice. Adds pitch irregularity, amplitude reduction, tighter glottal closure. |
| `breathiness` | double | 0.0 | 0.0–1.0 | Additional voiced breathiness, independent of `voiceTurbulenceAmplitude`. |
| `jitter` | double | 0.0 | 0.0–1.0 | Pitch perturbation (cycle-to-cycle F0 variation). |
| `shimmer` | double | 0.0 | 0.0–1.0 | Amplitude perturbation (cycle-to-cycle intensity variation). |
| `sharpness` | double | 0.0 | 0.0–15.0 | Glottal closure sharpness multiplier. 0 = use sample-rate default. 0.5–2.0 typical slider range. |

### Formant end targets (DSP v6+)

These enable within-frame formant ramping for smoother CV transitions:

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `endCf1` | double | NAN | End target for cascade F1 (Hz). NAN = no ramping. |
| `endCf2` | double | NAN | End target for cascade F2 (Hz). NAN = no ramping. |
| `endCf3` | double | NAN | End target for cascade F3 (Hz). NAN = no ramping. |
| `endPf1` | double | NAN | End target for parallel F1 (Hz). NAN = no ramping. |
| `endPf2` | double | NAN | End target for parallel F2 (Hz). NAN = no ramping. |
| `endPf3` | double | NAN | End target for parallel F3 (Hz). NAN = no ramping. |

### Fujisaki pitch model (DSP v6+)

Natural-sounding intonation with phrase-level declination and accent peaks. Used when `legacyPitchMode: "fujisaki_style"` is set in the language pack. All time units are in **samples**, not milliseconds.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `fujisakiEnabled` | double | 0.0 | 1.0 to enable Fujisaki processing, 0.0 to disable. |
| `fujisakiReset` | double | 0.0 | 1.0 to reset model state (at clause start). |
| `fujisakiPhraseAmp` | double | 0.0 | Phrase command amplitude (log-F0 domain). Triggers phrase arc on rising edge. |
| `fujisakiPhraseLen` | double | 0.0 | Phrase filter length in samples. 0 = use DSP default (~4250 @ 22050 Hz). |
| `fujisakiAccentAmp` | double | 0.0 | Accent command amplitude (log-F0 domain). Triggers accent peak on rising edge. |
| `fujisakiAccentDur` | double | 0.0 | Accent pulse duration in samples. 0 = use DSP default (~7500 @ 22050 Hz). |
| `fujisakiAccentLen` | double | 0.0 | Accent filter length in samples. 0 = use DSP default (~1024 @ 22050 Hz). |

These parameters are interpolated during frame crossfades, just like the core frame parameters.

### Per-parameter transition speed scales (DSP v7+)

Control how fast individual formant groups reach their targets during boundary crossfades. The boundary smoothing pass sets these on tokens based on language pack settings. A scale value < 1.0 means "reach the target in that fraction of the fade time, then hold." 0.0 means no override.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `transF1Scale` | double | 0.0 | Transition speed for F1 group (cf1, pf1, cb1, pb1). |
| `transF2Scale` | double | 0.0 | Transition speed for F2 group (cf2, pf2, cb2, pb2). |
| `transF3Scale` | double | 0.0 | Transition speed for F3 group (cf3, pf3, cb3, pb3). |
| `transNasalScale` | double | 0.0 | Transition speed for nasal group (cfN0, cfNP, cbN0, cbNP, caNP). |

Example: with `transF1Scale = 0.6` and a 20ms fade, F1 reaches its target at 12ms and holds for the remaining 8ms, while F2 ramps across the full 20ms. This makes F1 "arrive first" at segment boundaries, which is perceptually important for place-of-articulation cues.

### Equal-power amplitude crossfade (DSP v7+)

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `transAmplitudeMode` | double | 0.0 | Amplitude crossfade curve: 0.0 = linear (legacy), 1.0 = equal-power (sin/cos). |

When the voicing source type changes between frames (e.g. voiced /n/ → voiceless /h/), linear crossfade creates an energy valley at the midpoint (~58%). Equal-power crossfade uses `oldVal * cos(theta) + newVal * sin(theta)` to maintain constant total energy.

The DSP applies equal-power only to source amplitude parameters (`voiceAmplitude`, `aspirationAmplitude`, `fricationAmplitude`, `voiceTurbulenceAmplitude`, `preFormantGain`). Parallel amplitudes (`pa1`-`pa6`), `outputGain`, and `caNP` are excluded — they track formant structure, not energy sources.

The frontend (`frame_emit.cpp`) sets this automatically when it detects a voicing source change (amplitude crosses a 0.05 threshold in either direction).

### Higher cascade formants F7/F8 (DSP v8)

At sample rates >= 22050 Hz, the original 6 cascade formants leave a spectral gap above F6 (~5.5 kHz). Adding explicit F7 and F8 as fixed cascade resonators fills this gap with the correct spectral envelope, adding natural "presence" and "air." Defaults from Rabiner 1968, as cited in the [QLatt project](https://github.com/nicclase/qlatt).

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `cf7` | double | 6500.0 | F7 frequency (Hz). Vocal-tract-length property, not vowel-dependent. |
| `cb7` | double | 720.0 | F7 bandwidth (Hz). |
| `cf8` | double | 7500.0 | F8 frequency (Hz). |
| `cb8` | double | 1250.0 | F8 bandwidth (Hz). |

Per-phoneme overridable via YAML keys `cf7`, `cb7`, `cf8`, `cb8`. At low sample rates, the cascade Nyquist-proximity fade automatically mutes them (ratio > 0.85 -> bypass), so they cost nothing at 11025/16000 Hz.

Cascade order: N0 -> NP -> F8 -> F7 -> F6 -> F5 -> F4 -> F3 -> F2 -> F1.

### Source amplitude timing (DSP v8)

Per-parameter onset/offset timing for noise sources and voicing during frame crossfades. Together these create natural temporal structure for stop releases and affricates — the noise burst can hold while voicing ramps in independently, mimicking real glottal coordination.

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `transSourceHoldRatio` | double | 0.0 | 0.0–0.5 | Delays fadeout of old noise sources (`fricationAmplitude`, `aspirationAmplitude`) during crossfades. 0.3 = old noise holds for first 30% of fade, then fades over the remaining 70%. `voiceAmplitude` is unaffected. |
| `transVoicingHoldRatio` | double | 0.0 | 0.0–0.5 | Delays ramp-in of new `voiceAmplitude` during crossfades. 0.25 = voicing stays at the old value for the first 25% of the fade, then ramps in over the remaining 75%. |

Combined example: `sourceHold=0.40, voicingHold=0.25` produces:
- 0–25%: frication HIGH, voicing ZERO  (pure affricate release)
- 25–40%: frication HIGH, voicing ramping (overlap)
- 40–100%: frication fading, voicing ramping (transition)

The boundary smoothing pass (`boundary_smoothing.cpp`) sets these on tokens based on transition type. Research on natural affricate production motivated the per-parameter scheme.

### 2x source oversampling (DSP v8)

The glottal LF model is evaluated at twice the output sample rate, using sharper closure settings appropriate for the effective 2x rate, then decimated with a half-band average filter. This produces cleaner harmonics at lower sample rates without aliasing.

| Output SR | Effective source SR | Before | After |
|-----------|---------------------|--------|-------|
| 11025 Hz | 22050 Hz | sharpness 2.5, 30% LF | sharpness 3.5, 100% LF |
| 16000 Hz | 32000 Hz | sharpness 3.0, 100% LF | sharpness 3.5, 100% LF |
| 22050 Hz | 44100 Hz | sharpness 4.0, 100% LF | sharpness 4.5, 100% LF |
| 44100 Hz | (no oversampling) | sharpness 6.0, 100% LF | unchanged |

The oversampling is bypassed at 44100+ Hz where the existing sharpness is already clean.

### Silence transition fixes (DSP v7+)

Two fixes prevent pops at silence boundaries:

1. **Source amplitude zeroing** (`frame.cpp`): When transitioning from/to silence, the "from" side has all source amplitudes zeroed alongside `preFormantGain`. Previously, the silence path copied target amplitudes into the old frame, hitting resonators at full strength from sample 1.

2. **Resonator reset on preFormantGain recovery** (`speechWaveGenerator.cpp`): When `smoothPreGain` rises from near-zero to above 0.01, cascade and parallel resonators are reset. This clears residual IIR state from the previous phoneme across word-boundary gaps.

**ABI note:** The struct grows by appending only — older callers with smaller `frameExSize` silently get DSP defaults for any trailing fields they omit. The DSP v7 transition fields sit at indices 18–22, the DSP v8 cf7/cb7/cf8/cb8 at 23–26, and `transSourceHoldRatio`/`transVoicingHoldRatio` at 27–28.

### Version history

| DSP version | VoicingTone struct | FrameEx fields added | What landed |
|-------------|--------------------|----------------------|-------------|
| 4 | V2 (10 doubles) | — | Pitch-sync F1/B1 modulation, Klatt-style noise AM |
| 5 | V3 (14 doubles) | initial 18 (creakiness, breathiness, jitter, shimmer, sharpness, endCf/Pf, Fujisaki) | Speed quotient, aspiration tilt, cascade BW scale, tremor; FrameEx introduced |
| 6 | V4 (17 doubles) | — | Vocal tract shape: nasalBwScale, f4FreqScale, nasalGainScale |
| 7 | V4 | +transF1/F2/F3/NasalScale + transAmplitudeMode (→23) | Per-parameter formant transition timing, equal-power crossfade |
| **8** | **V5 (19 doubles)** | **+cf7/cb7/cf8/cb8 + transSourceHoldRatio + transVoicingHoldRatio (→29)** | **F7/F8 cascade formants, 2x source oversampling, per-source amplitude timing, dual-oscillator chorus** |
| (next) | V6 | (next batch) | DSP v9 — opens when the next functional batch is ready |

**Closing convention:** A DSP version is "closed" once any released build ships with that number set as `SPEECHPLAYER_DSP_VERSION`. After that point, additions go into the *next* version slot — do not retroactively add fields to a closed version, even if the field is logically related, because shipped DLLs already advertise that version with a fixed feature set.

---

## Frontend C API (`src/frontend/nvspFrontend.h`)

The frontend is a C-linkage API exposed from `nvspFrontend.dll` (Windows) or `libnvspFrontend.so` (Linux). It converts IPA strings into timed frame callbacks suitable for the DSP engine.

### ABI versioning

```c
#define NVSP_FRONTEND_ABI_VERSION 5
int nvspFrontend_getABIVersion(void);
```

The ABI version increments when new functions are added. Callers can check this at runtime to gate feature usage. All functions are additive — older functions continue to work unchanged.

| ABI | Key additions |
|-----|---------------|
| 1 | Core: create/destroy, setLanguage, queueIPA |
| 2 | FrameEx: queueIPA_Ex, setFrameExDefaults, voice profiles, voicing tone |
| 3 | Data export: exportData |
| 4 | Text-aware: queueIPA_ExWithText, prepareText, setPitchMode, applySettingOverrides, previewPhoneme, getAvailableLanguages |
| 5 | Generic data query: getDataCount, queryData, setData |

### Lifecycle

```c
// Create a frontend handle. packDirUtf8 should contain the "packs" folder.
nvspFrontend_handle_t nvspFrontend_create(const char* packDirUtf8);

// Destroy and free all resources.
void nvspFrontend_destroy(nvspFrontend_handle_t handle);
```

### Language and configuration

```c
// Load and merge language packs: default.yaml → <lang>.yaml → <lang-region>.yaml
// Returns 1 on success, 0 on failure.
int nvspFrontend_setLanguage(nvspFrontend_handle_t handle, const char* langTagUtf8);

// Set override directory checked before bundle for user-imported packs.
// Used by iOS app group container. Pass NULL to clear.
void nvspFrontend_setOverrideDirectory(nvspFrontend_handle_t handle,
                                        const char* overrideDirUtf8);

// Get available language tags (newline-separated, malloc'd).
// Caller must free with nvspFrontend_freeString().
char* nvspFrontend_getAvailableLanguages(nvspFrontend_handle_t handle);

// Human-readable error message from the last failed call.
const char* nvspFrontend_getLastError(nvspFrontend_handle_t handle);

// Non-fatal warnings from pack loading (e.g., profile parse errors).
const char* nvspFrontend_getPackWarnings(nvspFrontend_handle_t handle);
```

### IPA synthesis

Three callback-based synthesis functions, from simplest to most capable:

```c
// Legacy callback (ABI v1): frame + timing
typedef void (*nvspFrontend_FrameCallback)(
    void* userData,
    const nvspFrontend_Frame* frameOrNull,  // NULL = silence
    double durationMs,
    double fadeMs,
    int userIndex
);

// Extended callback (ABI v2+): frame + frameEx + timing
typedef void (*nvspFrontend_FrameExCallback)(
    void* userData,
    const nvspFrontend_Frame* frameOrNull,
    const nvspFrontend_FrameEx* frameExOrNull,
    double durationMs,
    double fadeMs,
    int userIndex
);

// Basic IPA → frames (ABI v1)
int nvspFrontend_queueIPA(handle, ipaUtf8, speed, basePitch,
                           inflection, clauseTypeUtf8, userIndexBase,
                           FrameCallback cb, userData);

// Extended IPA → frames with FrameEx (ABI v2+)
int nvspFrontend_queueIPA_Ex(handle, ipaUtf8, speed, basePitch,
                              inflection, clauseTypeUtf8, userIndexBase,
                              FrameExCallback cb, userData);

// Text-aware IPA → frames with dictionary lookup (ABI v4+)
// If textUtf8 is provided, the frontend corrects stress placement
// using loaded dictionaries (CMU, compounds, pronunciation overrides).
int nvspFrontend_queueIPA_ExWithText(handle, textUtf8, ipaUtf8, speed,
                                      basePitch, inflection, clauseTypeUtf8,
                                      userIndexBase, FrameExCallback cb, userData);
```

Parameters:
- `speed`: divisor for timing (higher = faster)
- `basePitch`: base F0 in Hz
- `inflection`: pitch range scaling (0.0–1.0)
- `clauseTypeUtf8`: one of `"."` `","` `"?"` `"!"` (NULL treated as `"."`)
- `userIndexBase`: passed through to callback for text position mapping

### Voice quality defaults (ABI v2+)

```c
// Set user-level FrameEx defaults (added to per-phoneme values from YAML).
// Typically called when user adjusts voice quality sliders.
void nvspFrontend_setFrameExDefaults(handle,
    double creakiness, double breathiness,
    double jitter, double shimmer, double sharpness);

// Read current defaults.
int nvspFrontend_getFrameExDefaults(handle, nvspFrontend_FrameEx* outDefaults);
```

### Voice profiles (ABI v2+)

Voice profiles apply formant scaling, voicing tone overrides, and other transformations to produce different voice qualities (female, child, etc.) from a single phoneme table.

```c
// Set profile by name (e.g., "Beth", "Bobby"). NULL or "" to clear.
int nvspFrontend_setVoiceProfile(handle, const char* profileNameUtf8);

// Get current profile name (empty string if none).
const char* nvspFrontend_getVoiceProfile(handle);

// List available profile names (newline-separated).
const char* nvspFrontend_getVoiceProfileNames(handle);

// Get the voicing tone for the current profile.
// Returns 1 if profile has explicit voicingTone, 0 if using defaults.
int nvspFrontend_getVoicingTone(handle, nvspFrontend_VoicingTone* outTone);

// Save slider values back to phonemes.yaml for a profile.
int nvspFrontend_saveVoiceProfileSliders(handle,
    const char* profileNameUtf8,
    const nvspFrontend_VoiceProfileSliders* sliders);
```

Shipped profiles: **Adam** (base male), **Beth** (female — f4FreqScale, nasalBwScale, speedQuotient adjusted), **Bobby** (child — higher formant scaling).

### Pitch mode (ABI v4+)

```c
// Override pitch intonation model. Persists until next setLanguage().
int nvspFrontend_setPitchMode(handle, const char* modeUtf8);

// Override legacy pitch inflection scale (only affects "legacy" mode).
void nvspFrontend_setLegacyPitchInflectionScale(handle, double scale);
```

Available modes:
| Mode | Description |
|------|-------------|
| `"espeak_style"` | ToBI-based intonation regions (default) |
| `"legacy"` | Older time-based pitch curve |
| `"fujisaki_style"` | Flat base + DSP phrase/accent contours |
| `"impulse_style"` | Multi-layer additive pitch |
| `"klatt_style"` | Klatt 1987 hat-pattern intonation |

### Text preparation (ABI v4+)

```c
// Pre-eSpeak text processing: compound word splitting + dictionary replacements.
// Returns NULL if no transforms applied (use original text).
// Returns malloc'd string if transforms fired. Caller frees with freeString().
char* nvspFrontend_prepareText(handle, const char* textUtf8);

// Backwards-compatible alias:
#define nvspFrontend_splitCompounds nvspFrontend_prepareText

void nvspFrontend_freeString(char* str);
```

Use case: feeds corrected text to eSpeak so each compound half is phonemized independently with correct vowel quality. Essential for speech-dispatcher integration where eSpeak is an external process.

### Settings and data export (ABI v3–4)

```c
// Apply YAML snippet overrides on top of loaded language pack.
// Dot-notation supported: "boundarySmoothing.enabled: true"
// Must be called after setLanguage(). Returns 1 on success.
int nvspFrontend_applySettingOverrides(handle, const char* yamlSnippetUtf8);

// Export merged YAML (base file + user overrides) with comment preservation.
// domain: NVSP_DATA_SETTINGS or NVSP_DATA_PHONEMES
// Returns malloc'd YAML string. Caller frees with freeString().
char* nvspFrontend_exportData(handle, int domain,
                               const char* langTagUtf8,
                               const char* overridesJsonUtf8);
```

### Phoneme preview (ABI v4+)

```c
// Preview a single phoneme by key, bypassing the full pipeline.
// Looks up PhonemeDef, emits one steady-state frame to the callback.
// No eSpeak, no allophone rules, no pitch contour.
int nvspFrontend_previewPhoneme(handle, const char* phonemeKeyUtf8,
                                 double pitchHz, double durationMs,
                                 nvspFrontend_FrameExCallback cb, void* userData);
```

### Generic data query API (ABI v5+)

Paginated, typed access to pack data without re-reading YAML from disk. Accepts a language tag so callers can query any language without a disruptive `setLanguage()` switch.

```c
// Data domains
#define NVSP_DATA_SETTINGS   0   // Pack settings (~291 keys)
#define NVSP_DATA_PHONEMES   1   // Phoneme definitions
#define NVSP_DATA_DICTIONARY 2   // Reserved for future

// Count records in a domain. Returns -1 on error.
// For PHONEMES: "" = all base phonemes, "en-gb" = only phonemes
// referenced as replacement targets in that language.
int nvspFrontend_getDataCount(handle, int domain, const char* langTagUtf8);

// Query a page of records as a JSON array string.
// Each element has at least "key", "type", "value" fields.
// Settings also include "group". Phonemes also include "class".
// Returns malloc'd JSON. Caller frees with freeString().
char* nvspFrontend_queryData(handle, int domain, const char* langTagUtf8,
                              int offset, int limit);

// Set a single value. If langTag matches active language, applied
// to in-memory pack immediately (live preview).
// Returns 1 on success, 0 on failure.
int nvspFrontend_setData(handle, int domain, const char* langTagUtf8,
                          const char* keyUtf8, const char* valueUtf8);
```

**queryData JSON format examples:**

Settings:
```json
[{"key": "boundarySmoothing.enabled", "type": "bool", "value": true, "group": "boundarySmoothing"}]
```

Phonemes:
```json
[{"key": "ɪ.cf2", "type": "float", "value": 1750, "group": "ɪ", "class": "vowel"}]
```

### Frontend structs

The frontend defines its own copies of the frame structs for ABI isolation from the DSP DLL. Field order and count match exactly.

- `nvspFrontend_Frame` — 47 doubles, matches `speechPlayer_frame_t`
- `nvspFrontend_FrameEx` — 23 doubles, matches `speechPlayer_frameEx_t`
- `nvspFrontend_VoicingTone` — 17 doubles (no magic header — the frontend handles version negotiation internally)
- `nvspFrontend_VoiceProfileSliders` — 13 doubles: 8 voicing tone sliders + 5 FrameEx sliders. Used by `saveVoiceProfileSliders()`.

---

## NVDA driver sliders

The driver exposes 14 sliders for real-time voice tuning:

### VoicingTone sliders (9 — global voice character)

| Slider | Range | Default | Maps to |
|--------|-------|---------|---------|
| Voice tilt (brightness) | 0–100 | 50 | `voicedTiltDbPerOct` (-24 to +24 dB/oct) |
| Noise glottal modulation | 0–100 | 0 | `noiseGlottalModDepth` (0.0–1.0) |
| Pitch-sync F1 delta | 0–100 | 50 | `pitchSyncF1DeltaHz` (-60 to +60 Hz) |
| Pitch-sync B1 delta | 0–100 | 50 | `pitchSyncB1DeltaHz` (-50 to +50 Hz) |
| Speed quotient (voice tension) | 0–100 | 50 | `speedQuotient` (0.5–4.0) |
| Aspiration tilt (breath color) | 0–100 | 50 | `aspirationTiltDbPerOct` (-12 to +12 dB/oct) |
| Formant sharpness | 0–100 | 50 | `cascadeBwScale` (2.0–0.3, inverted: higher slider = sharper) |
| Voice tremor (shakiness) | 0–100 | 0 | `tremorDepth` (0.0–0.4) |
| Head size (pharynx length) | 0–100 | 50 | `f4FreqScale` (1.25–0.85, inverted: 0 = small/high F4, 100 = large/low F4) |

### FrameEx sliders (5 — per-frame voice quality)

| Slider | Range | Default | Maps to |
|--------|-------|---------|---------|
| Creakiness | 0–100 | 0 | `creakiness` (0.0–1.0) |
| Breathiness | 0–100 | 0 | `breathiness` (0.0–1.0) |
| Jitter | 0–100 | 0 | `jitter` (0.0–1.0) |
| Shimmer | 0–100 | 0 | `shimmer` (0.0–1.0) |
| Glottal sharpness | 0–100 | 50 | `sharpness` (0.5–2.0 multiplier) |

Note: Sliders centered at 50 have meaningful positive and negative ranges. Sliders starting at 0 are effect amounts that default to "off". All slider values are stored per-voice profile.

---

## Frontend model (YAML packs)

The frontend replaces the legacy Python IPA runtime pipeline. Language changes happen as data (YAML) rather than code.

### What the frontend does

Given an IPA string and a language tag, the frontend:
- normalizes IPA using YAML rules (`packs/lang/*.yaml`)
- tokenizes IPA (stress marks, length marks, tie bars, etc.)
- applies allophone rules (context-dependent sound changes)
- applies timing rules (vowels vs stops vs affricates, stress scaling, length marks)
- applies rate compensation (minimum durations, sonorant-context protection)
- applies boundary smoothing (coarticulation at segment boundaries)
- applies intonation and pitch shaping (5 pitch modes)
- emits timed frames compatible with `speechPlayer_queueFrame()`

### Why YAML packs matter

YAML packs are community-friendly:
- easier to read and review than embedded code
- allows language/dialect refinements without recompiling the DLL
- keeps "phoneme sound definitions" separate from "language mapping rules"

Pack structure:
- `packs/phonemes.yaml` — phoneme definitions (formant values, flags, voice profiles)
- `packs/lang/default.yaml` — global defaults
- `packs/lang/<lang>.yaml` — language rules (e.g., `en.yaml`)
- `packs/lang/<lang-region>.yaml` — dialect refinements (e.g., `en-us.yaml`)
- `packs/dict/` — stress dictionaries, compound dictionaries

### Language pack loading and inheritance

The loader merges packs in this order:
1. `packs/lang/default.yaml`
2. `packs/lang/<lang>.yaml` (e.g., `en.yaml`)
3. `packs/lang/<lang-region>.yaml` (e.g., `en-us.yaml`)
4. optional deeper variants (e.g., `en-us-nyc.yaml`)

This is how dialect differences can be expressed even when upstream IPA does not mark them clearly.

### Supported languages (26+)

English (US, GB, AU, CA), German, French, Spanish (ES, MX), Italian, Portuguese (PT, BR), Dutch, Polish, Russian, Ukrainian, Czech, Slovak, Hungarian, Romanian, Croatian, Bulgarian, Swedish, Danish, Finnish, Turkish, Chinese.

---

## Advanced multilingual tokenization

The C++ frontend features a **greedy longest-match tokenizer** for robust multilingual support. Language packs can define multi-character phonemes of arbitrary length without requiring tie bars in the phoneme keys.

| Language | Challenge | Solution |
|----------|-----------|----------|
| **Russian** | Palatalized consonants (`nʲ`, `lʲ`, `mʲ`) are single phonemic units | Greedy matching finds `nʲ` as one token |
| **Polish** | Complex affricates and palatalization | Multi-character keys like `t͡ɕ`, `d͡ʑ` match correctly |
| **Hungarian** | Geminate consonants, unique vowels | Proper handling of length marks and diacritics |
| **English** | Syllabic L/R in "bottle", "butter" | Normalization splits `ə͡l` → `ə` + `l` for proper lateral quality |

### Directional tie-bar flexibility

- Input `n͡ʲ` matches pack key `nʲ` — extra tie bars in input are ignored
- Input `nʲ` matches pack key `nʲ` — exact matches always work
- Input `əl` won't match pack key `ə͡l` — respects normalization decisions

The sorted phoneme key list is computed once during pack loading and stored immutably, making the tokenizer fully thread-safe.

---

## Dictionary system

TGSpeechBox supports a 4-tier dictionary system for pronunciation control:

1. **Pronunciation dictionary** — Text-to-text respellings with optional `to_ipa` field that bypasses eSpeak entirely. Space-delimited phoneme keys for IPA injection.
2. **Stress dictionary** — CMU-style stress patterns. en-us: ~109K entries, en-gb: ~2K entries.
3. **Compound dictionary** — Word splitting for correct stress. ~3,686 English entries.
4. **Character dictionary** — Letter-name overrides for spelling mode.

Dictionary files live in `packs/dict/`. See [Dictionary-editing.md](Dictionary-editing.md) for full documentation.

The frontend's `nvspFrontend_queueIPA_ExWithText()` function enables dictionary lookup by accepting both the original text and eSpeak's IPA output, allowing stress correction and pronunciation overrides.

---

## Platform bridge APIs

### iOS full pipeline (`src/platforms/ios/bridge/tgsb_bridge.h`)

GPL-3.0 (links eSpeak). Wraps eSpeak + frontend + DSP into a simple C API callable from Swift.

```c
TgsbEngine *tgsb_create(const char *espeakDataPath,
                          const char *packDir, int sampleRate);
void tgsb_destroy(TgsbEngine *engine);

// Configuration
void tgsb_set_override_directory(TgsbEngine *engine, const char *overrideDir);
int tgsb_set_language(TgsbEngine *engine, const char *espeakLang,
                       const char *tgsbLang);
int tgsb_set_voice(TgsbEngine *engine, const char *voiceName);

// Synthesis: queue text, pull PCM in a loop until 0 returned
void tgsb_queue_text(TgsbEngine *engine, const char *text,
                      double speed, double pitch);
int tgsb_pull_audio(TgsbEngine *engine, int16_t *outBuffer, int maxSamples);
void tgsb_stop(TgsbEngine *engine);

// Voice quality (11 voicing tone params + 5 FrameEx defaults)
void tgsb_set_voicing_tone(TgsbEngine *engine,
    double voicedTiltDbPerOct, double noiseGlottalModDepth,
    double pitchSyncF1DeltaHz, double pitchSyncB1DeltaHz,
    double speedQuotient, double aspirationTiltDbPerOct,
    double cascadeBwScale, double tremorDepth,
    double nasalBwScale, double f4FreqScale, double nasalGainScale);
void tgsb_set_frame_ex_defaults(TgsbEngine *engine,
    double creakiness, double breathiness,
    double jitter, double shimmer, double sharpness);

// Pitch, inflection, pause mode, sample rate
int tgsb_set_pitch_mode(TgsbEngine *engine, const char *mode);
void tgsb_set_legacy_pitch_inflection_scale(TgsbEngine *engine, double scale);
void tgsb_set_inflection(TgsbEngine *engine, double inflection);
void tgsb_set_pause_mode(TgsbEngine *engine, int mode);  // 0=off, 1=short, 2=long
void tgsb_set_sample_rate(TgsbEngine *engine, int sampleRate);

// Voice profiles
int tgsb_set_voice_profile(TgsbEngine *engine, const char *profileName);
char *tgsb_get_voice_profile_names(TgsbEngine *engine);  // caller free()s

// Data query API (same domains as NVSP_DATA_*)
#define TGSB_DATA_SETTINGS   0
#define TGSB_DATA_PHONEMES   1
#define TGSB_DATA_DICTIONARY 2
int tgsb_get_data_count(TgsbEngine *engine, int domain, const char *langTag);
char *tgsb_query_data(TgsbEngine *engine, int domain, const char *langTag,
                       int offset, int limit);
int tgsb_set_data(TgsbEngine *engine, int domain, const char *langTag,
                   const char *key, const char *value);

// Utilities
char *tgsb_export_data(TgsbEngine *engine, int domain,
                        const char *langTag, const char *overridesJson);
int tgsb_preview_phoneme(TgsbEngine *engine, const char *phonemeKey,
                          double pitchHz, double durationMs);
char *tgsb_text_to_ipa(TgsbEngine *engine, const char *text);
char *tgsb_get_available_languages(TgsbEngine *engine);
void tgsb_free_string(char *str);
```

### iOS synthesis-only (`src/platforms/ios/bridge/tgsb_synth.h`)

MIT licensed. Takes IPA strings, produces PCM. No eSpeak — phonemization happens in the XPC service.

```c
TgsbSynth *tgsb_synth_create(const char *packDir, int sampleRate);
void tgsb_synth_destroy(TgsbSynth *synth);
int tgsb_synth_set_language(TgsbSynth *synth, const char *tgsbLang);
int tgsb_synth_set_voice(TgsbSynth *synth, const char *voiceName);
void tgsb_synth_queue_ipa(TgsbSynth *synth, const char *ipa,
                            double speed, double pitch);
int tgsb_synth_pull_audio(TgsbSynth *synth, int16_t *outBuffer, int maxSamples);
void tgsb_synth_stop(TgsbSynth *synth);
```

### iOS phonemizer XPC (`src/platforms/ios/phonemizer-ext/tgsb_phonemizer.h`)

GPL-3.0. Isolates eSpeak behind a process boundary so the AU extension stays MIT.

```c
int tgsb_phonemizer_init(const char *espeakDataPath);
void tgsb_phonemizer_terminate(void);
int tgsb_phonemizer_set_language(const char *espeakLang);
char *tgsb_phonemizer_phonemize(const char *text);  // caller free()s
```

### Android JNI (`src/platforms/android/jni/tgsb_jni.cpp`)

Two JNI paths exist and must both be updated when adding new parameters:

**TgsbTtsService** — Full pipeline (eSpeak + frontend + DSP) for system-wide TTS. Exposed via Android's `TextToSpeech` framework for TalkBack and other apps.

**TgsbSpeakEngine** — IPA-only synthesis for the standalone app. Also supports text input (routes through eSpeak internally).

Both paths expose: lifecycle, language, voice/profile, voicing tone (11 params as double array), FrameEx defaults, pitch mode, inflection, sample rate, pause mode, volume, data query API.

### SAPI (`src/sapi/`)

The SAPI5 engine is a single **statically linked** `TGSpeechSapi.dll` containing the DSP engine, frontend, and eSpeak. It implements the standard `ISpTTSEngine` COM interface. Settings are persisted in `%APPDATA%\TGSpeechSapi\settings.ini` via a companion settings app (`tools/tgsbSapiSettings/`).

Standard COM exports: `DllCanUnloadNow`, `DllGetClassObject`, `DllRegisterServer`, `DllUnregisterServer`.

---

## Build system

### CMake targets

```bat
# Default build: speechPlayer.dll + nvspFrontend.dll (MIT)
cmake -S . -B build-win32
cmake --build build-win32 --config Release

# SAPI build: single statically-linked TGSpeechSapi.dll (GPL)
cmake -S . -B build-sapi -DTGSB_BUILD_SAPI=ON -DESPEAK_NG_DIR=<path>
cmake --build build-sapi --config Release
```

### Output artifacts

| Target | License | Contents |
|--------|---------|----------|
| `speechPlayer.dll` | MIT | DSP engine only |
| `nvspFrontend.dll` | MIT | Frontend + YAML parser |
| `TGSpeechSapi.dll` | GPL-3.0 | DSP + frontend + eSpeak, statically linked |
| Linux `.so` | MIT | Shared libraries for speech-dispatcher integration |

### NVDA add-on packaging

The add-on includes:
- `speechPlayer.dll` (x86 and x64)
- `nvspFrontend.dll` (x86 and x64)
- `packs/` directory (phonemes.yaml, lang/*.yaml, dict/*.tsv)
- Python driver (`addon/synthDrivers/tgSpeechBox/`)

### GitHub Actions CI

Linux x86_64 and aarch64 builds run on ubuntu-24.04 (GCC 13+ required — GCC 11 on ubuntu-22.04 fails with incomplete type errors on self-referential `Node` with `unordered_map<string, Node>`).

---

## Usage example (Python/ctypes)

```python
from ctypes import Structure, c_double, c_uint32, c_void_p, POINTER, byref, sizeof

# Constants (must match voicingTone.h)
SPEECHPLAYER_VOICINGTONE_MAGIC = 0x32544F56  # "VOT2"
SPEECHPLAYER_VOICINGTONE_VERSION = 3

class VoicingTone(Structure):
    _fields_ = [
        # Version detection header
        ("magic", c_uint32),
        ("structSize", c_uint32),
        ("structVersion", c_uint32),
        ("dspVersion", c_uint32),
        # V1 parameters
        ("voicingPeakPos", c_double),
        ("voicedPreEmphA", c_double),
        ("voicedPreEmphMix", c_double),
        ("highShelfGainDb", c_double),
        ("highShelfFcHz", c_double),
        ("highShelfQ", c_double),
        ("voicedTiltDbPerOct", c_double),
        # V2 parameters
        ("noiseGlottalModDepth", c_double),
        ("pitchSyncF1DeltaHz", c_double),
        ("pitchSyncB1DeltaHz", c_double),
        # V3 parameters
        ("speedQuotient", c_double),
        ("aspirationTiltDbPerOct", c_double),
        ("cascadeBwScale", c_double),
        ("tremorDepth", c_double),
        # V4 parameters — vocal tract shape
        ("nasalBwScale", c_double),
        ("f4FreqScale", c_double),
        ("nasalGainScale", c_double),
    ]

    @classmethod
    def defaults(cls):
        tone = cls()
        tone.magic = SPEECHPLAYER_VOICINGTONE_MAGIC
        tone.structSize = sizeof(cls)
        tone.structVersion = SPEECHPLAYER_VOICINGTONE_VERSION
        tone.dspVersion = 6
        tone.voicingPeakPos = 0.91
        tone.voicedPreEmphA = 0.92
        tone.voicedPreEmphMix = 0.35
        tone.highShelfGainDb = 5.5
        tone.highShelfFcHz = 2000.0
        tone.highShelfQ = 0.7
        tone.voicedTiltDbPerOct = 0.0
        tone.noiseGlottalModDepth = 0.0
        tone.pitchSyncF1DeltaHz = 0.0
        tone.pitchSyncB1DeltaHz = 0.0
        tone.speedQuotient = 2.0
        tone.aspirationTiltDbPerOct = 0.0
        tone.cascadeBwScale = 0.9
        tone.tremorDepth = 0.0
        tone.nasalBwScale = 1.0
        tone.f4FreqScale = 1.0
        tone.nasalGainScale = 1.0
        return tone

# Set up function prototypes
dll.speechPlayer_setVoicingTone.argtypes = [c_void_p, POINTER(VoicingTone)]
dll.speechPlayer_setVoicingTone.restype = None

# Create and configure tone
tone = VoicingTone.defaults()
tone.voicedTiltDbPerOct = -6.0  # Brighter tilt
tone.speedQuotient = 1.2        # Softer, more female-like pulse
tone.cascadeBwScale = 0.5       # Sharper formants
tone.f4FreqScale = 1.08         # Slightly shorter pharynx (female)

# Apply to running synthesizer
dll.speechPlayer_setVoicingTone(handle, byref(tone))
```

---

## Python DSP wrapper (`speechPlayer.py`)

`speechPlayer.py` is a ctypes wrapper around the DSP DLL. It provides clean Python classes (`Frame`, `FrameEx`, `VoicingTone`) and a `SpeechPlayer` class that handles:

- **Architecture detection** — automatically loads from `x86/` or `x64/` subfolder based on Python process bitness
- **Duration conversion** — `queueFrame()` accepts milliseconds, converts to samples internally
- **Runtime version checking** — probes for optional DLL exports (`setVoicingTone`, `queueFrameEx`, `setOutputGain`, `setTimeStretch`) and falls back gracefully on older DLLs
- **ABI safety** — validates `Frame` struct size on init, uses `frameExSize` parameter for forward compatibility

Key methods for version checking:

```python
player = SpeechPlayer(sampleRate=22050)

# Check capabilities before using extended APIs
player.hasVoicingToneSupport()  # True if DLL exports voicing tone functions
player.hasFrameExSupport()       # True if DLL exports queueFrameEx

# These return False if the API isn't available (older DLL)
player.setVoicingTone(tone)      # -> bool
player.setOutputGain(1.6)        # -> bool
player.setTimeStretch(2.0)       # -> bool
```

This file lives at `speechPlayer.py` (repo root) and is the **single source of truth** for all consumers — standalone Python users and the NVDA add-on alike. The NVDA add-on packaging step (documented in `claude-release.md`) manually copies this file into `C:\git\nvda-release-tg\synthDrivers\tgSpeechBox\speechPlayer.py` at release time. There is no second in-repo copy under `nvdaAddon/` — that was eliminated in April 2026 after issue #93 exposed a silent drift where slider values never reached the DLL.

Note: `speechPlayer.py` does `from logHandler import log` at the top, which is NVDA-specific. Standalone users outside NVDA should provide a compatible `logHandler` shim on `sys.path` or adapt the import in their own fork.

---

## Python testing tools (`tools/`)

These tools provide a complete Python reimplementation of the frontend's pack loading and frame emission logic. They are designed for **phoneme-by-phoneme acoustic analysis**, not for full synthesis pipelines — they don't support pitch modes, dictionary loading, or text-to-IPA conversion.

### `simple_yaml.py` — Lenient YAML parser

Handles unquoted IPA symbols (`ʃ`, `ɪ`, `@`) as keys, which standard YAML parsers reject. Used by all other Python tools.

```python
from simple_yaml import load_yaml_file
data = load_yaml_file("packs/phonemes.yaml")
```

### `lang_pack.py` — Complete pack parser

Auto-generated from `pack.h` + `pack.cpp` via `generate_lang_pack.py`. Provides a `PackSet` dataclass matching the C++ `PackSet` struct with all ~291 settings.

```python
from lang_pack import load_pack_set
pack = load_pack_set("packs", "en-us")
print(pack.lang.coarticulation_strength)
```

### `formant_trajectory.py` — Formant visualization + synthesis

Simulates TGSpeechBox's frame manager (interpolation, fading, pitch ramping) and renders formant trajectories as plots. Can optionally output WAV audio.

```
python formant_trajectory.py --packs packs --lang en-us --text "hello" --out trajectory.png
python formant_trajectory.py --packs packs --lang hu --ipa "həˈləʊ" --out traj.png --wav out.wav
```

### `frame_inspector.py` — Frame-level analysis

Inspect sample-by-sample frame interpolation, compare phoneme transitions, dump parameters at specific time points, measure formant transition rates (Hz/ms). Useful for debugging coarticulation.

```
python frame_inspector.py --packs packs --lang en-us dump a
python frame_inspector.py --packs packs --lang hu compare a i --fade 15
python frame_inspector.py --packs packs --lang en-us settings
```

These tools enable complete acoustic analysis of any phoneme or transition using only Python and the YAML packs — no compiled DLLs or platform dependencies needed.

---

## Legacy Python files (kept for reference)

`ipa.py` and `data.py` still live alongside the repo but are no longer the runtime path for NVDA. They are retained for historical reference and comparing behavior during development.

If you want to tune phonemes or language rules, update YAML packs instead.
