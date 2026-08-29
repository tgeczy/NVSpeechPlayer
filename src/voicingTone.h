/*
TGSpeechBox — VoicingTone parameter struct.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSPEECHBOX_VOICINGTONE_H
#define TGSPEECHBOX_VOICINGTONE_H

#include <stdint.h>

/*
 * DSP versioning
 *
 * Increments when the synthesizer DSP changes in a way that callers may want
 * to detect (even if the core ABI stays stable).
 */
#define SPEECHPLAYER_DSP_VERSION 8u

/*
 * VoicingTone struct versioning
 *
 * We keep a small header at the top of the struct so newer/older callers can
 * negotiate how much data is safe to read/write.
 *
 * If a caller passes an older VoicingTone layout (the original 7 doubles),
 * the magic won't match and the DLL will treat it as the legacy layout.
 */
#define SPEECHPLAYER_VOICINGTONE_MAGIC 0x32544F56u /* "VOT2" */
#define SPEECHPLAYER_VOICINGTONE_VERSION 5u  /* V5: chorus depth/detune added */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Voicing tone parameters for DSP-level voice quality adjustments.
 * 
 * These parameters allow tuning the "brightness" and "crispness" of the
 * synthesized voice without modifying per-frame data or breaking ABI.
 * 
 * All fields have sensible defaults matching the original hardcoded behavior,
 * so existing drivers that never call the setter will sound identical.
 */
typedef struct {
    /**
     * ABI header
     *
     * Callers should set:
     *   - magic = SPEECHPLAYER_VOICINGTONE_MAGIC
     *   - structSize = sizeof(speechPlayer_voicingTone_t)
     *
     * If the DLL sees an unexpected magic, it will treat the pointer as the
     * legacy 7-double layout.
     */
    uint32_t magic;

    /**
     * Size of this struct in bytes.
     * Used to safely extend the struct in the future without crashes.
     */
    uint32_t structSize;

    /**
     * Struct layout version.
     * This is about the C layout (fields/order), not the DSP behavior.
     */
    uint32_t structVersion;

    /**
     * DSP version implemented by the DLL (see SPEECHPLAYER_DSP_VERSION).
     * Callers may ignore this field and instead call speechPlayer_getDspVersion().
     */
    uint32_t dspVersion;

    /**
     * Glottal pulse peak position (0.0 to 1.0, typically 0.85-0.95).
     * Higher values => faster closing portion => more high-frequency harmonic energy ("crisper").
     * Lower values => smoother, more muffled voice.
     * Default: 0.91
     */
    double voicingPeakPos;

    /**
     * Voiced-only pre-emphasis filter coefficient (0.0 to ~0.97).
     * Higher values => more high-frequency boost on voiced sounds.
     * Default: 0.92
     */
    double voicedPreEmphA;

    /**
     * Mix amount for voiced pre-emphasis (0.0 to 1.0).
     * 0.0 = no pre-emphasis, 1.0 = full pre-emphasis.
     * Default: 0.35
     */
    double voicedPreEmphMix;

    /**
     * High-shelf EQ gain in dB (can be negative for cut, positive for boost).
     * Applied to the final output for overall brightness adjustment.
     * Default: 4.0 dB
     */
    double highShelfGainDb;

    /**
     * High-shelf EQ corner frequency in Hz.
     * Frequencies above this are boosted/cut by highShelfGainDb.
     * Default: 2000.0 Hz
     */
    double highShelfFcHz;

    /**
     * High-shelf EQ Q factor (resonance/bandwidth).
     * Higher Q => sharper transition. Typical range: 0.5 to 2.0
     * Default: 0.7
     */
    double highShelfQ;

    /**
     * Spectral tilt applied to voiced signal, in dB per octave.
     * 
     * Implemented as a tilt pole plus a lip-radiation mix ramp (see
     * voiceGenerator.h updateTiltTargets): negative values ramp in more
     * of the +6 dB/oct radiation derivative (brighter, more presence,
     * still warm); positive values fade it out and steepen the roll-off
     * toward pure glottal flow (darker, smoother).
     * 
     * SIGN CONVENTION: NEGATIVE = BRIGHTER, POSITIVE = DARKER.
     * This is the OPPOSITE of aspirationTiltDbPerOct and of the
     * per-phoneme frameEx.fricationTiltDb (both: negative = darker).
     * Shipped tuning depends on this direction (voice profiles use
     * -6/-10 "for brightness"), so the sign must not be flipped without
     * migrating every stored profile and preset.
     *
     * Typical values:
     *   - -4 to -8:   brighter, crisper, more presence
     *   - 0:          neutral (no tilt)
     *   - +4 to +8:   darker, smoother, mellower
     *   - +9 to +12:  very smooth/muffled (child/soft)
     * 
     * Negative values = brighter (radiation boost; how profiles brighten)
     * Positive values = darker/smoother (fades radiation, steeper roll-off)
     * 
     * Default: 0.0 (no additional tilt, preserves original DSP behavior)
     */
    double voicedTiltDbPerOct;

    /**
     * Optional glottal-cycle amplitude modulation depth for *noise* sources
     * (aspiration + frication), matching the classic Klatt 50% AM idea.
     *
     * 0.0 = off (noise is steady)
     * 1.0 = full Klatt-style modulation (second half-cycle attenuated)
     *
     * Default: 0.0 (off, preserves original behavior)
     */
    double noiseGlottalModDepth;

    /* ================================================================
     * V3 additions: Pitch-synchronous F1 modulation (Eloquence-like)
     * ================================================================
     * 
     * These parameters enable pitch-synchronous modulation of the first
     * formant (F1) and its bandwidth (B1), which is a key characteristic
     * of the ETI-Eloquence "buzzy clarity" sound.
     * 
     * During each glottal cycle:
     *   - OPEN phase: F1 is raised by pitchSyncF1DeltaHz, B1 widened by pitchSyncB1DeltaHz
     *   - CLOSED phase: F1/B1 return to base values
     * 
     * This models the acoustic coupling between the glottal source and
     * the vocal tract that occurs during the open phase of voicing.
     * 
     * Reference: Klatt 1980, Qlatt pitch-sync-mod crate
     */

    /**
     * F1 frequency delta during glottal open phase (Hz).
     * Positive values raise F1 during open phase (typical: 0-100 Hz).
     * 
     * 0.0 = off (no pitch-sync modulation)
     * 50.0 = moderate Eloquence-like effect
     * 100.0 = strong effect
     * 
     * Default: 0.0 (off, preserves original behavior)
     */
    double pitchSyncF1DeltaHz;

    /**
     * B1 bandwidth delta during glottal open phase (Hz).
     * Positive values widen B1 during open phase (typical: 0-80 Hz).
     * Wider B1 during open phase simulates increased glottal losses.
     * 
     * 0.0 = off
     * 40.0 = moderate effect
     * 80.0 = strong effect
     * 
     * Default: 0.0 (off, preserves original behavior)
     */
    double pitchSyncB1DeltaHz;

    /* ================================================================
     * V3 addition: Speed Quotient (glottal pulse asymmetry)
     * ================================================================
     * 
     * Controls the ratio of glottal opening time to closing time.
     * This is a key parameter for distinguishing male vs female voice quality.
     * 
     * In real speech:
     *   - Female voices: slower opening, slower/smoother closing (SQ ~1.5-2.5)
     *   - Male voices: faster opening, sharper closing (SQ ~2.5-4.0)
     *   - Pressed/tense: very fast opening, very sharp closing (SQ ~4.0+)
     *   - Breathy/lax: slow symmetric (SQ ~1.0-1.5)
     * 
     * The speed quotient affects the harmonic spectrum:
     *   - Lower SQ (more symmetric): fewer high harmonics, softer/breathier
     *   - Higher SQ (more asymmetric): richer harmonics, buzzier/brighter
     */

    /**
     * Speed quotient: ratio controlling glottal pulse asymmetry.
     * 
     * Affects peak position (primary, per LF model), opening curve
     * steepness, and closing sharpness.  This is the main "voice gender"
     * control: lower values produce softer, breathier voice quality;
     * higher values produce buzzier, more pressed voice quality.
     * 
     * Range: 0.5 to 4.0 (values outside this are clamped)
     *   - 0.5-1.0: Very soft/breathy (slow open, slow close)
     *   - 1.0-1.5: Female-like (moderate asymmetry)
     *   - 2.0: Neutral/default (matches original behavior)
     *   - 2.5-3.5: Male-like (faster open, sharper close)
     *   - 3.5-4.0: Pressed/tense voice
     * 
     * Default: 2.0 (preserves original DSP behavior)
     */
    double speedQuotient;

    /**
     * Spectral tilt applied to aspiration noise, in dB per octave.
     * 
     * Controls the brightness/darkness of breath noise independently
     * from the voiced signal tilt. Useful for shaping breathy voice quality.
     * 
     * Typical values:
     *   - -6 to -3 dB/oct: Darker, softer breath (more natural/relaxed)
     *   - 0 dB/oct: No tilt (default, white-ish noise)
     *   - +3 to +6 dB/oct: Brighter, harsher breath (more "airy")
     * 
     * Default: 0.0 (no tilt, preserves original behavior)
     */
    double aspirationTiltDbPerOct;

    /**
     * Global cascade formant bandwidth multiplier.
     * 
     * Scales all cascade resonator bandwidths (B1-B6), changing the
     * fundamental resonance character of the vocal tract model.
     * This is an "instrument" knob rather than a "voice" knob -- it
     * changes how sharply defined each formant peak is.
     * 
     * Narrower bandwidths (< 1.0) produce sharper, more defined formant
     * peaks where each vowel rings more distinctly (Eloquence-like clarity).
     * Wider bandwidths (> 1.0) blur formant peaks together, producing a
     * softer, more muffled quality (some DECTalk-like warmth).
     * 
     * The effect is subtle but pervasive -- it changes the entire
     * character of voiced sounds without affecting fricatives (which
     * use the parallel path).
     * 
     * Range: 0.3 to 2.0 (clamped by DSP)
     *   - 0.3: Very sharp/ringy formants, crystalline, may ring on transitions
     *   - 0.7: Noticeably sharper, clear vowel definition
     *   - 0.9: default — mild narrowing for clearer vowels
     *   - 1.0: neutral, slight muffle
     *   - 1.2-1.4: Softer, warmer, formants blend more
     *
     * NOTE: the cascade additionally tightens F1 (x0.75) and F2 (x0.88)
     * relative to this scale (formantGenerator.h), so the effective
     * F1/F2 scaling at the default is ~0.675/0.792 — narrower than this
     * value alone suggests.
     *
     * Default: 0.9 (mild narrowing, matches TGSB_VOICING_TONE_DEFAULTS)
     */
    double cascadeBwScale;

    /**
     * Tremor depth - slow amplitude modulation for elderly/shaky voice.
     * 
     * A ~5.5 Hz sine LFO modulates voiced amplitude to create tremulous
     * quality typical of elderly, nervous, or emotional voices.
     * 
     * Range: 0.0 to 0.5 (clamped by DSP)
     *   - 0.0: No tremor (default)
     *   - 0.1-0.2: Subtle shakiness
     *   - 0.3-0.4: Noticeable elderly tremor
     * 
     * Stacks with jitter/shimmer for realistic aging effects.
     * Rate is fixed at 5.5 Hz internally.
     */
    double tremorDepth;

    /* ================================================================
     * V4 additions: Vocal tract shape parameters for voice character
     * ================================================================
     *
     * These model structural vocal tract differences between male,
     * female, and child voices — NOT source differences (breathiness).
     * Real female/child voices differ primarily in tract geometry
     * (shorter pharynx, wider nasal passages), not in aspiration.
     */

    /**
     * Nasal resonator bandwidth multiplier.
     * Scales cbN0 (antiresonator) and cbNP (nasal pole) bandwidths.
     *
     * Female nasal passages are wider relative to tract size, creating
     * more damped nasal resonances. This changes the overall voice
     * color without affecting breathiness.
     *
     * Range: 0.5 to 2.0 (clamped by DSP)
     *   - 0.8-0.9: Sharper nasal murmur (prominent nasality)
     *   - 1.0: Default (no scaling)
     *   - 1.2-1.4: Wider, more damped nasal resonances (female character)
     *
     * Default: 1.0
     */
    double nasalBwScale;

    /**
     * F4 frequency multiplier.
     * Scales cf4 and pf4 (cascade and parallel fourth formant).
     *
     * F4 is primarily determined by pharynx length. Shorter pharynx
     * (female/child) raises F4. This is one of the strongest acoustic
     * cues for perceived vocal tract size independent of pitch.
     *
     * Range: 0.7 to 1.5 (clamped by DSP)
     *   - 0.9-0.95: Larger pharynx (deep male)
     *   - 1.0: Default (no scaling, base F4 ~3300 Hz)
     *   - 1.05-1.10: Female pharynx (F4 ~3465-3630 Hz)
     *   - 1.15-1.25: Child pharynx (F4 ~3795-4125 Hz)
     *
     * Default: 1.0
     */
    double f4FreqScale;

    /**
     * Nasal pole coupling amplitude multiplier.
     * Scales caNP, controlling how much nasal resonance bleeds through.
     *
     * Range: 0.5 to 1.5 (clamped by DSP)
     *   - 0.6-0.8: Reduced nasality (denasalized quality)
     *   - 1.0: Default (no scaling)
     *   - 1.1-1.3: Increased nasality (hypernasal quality)
     *
     * Default: 1.0
     */
    double nasalGainScale;

    /* ================================================================
     * V5 additions: Dual-oscillator chorus (vocal fold asymmetry)
     * ================================================================
     *
     * Real vocal folds are not perfectly symmetric — each fold has
     * slightly different mass and tension, producing cycle-to-cycle
     * variation in the glottal pulse.  This dual-oscillator model
     * runs a second phase accumulator at a slightly detuned pitch
     * and blends it with the primary, creating natural richness
     * that is deterministic (unlike jitter, which is random).
     *
     * The detune offset is tiny (1-3 Hz), so the result is NOT
     * two separate voices — it's a subtle thickening of a single
     * voice, similar to what natural vocal fold asymmetry produces.
     */

    /**
     * Chorus depth: blend amount of the second oscillator.
     *
     * Range: 0.0 to 1.0 (clamped by DSP)
     *   - 0.0: Off — single oscillator (default, backward compatible)
     *   - 0.3: Subtle warmth / thickening
     *   - 0.5: Moderate chorus (natural richness)
     *   - 1.0: Full 50/50 blend (maximum vocal fold asymmetry effect)
     *
     * Default: 0.0 (off)
     */
    double chorusDepth;

    /**
     * Chorus detune: pitch offset of the second oscillator in Hz.
     *
     * Controls the beating rate between the two oscillators.
     * Lower values = slower beating (warm), higher = faster (more alive).
     *
     * Range: 0.5 to 5.0 Hz (clamped by DSP)
     *   - 1.0: Very slow beating (~1 Hz), gentle warmth
     *   - 2.0: Natural vocal fold asymmetry range (default)
     *   - 3.0-5.0: Faster beating, more noticeable chorus
     *
     * Default: 2.0 Hz
     */
    double chorusDetuneHz;

} speechPlayer_voicingTone_t;

/**
 * Default values matching the original hardcoded constants.
 * Use this to initialize a voicingTone struct before modifying specific fields.
 */
#define SPEECHPLAYER_VOICINGTONE_DEFAULTS { \
    SPEECHPLAYER_VOICINGTONE_MAGIC,              /* magic */ \
    (uint32_t)sizeof(speechPlayer_voicingTone_t),/* structSize */ \
    SPEECHPLAYER_VOICINGTONE_VERSION,            /* structVersion */ \
    SPEECHPLAYER_DSP_VERSION,                    /* dspVersion */ \
    0.91,   /* voicingPeakPos */ \
    0.92,   /* voicedPreEmphA */ \
    0.35,   /* voicedPreEmphMix */ \
    4.0,    /* highShelfGainDb — matches NVDA; 5.5 was too bright on iOS/Android */ \
    2000.0, /* highShelfFcHz */ \
    0.7,    /* highShelfQ */ \
    0.0,    /* voicedTiltDbPerOct (no tilt by default) */ \
    0.0,    /* noiseGlottalModDepth */ \
    0.0,    /* pitchSyncF1DeltaHz (off by default) */ \
    0.0,    /* pitchSyncB1DeltaHz (off by default) */ \
    2.0,    /* speedQuotient (neutral, matches original behavior) */ \
    0.0,    /* aspirationTiltDbPerOct (no tilt by default) */ \
    0.9,    /* cascadeBwScale (mild narrowing for clearer vowels, less tubey than 0.8) */ \
    0.0,    /* tremorDepth (no tremor by default) */ \
    1.0,    /* nasalBwScale (no scaling) */ \
    1.0,    /* f4FreqScale (no scaling, base F4 ~3300 Hz) */ \
    1.0,    /* nasalGainScale (no scaling) */ \
    0.0,    /* chorusDepth (off by default) */ \
    2.0     /* chorusDetuneHz (natural vocal fold asymmetry range) */ \
}

/**
 * Helper function to get default voicing tone parameters.
 * Useful for C code or when the macro isn't convenient.
 */
static inline speechPlayer_voicingTone_t speechPlayer_getDefaultVoicingTone(void) {
    speechPlayer_voicingTone_t tone = SPEECHPLAYER_VOICINGTONE_DEFAULTS;
    return tone;
}

#ifdef __cplusplus
}
#endif

#endif /* TGSPEECHBOX_VOICINGTONE_H */
