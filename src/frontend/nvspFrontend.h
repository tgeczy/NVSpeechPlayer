/*
TGSpeechBox — Frontend public C API and FrameEx struct.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSB_FRONTEND_H
#define TGSB_FRONTEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef NVSP_FRONTEND_EXPORTS
    #define NVSP_FRONTEND_API __declspec(dllexport)
  #else
    #define NVSP_FRONTEND_API __declspec(dllimport)
  #endif
#else
  #define NVSP_FRONTEND_API
#endif

#define NVSP_FRONTEND_ABI_VERSION 5

typedef void* nvspFrontend_handle_t;

/*
  Frame struct. Field order MUST stay in sync with speechPlayer.dll.
  This is intentionally a plain-old-data struct for ABI stability.
  
  This struct contains the core 47 parameters that have been stable since v1.
*/
typedef struct nvspFrontend_Frame {
  double voicePitch;
  double vibratoPitchOffset;
  double vibratoSpeed;
  double voiceTurbulenceAmplitude;
  double glottalOpenQuotient;
  double voiceAmplitude;
  double aspirationAmplitude;
  double cf1, cf2, cf3, cf4, cf5, cf6, cfN0, cfNP;
  double cb1, cb2, cb3, cb4, cb5, cb6, cbN0, cbNP;
  double caNP;
  double fricationAmplitude;
  double pf1, pf2, pf3, pf4, pf5, pf6;
  double pb1, pb2, pb3, pb4, pb5, pb6;
  double pa1, pa2, pa3, pa4, pa5, pa6;
  double parallelBypass;
  double preFormantGain;
  double outputGain;
  double endVoicePitch;
} nvspFrontend_Frame;

/*
  Extended frame parameters (ABI v2+).
  
  These voice quality parameters are kept separate from nvspFrontend_Frame
  to maintain backward compatibility. They match speechPlayer_frameEx_t in
  the DSP DLL.
  
  All fields are in range [0.0, 1.0] except sharpness which is a multiplier
  (typically 0.5-2.0, where 1.0 is neutral), and endCf/endPf which are Hz
  (or NAN for no ramping).
*/
// >>> AUTO-GENERATED FROM src/frame.h - DO NOT EDIT MANUALLY (regenerate: python tools/gen_frame_ex.py) >>>
typedef struct {
  // Voice quality parameters (DSP v5)
  double creakiness;  // laryngealization / creaky voice (e.g. Danish stød)
  double breathiness;  // breath noise mixed into voicing
  double jitter;  // pitch period variation (irregular F0)
  double shimmer;  // amplitude variation (irregular loudness)
  double sharpness;  // glottal closure sharpness MULTIPLIER (0=use SR default, 0.5-2.0 typical)
  // Formant end targets for within-frame ramping (NAN = no ramp)
  double endCf1;  // Cascade F1 end target (Hz), NAN = no ramp
  double endCf2;  // Cascade F2 end target (Hz), NAN = no ramp
  double endCf3;  // Cascade F3 end target (Hz), NAN = no ramp
  double endPf1;  // Parallel F1 end target (Hz), NAN = no ramp
  double endPf2;  // Parallel F2 end target (Hz), NAN = no ramp
  double endPf3;  // Parallel F3 end target (Hz), NAN = no ramp
  // Fujisaki-Bartman pitch contour model (DSP v6+)
  double fujisakiEnabled;  // 0.0 = off (legacy behavior), >0.5 = on
  double fujisakiReset;  // rising edge resets model filter state
  double fujisakiPhraseAmp;  // phrase command amplitude (e.g. 1.3)
  double fujisakiPhraseLen;  // phrase filter L (samples to peak). 0 = use default
  double fujisakiAccentAmp;  // accent command amplitude (e.g. 0.4)
  double fujisakiAccentDur;  // accent duration D (samples). 0 = use default
  double fujisakiAccentLen;  // accent filter L (samples to peak). 0 = use default
  // Per-parameter transition speed scales (DSP v7). 0.0 = no override.
  double transF1Scale;  // cf1, pf1, cb1, pb1
  double transF2Scale;  // cf2, pf2, cb2, pb2
  double transF3Scale;  // cf3, pf3, cb3, pb3
  double transNasalScale;  // cfN0, cfNP, cbN0, cbNP, caNP
  // Amplitude crossfade mode (DSP v7.1). 0=linear, 1=equal-power.
  double transAmplitudeMode;
  // Higher cascade formants F7/F8 (DSP v8, Rabiner 1968 defaults)
  double cf7;  // F7 frequency (Hz).  Default 6500.0
  double cb7;  // F7 bandwidth (Hz).  Default 720.0
  double cf8;  // F8 frequency (Hz).  Default 7500.0
  double cb8;  // F8 bandwidth (Hz).  Default 1250.0
  // Source amplitude timing (DSP v8). 0.0 = legacy, no hold.
  double transSourceHoldRatio;
  // Voicing onset hold (DSP v8). 0.0 = legacy, no hold.
  double transVoicingHoldRatio;
  // Frication spectral tilt (DSP v9). 0=flat, negative=darken high-freq parallels.
  double fricationTiltDb;
} nvspFrontend_FrameEx;
// <<< END AUTO-GENERATED <<<

/* Number of fields in FrameEx struct (for size validation) */
#define NVSP_FRONTEND_FRAMEEX_NUM_PARAMS 30

/*
  VoicingTone parameters for DSP-level voice quality (ABI v2+).
  
  These control the glottal pulse shape, spectral tilt, and EQ at the DSP level.
  They are read from the voicingTone: block in voice profiles.
  
  All fields have defaults that result in neutral/bypass behavior.
*/
typedef struct nvspFrontend_VoicingTone {
  // >>> AUTO-GENERATED FROM src/voicingTone.h - DO NOT EDIT MANUALLY (regenerate: python tools/gen_voicing_tone.py) >>>
  // V1 parameters
  double voicingPeakPos;
  double voicedPreEmphA;
  double voicedPreEmphMix;
  double highShelfGainDb;
  double highShelfFcHz;
  double highShelfQ;
  double voicedTiltDbPerOct;
  // V2 parameters
  double noiseGlottalModDepth;
  double pitchSyncF1DeltaHz;
  double pitchSyncB1DeltaHz;
  // V3 parameters
  double speedQuotient;
  double aspirationTiltDbPerOct;
  double cascadeBwScale;
  double tremorDepth;
  // V4 parameters - vocal tract shape
  double nasalBwScale;
  double f4FreqScale;
  double nasalGainScale;
  // V5 parameters - dual-oscillator chorus (vocal fold asymmetry)
  double chorusDepth;
  double chorusDetuneHz;
  // <<< END AUTO-GENERATED <<<
} nvspFrontend_VoicingTone;

/* Number of fields in VoicingTone struct */
#define NVSP_FRONTEND_VOICINGTONE_NUM_PARAMS 19

/*
  VoiceProfileSliders - the 12 user-adjustable slider values (ABI v2+).
  
  These are the values exposed to users via NVDA sliders.
  The "hidden" VoicingTone params (voicingPeakPos, voicedPreEmphA, etc.)
  are NOT included here - they are preserved if manually edited in YAML.
  
  Used by nvspFrontend_saveVoiceProfileSliders() to write user settings
  back to phonemes.yaml.
*/
typedef struct nvspFrontend_VoiceProfileSliders {
  /* VoicingTone sliders (8) */
  double voicedTiltDbPerOct;      /* Spectral tilt in dB/octave */
  double noiseGlottalModDepth;    /* Noise modulation by glottal cycle (0.0-1.0) */
  double pitchSyncF1DeltaHz;      /* Pitch-synchronous F1 delta */
  double pitchSyncB1DeltaHz;      /* Pitch-synchronous B1 delta */
  double speedQuotient;           /* Glottal speed quotient (0.5-4.0, 2.0 = neutral) */
  double aspirationTiltDbPerOct;  /* Aspiration spectral tilt */
  double cascadeBwScale;          /* Global cascade bandwidth multiplier (0.4-1.4, 1.0 = neutral) */
  double tremorDepth;             /* Tremor depth for elderly/shaky voice (0-0.5) */
  
  /* FrameEx sliders (5) */
  double creakiness;              /* Laryngealization (0.0-1.0) */
  double breathiness;             /* Breathiness (0.0-1.0) */
  double jitter;                  /* Pitch variation (0.0-1.0) */
  double shimmer;                 /* Amplitude variation (0.0-1.0) */
  double sharpness;               /* Glottal sharpness multiplier (0.5-2.0, 1.0 = neutral) */
} nvspFrontend_VoiceProfileSliders;

/* Number of fields in VoiceProfileSliders struct */
#define NVSP_FRONTEND_VOICEPROFILESLIDERS_NUM_PARAMS 13

/*
  Callback invoked for each frame (legacy, ABI v1).
  - frameOrNull: NULL means "silence" for the given duration.
  - durationMs and fadeMs are in milliseconds (same units as the Python side today).
  - userIndex is passed through, so callers can map audio back to text positions.
*/
typedef void (*nvspFrontend_FrameCallback)(
  void* userData,
  const nvspFrontend_Frame* frameOrNull,
  double durationMs,
  double fadeMs,
  int userIndex
);

/*
  Extended callback invoked for each frame (ABI v2+).
  - frameOrNull: NULL means "silence" for the given duration.
  - frameExOrNull: Extended parameters, or NULL if not applicable.
  - durationMs and fadeMs are in milliseconds.
  - userIndex is passed through for text position mapping.
*/
typedef void (*nvspFrontend_FrameExCallback)(
  void* userData,
  const nvspFrontend_Frame* frameOrNull,
  const nvspFrontend_FrameEx* frameExOrNull,
  double durationMs,
  double fadeMs,
  int userIndex
);

/* Create/destroy. packDir should contain:
   - packs/phonemes.yaml
   - packs/lang/default.yaml
   - packs/lang/<lang>.yaml (optional)

   You can point packDir to the directory that contains the "packs" folder.
*/
NVSP_FRONTEND_API nvspFrontend_handle_t nvspFrontend_create(const char* packDirUtf8);
NVSP_FRONTEND_API void nvspFrontend_destroy(nvspFrontend_handle_t handle);

/*
  Set an optional override directory.  When set, loadPackSet checks
  <overrideDir>/packs/lang/<file>.yaml before the bundle packDir.
  This lets the iOS app group container hold user-imported packs that
  shadow the built-in ones.  Pass NULL or "" to clear.
*/
NVSP_FRONTEND_API void nvspFrontend_setOverrideDirectory(nvspFrontend_handle_t handle,
                                                          const char* overrideDirUtf8);

/*
  Set the language (BCP-47-ish: en, en-us, hu, pl, bg, ...).
  Loads and merges:
    default.yaml, <base>.yaml, <base-region>.yaml, ... up to the most specific tag.

  Returns 1 on success, 0 on failure.
*/
NVSP_FRONTEND_API int nvspFrontend_setLanguage(nvspFrontend_handle_t handle, const char* langTagUtf8);

/*
  Convert IPA text into frames.

  Inputs:
  - ipaUtf8: IPA string (ideally already normalized to your phoneme table)
  - speed: same meaning as ipa_convert.py today (used as divisor in timing)
  - basePitch: base pitch in Hz-ish terms used by speechPlayer
  - inflection: 0..1-ish pitch range scaling (same meaning as ipa_convert.py)
  - clauseTypeUtf8: one of "." "," "?" "!" (NULL treated as ".")
  - userIndexBase: the index passed to callback; you can call multiple times with different bases

  Output:
  - cb is called once per frame.

  Returns 1 on success, 0 on failure.
*/
NVSP_FRONTEND_API int nvspFrontend_queueIPA(
  nvspFrontend_handle_t handle,
  const char* ipaUtf8,
  double speed,
  double basePitch,
  double inflection,
  const char* clauseTypeUtf8,
  int userIndexBase,
  nvspFrontend_FrameCallback cb,
  void* userData
);

/*
  Set the voice profile to use for parameter transformation.
  
  Voice profiles allow producing different voice qualities (e.g., female voice)
  by applying formant scaling and other modifications to phoneme parameters,
  without maintaining separate phoneme tables.
  
  - profileNameUtf8: Name of the profile to use (e.g., "female").
    Pass NULL or "" to disable voice profiles (use base phoneme parameters).
  - The profile must be defined in the pack's phonemes.yaml under "voiceProfiles:".
  - Unknown profile names are silently ignored (no effect).
  
  The voice profile setting persists until changed or the handle is destroyed.
  It can also be set per-language in the lang.yaml file via "voiceProfileName:".
  
  Returns 1 on success, 0 on failure (e.g., invalid handle).
*/
NVSP_FRONTEND_API int nvspFrontend_setVoiceProfile(nvspFrontend_handle_t handle, const char* profileNameUtf8);

/*
  Get the currently active voice profile name.
  Returns a pointer to an empty string if no profile is active.
  The returned pointer is owned by the handle and valid until the next API call.
*/
NVSP_FRONTEND_API const char* nvspFrontend_getVoiceProfile(nvspFrontend_handle_t handle);

/*
  Get non-fatal warnings from pack loading (e.g., voice profile parse errors).
  Returns an empty string if no warnings.
  Useful for debugging "why does my voice profile do nothing?" issues.
  The returned pointer is owned by the handle and valid until the next API call.
*/
NVSP_FRONTEND_API const char* nvspFrontend_getPackWarnings(nvspFrontend_handle_t handle);

/*
  If a function returns failure, call this to get a human-readable message.
  The returned pointer is owned by the frontend handle and remains valid until the next call.
*/
NVSP_FRONTEND_API const char* nvspFrontend_getLastError(nvspFrontend_handle_t handle);

/*
  Get the ABI version of the loaded DLL.
  Callers can use this to check for feature availability.
*/
NVSP_FRONTEND_API int nvspFrontend_getABIVersion(void);

/* ============================================================================
 * Extended Frame API (ABI v2+)
 * ============================================================================
 *
 * These functions provide per-frame voice quality control via FrameEx parameters.
 * The frontend mixes user-level defaults with per-phoneme values from YAML.
 */

/*
  Set user-level FrameEx defaults.
  
  These values are added to per-phoneme FrameEx values (from YAML) when emitting
  frames. The result is clamped to valid ranges.
  
  This is typically called when the user adjusts voice quality sliders.
  The defaults persist until changed or the handle is destroyed.
  
  Parameters:
  - creakiness:  0.0-1.0, added to phoneme creakiness
  - breathiness: 0.0-1.0, added to phoneme breathiness  
  - jitter:      0.0-1.0, added to phoneme jitter
  - shimmer:     0.0-1.0, added to phoneme shimmer
  - sharpness:   multiplier (0.5-2.0 typical), multiplied with phoneme sharpness
*/
NVSP_FRONTEND_API void nvspFrontend_setFrameExDefaults(
  nvspFrontend_handle_t handle,
  double creakiness,
  double breathiness,
  double jitter,
  double shimmer,
  double sharpness
);

/*
  Get the current FrameEx defaults.
  
  Writes the current user-level defaults to the provided struct.
  Returns 1 on success, 0 on failure (e.g., invalid handle or NULL pointer).
*/
NVSP_FRONTEND_API int nvspFrontend_getFrameExDefaults(
  nvspFrontend_handle_t handle,
  nvspFrontend_FrameEx* outDefaults
);

/*
  Convert IPA text into frames with extended parameters (ABI v2+).
  
  This is the extended version of nvspFrontend_queueIPA that uses the
  FrameExCallback to emit both Frame and FrameEx data.
  
  The FrameEx values in the callback are the result of mixing:
  - Per-phoneme values from YAML (if defined)
  - User-level defaults set via nvspFrontend_setFrameExDefaults()
  
  For silence frames, frameExOrNull will be NULL.
  
  Returns 1 on success, 0 on failure.
*/
NVSP_FRONTEND_API int nvspFrontend_queueIPA_Ex(
  nvspFrontend_handle_t handle,
  const char* ipaUtf8,
  double speed,
  double basePitch,
  double inflection,
  const char* clauseTypeUtf8,
  int userIndexBase,
  nvspFrontend_FrameExCallback cb,
  void* userData
);

/*
  Convert IPA text into frames with text-level corrections (ABI v4+).

  Same as nvspFrontend_queueIPA_Ex, but also accepts the original text.
  If textUtf8 is provided (non-NULL, non-empty), the frontend runs a text
  parser before the IPA engine.  Currently this corrects stress placement
  using a dictionary lookup (e.g., CMU Dict for en-us).

  If textUtf8 is NULL or "", the function behaves identically to
  nvspFrontend_queueIPA_Ex (no text parsing).

  Returns 1 on success, 0 on failure.
*/
NVSP_FRONTEND_API int nvspFrontend_queueIPA_ExWithText(
  nvspFrontend_handle_t handle,
  const char* textUtf8,
  const char* ipaUtf8,
  double speed,
  double basePitch,
  double inflection,
  const char* clauseTypeUtf8,
  int userIndexBase,
  nvspFrontend_FrameExCallback cb,
  void* userData
);

/*
  Get the voicing tone parameters for the current voice profile (ABI v2+).
  
  Writes the VoicingTone parameters for the currently active voice profile
  to the provided struct. If no profile is active or the profile doesn't
  have voicingTone settings, the struct is filled with default values.
  
  Parameters:
  - outTone: Pointer to struct to fill with voicing tone parameters.
  
  Returns:
  - 1 if the current profile has explicit voicingTone settings
  - 0 if using defaults (no profile or profile has no voicingTone block)
  
  The caller should use the returned value to decide whether to apply
  the voicing tone or fall back to Python-side defaults.
*/
NVSP_FRONTEND_API int nvspFrontend_getVoicingTone(
  nvspFrontend_handle_t handle,
  nvspFrontend_VoicingTone* outTone
);

/*
  Get a list of voice profile names (ABI v2+).
  
  Returns a null-terminated, newline-separated string of profile names.
  The returned pointer is owned by the handle and valid until the next API call.
  
  Example return value: "Crystal\nBeth\nBobby\n"
*/
NVSP_FRONTEND_API const char* nvspFrontend_getVoiceProfileNames(nvspFrontend_handle_t handle);

/*
  Save voice profile slider values to phonemes.yaml (ABI v2+).
  
  Writes the 11 user-adjustable slider values to the voicingTone block
  for the specified profile in phonemes.yaml.
  
  If the profile doesn't exist, it will be created under voiceProfiles:.
  If the voicingTone: block doesn't exist, it will be created.
  Existing "hidden" params (voicingPeakPos, etc.) are preserved.
  
  Parameters:
  - profileNameUtf8: Name of the profile (e.g., "Adam", "Beth")
  - sliders: Pointer to struct containing the 11 slider values
  
  Returns:
  - 1 on success
  - 0 on failure (call nvspFrontend_getLastError for details)
*/
NVSP_FRONTEND_API int nvspFrontend_saveVoiceProfileSliders(
  nvspFrontend_handle_t handle,
  const char* profileNameUtf8,
  const nvspFrontend_VoiceProfileSliders* sliders
);

/*
  Override the pitch intonation model at runtime.

  modeUtf8 must be one of:
    "espeak_style"    - ToBI-based intonation regions (default)
    "legacy"          - older time-based pitch curve
    "fujisaki_style"  - flat base + DSP phrase/accent contours
    "impulse_style"   - multi-layer additive pitch
    "klatt_style"     - Klatt 1987 hat-pattern intonation

  Persists until the next setLanguage() call (which reloads from YAML).
  Returns 1 on success, 0 on failure (invalid handle or unknown mode).
*/
NVSP_FRONTEND_API int nvspFrontend_setPitchMode(
  nvspFrontend_handle_t handle,
  const char* modeUtf8
);

/*
  Override the legacy pitch inflection scale at runtime.

  Only affects the "legacy" pitch mode.
  1.0 preserves historical behavior; 0.58 is the default (maps 0.60 → 0.35).

  Persists until the next setLanguage() call (which reloads from YAML).
*/
NVSP_FRONTEND_API void nvspFrontend_setLegacyPitchInflectionScale(
  nvspFrontend_handle_t handle,
  double scale
);

/*
  Pre-eSpeak compound word splitting (ABI v4+).

  Scans textUtf8 for words in the loaded compound map and replaces them
  with their constituent halves joined by spaces.  The caller should feed
  the returned text to eSpeak instead of the original, so each half is
  phonemized independently with correct vowel quality.

  Returns NULL if no transforms were applied (use original text unchanged).
  Returns a malloc'd UTF-8 string if any transforms fired.  The caller
  must free the returned pointer with nvspFrontend_freeString().
*/
NVSP_FRONTEND_API char* nvspFrontend_prepareText(
  nvspFrontend_handle_t handle,
  const char* textUtf8
);

// Backwards-compatible alias for older callers.
#define nvspFrontend_splitCompounds nvspFrontend_prepareText

/*
  Free a string returned by nvspFrontend_prepareText().
*/
NVSP_FRONTEND_API void nvspFrontend_freeString(char* str);

/*
  Export merged YAML for a domain (base file + user overrides).
  Comments are preserved for unchanged sections via surgical comparison.

  domain: NVSP_DATA_SETTINGS or NVSP_DATA_PHONEMES.
  langTagUtf8: language tag (e.g. "en-us") for settings; ignored for phonemes.
  overridesJsonUtf8: JSON object string {"key": "value", ...} of user overrides.

  Returns a malloc'd UTF-8 YAML string. Caller frees with nvspFrontend_freeString().
  Returns NULL if no base file found or on error.
*/
NVSP_FRONTEND_API char* nvspFrontend_exportData(
    nvspFrontend_handle_t handle,
    int domain,
    const char* langTagUtf8,
    const char* overridesJsonUtf8
);

/*
  Apply setting overrides on top of the currently loaded language pack.
  yamlSnippetUtf8 is a mini YAML snippet containing "key: value" lines.
  Dot-notation keys (e.g. "boundarySmoothing.enabled: true") are supported.
  Must be called after setLanguage. Returns 1 on success, 0 on failure.
*/
NVSP_FRONTEND_API int nvspFrontend_applySettingOverrides(
  nvspFrontend_handle_t handle,
  const char* yamlSnippetUtf8
);

/*
  Get available language tags by scanning packs/lang/*.yaml.
  Returns a malloc'd "langTag\n" string. Caller must free with nvspFrontend_freeString().
*/
NVSP_FRONTEND_API char* nvspFrontend_getAvailableLanguages(nvspFrontend_handle_t handle);

/*
  Preview a single phoneme by key, bypassing the full pipeline.

  Looks up the PhonemeDef for phonemeKeyUtf8 in the loaded pack and
  directly emits one steady-state frame to the callback.  No eSpeak,
  no allophone rules, no pitch contour — just the raw acoustic frame
  from the phoneme definition, exactly as modified by setData().

  pitchHz controls the fundamental frequency.
  durationMs controls how long the phoneme is held (default ~300ms).
  Returns 1 on success (phoneme found), 0 on failure.
*/
NVSP_FRONTEND_API int nvspFrontend_previewPhoneme(
  nvspFrontend_handle_t handle,
  const char* phonemeKeyUtf8,
  double pitchHz,
  double durationMs,
  nvspFrontend_FrameExCallback cb,
  void* userData
);

/* ============================================================================
 * Generic Data Query API (ABI v5+)
 * ============================================================================
 *
 * Paginated, typed access to pack settings (and future phoneme / dictionary
 * data) without re-reading YAML from disk on every call.
 *
 * Key improvement over getPackSettings: accepts a langTag parameter so
 * callers can query any language without a disruptive setLanguage() switch.
 */

/* Data domains. New values can be added without ABI break. */
#define NVSP_DATA_SETTINGS   0
#define NVSP_DATA_PHONEMES   1
#define NVSP_DATA_DICTIONARY 2
#define NVSP_DATA_FRAMETRACE 3   /* per-utterance phoneme-boundary map, read via nvspFrontend_queryData */
#define NVSP_DATA_PASSTRACE  4   /* per-utterance per-pass token snapshots, read via nvspFrontend_queryData */

/*
  Count records in a data domain for a language, without loading them.
  Useful for scroll sizing or showing "Settings (291)" or "Phonemes (195)"
  in tab headers.

  Parameters:
  - domain: NVSP_DATA_SETTINGS, NVSP_DATA_PHONEMES, or NVSP_DATA_DICTIONARY
  - langTagUtf8: For SETTINGS: language to query (required, non-empty).
                 For PHONEMES: "" = all base phonemes, "en-gb" = only phonemes
                 referenced as replacement targets in that language.

  Returns the number of records, or -1 on error.
*/
NVSP_FRONTEND_API int nvspFrontend_getDataCount(
  nvspFrontend_handle_t handle,
  int domain,
  const char* langTagUtf8
);

/*
  Query a page of typed records from a data domain.

  Returns a malloc'd UTF-8 JSON array string. Each element is an object with
  at least "key", "type", and "value" fields. Caller must free with
  nvspFrontend_freeString(). Returns NULL on error.

  For NVSP_DATA_SETTINGS, each object has:
    { "key": "boundarySmoothing.enabled", "type": "bool",
      "value": true, "group": "boundarySmoothing" }
  Types: "float" (numeric value), "bool" (true/false), "string" (quoted).

  For NVSP_DATA_PHONEMES, each object has:
    { "key": "ɪ.cf2", "type": "float", "value": 1750,
      "group": "ɪ", "class": "vowel" }
  In the lang-filtered view, objects also include:
    "mappingFrom": "ɜː"   (the IPA that maps to this phoneme)

  Parameters:
  - domain: NVSP_DATA_SETTINGS, NVSP_DATA_PHONEMES, etc.
  - langTagUtf8: For SETTINGS: language (required).
                 For PHONEMES: "" = all, "en-gb" = lang-filtered.
  - offset: 0-based record index to start from
  - limit: max records to return (0 = all remaining from offset)

  First call builds a cache; subsequent calls slice it (no disk I/O).
  Cache is invalidated by setLanguage() or setData() for the same langTag.
*/
NVSP_FRONTEND_API char* nvspFrontend_queryData(
  nvspFrontend_handle_t handle,
  int domain,
  const char* langTagUtf8,
  int offset,
  int limit
);

/*
  Set a single value in a data domain.

  If langTagUtf8 matches the currently active language, the change is applied
  to the in-memory pack immediately (live preview). Otherwise the call
  succeeds but the override is not applied — the caller should persist it
  and re-apply after the next setLanguage().

  Parameters:
  - domain: NVSP_DATA_SETTINGS, etc.
  - langTagUtf8: target language
  - keyUtf8: record key (dot-notation setting name, phoneme IPA, etc.)
  - valueUtf8: new value as string ("1.4", "true", "foo")

  Returns 1 on success, 0 on failure (call nvspFrontend_getLastError).
*/
NVSP_FRONTEND_API int nvspFrontend_setData(
  nvspFrontend_handle_t handle,
  int domain,
  const char* langTagUtf8,
  const char* keyUtf8,
  const char* valueUtf8
);

#ifdef __cplusplus
}
#endif

#endif
