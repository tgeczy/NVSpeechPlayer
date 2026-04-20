/*
TGSpeechBox — Frame struct and queue definitions.
Copyright 2014 NV Access Limited.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSPEECHBOX_FRAME_H
#define TGSPEECHBOX_FRAME_H

#include "lock.h"
#include <limits>

typedef double speechPlayer_frameParam_t;

typedef struct {
	// voicing and cascaide
	speechPlayer_frameParam_t voicePitch; //  fundermental frequency of voice (phonation) in hz
	speechPlayer_frameParam_t vibratoPitchOffset; // pitch is offset up or down in fraction of a semitone
	speechPlayer_frameParam_t vibratoSpeed; // Speed of vibrato in hz
	speechPlayer_frameParam_t voiceTurbulenceAmplitude; // amplitude of voice breathiness from 0 to 1 
	speechPlayer_frameParam_t glottalOpenQuotient; // fraction between 0 and 1 of a voice cycle that the glottis is open (allows voice turbulance, alters f1...)
	speechPlayer_frameParam_t voiceAmplitude; // amplitude of voice (phonation) source between 0 and 1.
	speechPlayer_frameParam_t aspirationAmplitude; // amplitude of aspiration (voiceless h, whisper) source between 0 and 1.
	speechPlayer_frameParam_t cf1, cf2, cf3, cf4, cf5, cf6, cfN0, cfNP; // frequencies of standard cascaide formants, nasal (anti) 0 and nasal pole in hz
	speechPlayer_frameParam_t cb1, cb2, cb3, cb4, cb5, cb6, cbN0, cbNP; // bandwidths of standard cascaide formants, nasal (anti) 0 and nasal pole in hz
	speechPlayer_frameParam_t caNP; // amplitude from 0 to 1 of cascade nasal pole formant
	// fricatives and parallel
	speechPlayer_frameParam_t fricationAmplitude; // amplitude of frication noise from 0 to 1.
	speechPlayer_frameParam_t pf1, pf2, pf3, pf4, pf5, pf6; // parallel formants in hz
	speechPlayer_frameParam_t pb1, pb2, pb3, pb4, pb5, pb6; // parallel formant bandwidths in hz
	speechPlayer_frameParam_t pa1, pa2, pa3, pa4, pa5, pa6; // amplitude of parallel formants between 0 and 1
	speechPlayer_frameParam_t parallelBypass; // amount of signal which should bypass parallel resonators from 0 to 1
	speechPlayer_frameParam_t preFormantGain; // amplitude from 0 to 1 of all vocal tract sound (voicing, frication) before entering formant resonators. Useful for stopping/starting speech
	speechPlayer_frameParam_t outputGain; // amplitude from 0 to 1 of final output (master volume) 
	speechPlayer_frameParam_t endVoicePitch; //  pitch of voice at the end of the frame length 
} speechPlayer_frame_t;

const int speechPlayer_frame_numParams=sizeof(speechPlayer_frame_t)/sizeof(speechPlayer_frameParam_t);

/* ============================================================================
 * Optional per-frame voice quality extensions (DSP v5+)
 * ============================================================================
 *
 * These parameters are intentionally kept out of speechPlayer_frame_t so the
 * original 47-parameter ABI stays stable.
 *
 * All fields are expected to be in the range [0.0, 1.0] unless documented
 * otherwise. Values outside that range may be clamped by the DSP.
 */
typedef struct {
	double creakiness;      // laryngealization / creaky voice (e.g. Danish stød)
	double breathiness;     // breath noise mixed into voicing
	double jitter;          // pitch period variation (irregular F0)
	double shimmer;         // amplitude variation (irregular loudness)
	double sharpness;       // glottal closure sharpness MULTIPLIER (0=use SR default, 0.5-2.0 typical)
	
	// Formant end targets for within-frame ramping (like endVoicePitch but for formants)
	// NAN = no ramping (use base value throughout frame)
	// Any other value = ramp from base to this value over the frame duration
	double endCf1;          // Cascade F1 end target (Hz), NAN = no ramp
	double endCf2;          // Cascade F2 end target (Hz), NAN = no ramp
	double endCf3;          // Cascade F3 end target (Hz), NAN = no ramp
	double endPf1;          // Parallel F1 end target (Hz), NAN = no ramp
	double endPf2;          // Parallel F2 end target (Hz), NAN = no ramp
	double endPf3;          // Parallel F3 end target (Hz), NAN = no ramp

	/* =========================================================================
	 * Optional pitch contour model (DSP v6+)
	 * =========================================================================
	 *
	 * Fujisaki-Bartman / DECTalk-style pitch contour model.
	 *
	 * IMPORTANT: All time units for this model are in *samples* (not milliseconds).
	 * This matches the reference Python model you provided.
	 *
	 * Behavior:
	 * - Base pitch still comes from speechPlayer_frame_t::voicePitch (with the existing
	 *   per-sample ramp toward endVoicePitch).
	 * - The model outputs a multiplicative contour which modulates that base pitch.
	 * - Phrase/accent fields act as *triggers* (use rising edges).
	 */
	double fujisakiEnabled;     // 0.0 = off (legacy behavior), >0.5 = on
	double fujisakiReset;       // rising edge resets model filter state
	double fujisakiPhraseAmp;   // phrase command amplitude (e.g. 1.3)
	double fujisakiPhraseLen;   // phrase filter L (samples to peak). 0 = use default
	double fujisakiAccentAmp;   // accent command amplitude (e.g. 0.4)
	double fujisakiAccentDur;   // accent duration D (samples). 0 = use default
	double fujisakiAccentLen;   // accent filter L (samples to peak). 0 = use default

	/* =========================================================================
	 * Per-parameter transition speed scales (DSP v7)
	 * =========================================================================
	 *
	 * Control how fast individual formant groups reach their targets during
	 * the fade between frames.
	 *
	 * 0.0 = no override (use base fade rate, equivalent to 1.0)
	 * 0.3 = reach target in 30% of fade time, then hold
	 * 1.0 = use full fade duration (default behavior)
	 * >1.0 = slower than base fade (use sparingly)
	 *
	 * These ONLY affect formant frequencies and bandwidths.
	 * Amplitude parameters always use the base fade rate.
	 * This is intentional — separating amplitude from formant timing
	 * causes audible artifacts (see: dual-fade experiment, RIP).
	 */
	double transF1Scale;    // cf1, pf1, cb1, pb1
	double transF2Scale;    // cf2, pf2, cb2, pb2
	double transF3Scale;    // cf3, pf3, cb3, pb3
	double transNasalScale; // cfN0, cfNP, cbN0, cbNP, caNP

	/* Amplitude crossfade curve selection (DSP v7.1)
	 *
	 * Controls the interpolation curve used for amplitude/gain parameters
	 * (voiceAmplitude, aspirationAmplitude, fricationAmplitude,
	 * voiceTurbulenceAmplitude, preFormantGain) during frame transitions.
	 *
	 * 0.0 = linear crossfade (legacy default — works fine when both frames
	 *        have similar total energy, e.g. vowel→vowel)
	 * 1.0 = equal-power crossfade (sin/cos curves — maintains constant
	 *        total energy across source transitions like voiced→voiceless)
	 *
	 * The frontend sets this based on whether the transition involves a
	 * change in voicing source. The DSP does not need phoneme awareness.
	 */
	double transAmplitudeMode;

	/* =========================================================================
	 * Higher cascade formants F7/F8 (DSP v8)
	 * =========================================================================
	 *
	 * At sample rates >= 22050 Hz, Klatt's original 5-6 cascade formants leave
	 * a spectral gap above F6 (~5.5 kHz).  The implicit z-transform phantom
	 * poles that filled this gap at the original 10 kHz rate are inaudible at
	 * 44.1 kHz.  Adding explicit F7 (~6.5 kHz) and F8 (~7.5 kHz) restores the
	 * spectral envelope above F6, adding "presence" and "air."
	 *
	 * Defaults from Rabiner 1968, as cited in the QLatt project
	 * (https://github.com/nicclase/qlatt):
	 *   F7: 6500 Hz, BW 720 Hz  (Q ~ 9)
	 *   F8: 7500 Hz, BW 1250 Hz (Q ~ 6)
	 *
	 * These are vocal-tract-length properties, not vowel-dependent — they
	 * stay roughly constant across all phonemes.  Individual phonemes can
	 * override them via YAML keys cf7/cb7/cf8/cb8.
	 *
	 * At low sample rates the cascade Nyquist-proximity fade automatically
	 * mutes them (ratio > 0.85 → bypass), so they cost nothing at 11025 Hz.
	 */
	double cf7;   /* F7 frequency (Hz).  Default 6500.0 */
	double cb7;   /* F7 bandwidth (Hz).  Default 720.0  */
	double cf8;   /* F8 frequency (Hz).  Default 7500.0 */
	double cb8;   /* F8 bandwidth (Hz).  Default 1250.0 */

	/* Source amplitude timing — noise hold ratio.
	 *
	 * Delays fadeout of old frame's noise sources (fricationAmplitude,
	 * aspirationAmplitude) during crossfades, creating temporal overlap
	 * with the incoming frame's voicing.
	 *
	 * 0.0 = no hold (legacy, noise fades at same rate as everything)
	 * 0.3 = old noise holds for first 30% of fade, then fades over 70%
	 *
	 * voiceAmplitude is NOT affected — it uses the normal fade ratio.
	 * This creates overlap: voicing ramps in while frication still holds.
	 */
	double transSourceHoldRatio;

	/* Voicing onset hold — delays ramp-in of new voiceAmplitude.
	 *
	 * Delays onset of new frame's voicing during crossfades, keeping
	 * voiceAmplitude at the OLD value for the first fraction of the fade.
	 *
	 * 0.0 = no hold (legacy, voicing ramps immediately)
	 * 0.25 = voicing stays at old value for first 25%, then ramps over 75%
	 *
	 * Combined with transSourceHoldRatio, this creates temporal structure:
	 *   sourceHold=0.40, voicingHold=0.25 →
	 *   0-25%:  frication=HIGH, voicing=ZERO  (pure affricate release)
	 *   25-40%: frication=HIGH, voicing=ramping (overlap)
	 *   40-100%: frication=fading, voicing=ramping (transition)
	 *
	 * Research shows per-parameter timing is key to natural affricates —
	 * each source should have its own onset/offset schedule.
	 */
	double transVoicingHoldRatio;

	/* =========================================================================
	 * Independent cascade anti-resonator amplitude (DSP v9)
	 * =========================================================================
	 *
	 * The cascade rN0 anti-resonator (controlled by cfN0/cbN0 on
	 * speechPlayer_frame_t) has historically been coupled to the nasal pole
	 * path — the anti-resonated signal only reaches the output when caNP
	 * (cascade nasal POLE amplitude) is > 0. That coupling assumed every
	 * phoneme needing an anti-resonance is nasal.
	 *
	 * Spanish /l/ (and other laterals) need a side-branch anti-resonance
	 * WITHOUT a nasal pole. caN0 is the independent amplitude that mixes
	 * the anti-resonated signal into the cascade's direct path, regardless
	 * of caNP.
	 *
	 * 0.0 = disabled (legacy behavior — rN0 contributes only via nasal path
	 *        when caNP > 0)
	 * 1.0 = fully replace direct input with rN0-applied signal
	 * 0<x<1 = crossfade mix (partial anti-resonance)
	 *
	 * Typical use: /l/ with cfN0 ≈ 1500-2500 Hz for the lateral zero,
	 * cbN0 ≈ 200-300 Hz (broad notch matching the physical side cavity),
	 * caN0 = 1.0. Leaves caNP at 0 so no nasal pole is added.
	 */
	double caN0;
} speechPlayer_frameEx_t;

// Default values for frameEx parameters. Used when:
// - Old callers pass a smaller struct (partial copy gets padded with these)
// - No frameEx is provided at all
// This makes it safe to add new parameters with non-zero defaults in the future.
#include <cmath>  // for NAN
static const speechPlayer_frameEx_t speechPlayer_frameEx_defaults = {
	0.0,  // creakiness: none
	0.0,  // breathiness: none
	0.0,  // jitter: none
	0.0,  // shimmer: none
	0.0,  // sharpness: 0 means "use sample-rate default"
	NAN,  // endCf1: no ramping
	NAN,  // endCf2: no ramping
	NAN,  // endCf3: no ramping
	NAN,  // endPf1: no ramping
	NAN,  // endPf2: no ramping
	NAN,  // endPf3: no ramping
	0.0,  // fujisakiEnabled: off
	0.0,  // fujisakiReset
	0.0,  // fujisakiPhraseAmp
	0.0,  // fujisakiPhraseLen
	0.0,  // fujisakiAccentAmp
	0.0,  // fujisakiAccentDur
	0.0,  // fujisakiAccentLen
	0.0,  // transF1Scale: no override
	0.0,  // transF2Scale: no override
	0.0,  // transF3Scale: no override
	0.0,  // transNasalScale: no override
	0.0,   // transAmplitudeMode: linear (legacy)
	6500.0, // cf7: Rabiner 1968 default
	720.0,  // cb7: Rabiner 1968 default
	7500.0, // cf8: Rabiner 1968 default
	1250.0, // cb8: Rabiner 1968 default
	0.0,    // transSourceHoldRatio: no hold (legacy)
	0.0,    // transVoicingHoldRatio: no hold (legacy)
	0.0     // caN0: independent anti-resonator amp (DSP v9, disabled by default)
};

const int speechPlayer_frameEx_numParams=sizeof(speechPlayer_frameEx_t)/sizeof(double);


class FrameManager {
	public:
	static FrameManager* create(); //factory function

	// Core frame queue (legacy)
	virtual void queueFrame(speechPlayer_frame_t* frame, unsigned int minNumSamples, unsigned int numFadeSamples, int userIndex, bool purgeQueue)=0;

	// Extended frame queue (DSP v5+): optional per-frame voice quality params.
	// If frameEx is NULL or frameExSize is 0, behavior must match queueFrame() exactly.
	// frameExSize allows safe extension of frameEx struct in future versions.
	virtual void queueFrameEx(speechPlayer_frame_t* frame, const speechPlayer_frameEx_t* frameEx, unsigned int frameExSize, unsigned int minNumSamples, unsigned int numFadeSamples, int userIndex, bool purgeQueue)=0;

	// Fetch the current frame (and optional extended params) for the next output sample.
	// The returned pointers are owned by the FrameManager and remain valid until the next call.
	// If there is no active frame (silence), returns NULL and sets *outFrameEx to NULL.
	virtual const speechPlayer_frame_t* const getCurrentFrameWithEx(const speechPlayer_frameEx_t** outFrameEx)=0;

	// Back-compat convenience wrapper.
	virtual const speechPlayer_frame_t* const getCurrentFrame() { return getCurrentFrameWithEx(NULL); }

	virtual const int getLastIndex()=0;

	// Check if a purge happened since last check (and clear the flag).
	// This allows the wave generator to detect interrupts even when frames continue.
	virtual bool checkAndClearPurgeFlag()=0;

	// Pure virtual, but still needs a definition.
	virtual ~FrameManager()=0;
};



// MSVC accepts `=0 {}` in-class, but GCC/Clang reject it.
// Keep the same ABI/intent while staying standard-compliant.
inline FrameManager::~FrameManager() {}

#endif