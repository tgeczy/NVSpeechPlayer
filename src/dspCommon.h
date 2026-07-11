/*
TGSpeechBox — DSP tuning constants, PRNG, lowpass, and utility classes.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSPEECHBOX_DSPCOMMON_H
#define TGSPEECHBOX_DSPCOMMON_H

#define _USE_MATH_DEFINES

#include <cmath>
#include <cstdint>
#include <cstdlib>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const double PITWO = M_PI * 2;

// ============================================================================
// Numeric helpers
// ============================================================================

static inline double clampDouble(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// -----------------------------------------------------------------------------
// Nyquist safety clamp for resonator center frequencies
//
// A resonator asked to sit at or above Nyquist hard-disables into passthrough.
// For the parallel bank's differential mix (resonate(in)-in)*amp, passthrough
// is EXACT silence — at 11025 Hz output this muted every phoneme whose energy
// lived in a single parallel formant above ~5512 Hz (Spanish /s/ pf6 6500,
// issue #100). Instead, slide the pole to just under Nyquist and widen the
// bandwidth proportionally so the peak survives as a wideband HF shelf.
//
// k = 0.475 (not lower): at 11025 Hz this places the pole at ~5237 Hz, which
// retains 87-99% of /s/-zone energy on the harness corpus; placements below
// ~5000 Hz fall into the frication lowpass shadow (kFricSustainFc_11k) and
// underperform. No-op at 16000+ Hz where all pack formants fit under the cap.
// -----------------------------------------------------------------------------

const double kResonatorNyquistClampRatio = 0.475;

static inline void clampResonatorToNyquist(double& freqHz, double& bwHz, int sampleRate) {
    const double fMax = kResonatorNyquistClampRatio * (double)sampleRate;
    if (freqHz > fMax) {
        bwHz *= freqHz / fMax;
        freqHz = fMax;
    }
}

// -----------------------------------------------------------------------------
// Formant sweep bandwidth handling
//
// Sweeping a resonator's center frequency while holding bandwidth constant changes
// effective Q (= F/B). For upward sweeps this narrows the resonance and can yield
// a "whistly / boxy" quality as individual harmonics get over-emphasized.
// To keep sweeps sounding speech-like we cap Q by widening bandwidth as needed.
//
// Applied only when the current frame provides endCf/endPf targets (diphthongs etc.).
// -----------------------------------------------------------------------------

static inline double bandwidthForSweep(double freqHz, double baseBwHz, double qMax, double bwMinHz, double bwMaxHz) {
    if (!std::isfinite(freqHz) || !std::isfinite(baseBwHz) || freqHz <= 0.0 || baseBwHz <= 0.0) {
        return baseBwHz;
    }
    // Enforce minimum bandwidth (and thus a maximum Q).
    double bw = baseBwHz;
    double bwFromQ = freqHz / qMax;
    if (bwFromQ > bw) bw = bwFromQ;
    return clampDouble(bw, bwMinHz, bwMaxHz);
}

// ============================================================================
// Formant sweep Q limits
// ============================================================================

// Limits used only during within-phoneme formant sweeps (endCf/endPf).
// These keep resonators from becoming ultra-high-Q as formants move upward.
const double kSweepQMaxF1 = 10.0;
const double kSweepQMaxF2 = 18.0;
const double kSweepQMaxF3 = 20.0;

const double kSweepBwMinF1 = 30.0;
const double kSweepBwMinF2 = 40.0;
const double kSweepBwMinF3 = 60.0;
const double kSweepBwMax = 1000.0;

// ============================================================================
// Tuning knobs (DSP-layer)
// ============================================================================

// Radiation characteristic: 
// The derivative (dFlow) is naturally very quiet compared to the flow.
// We apply gain to dFlow to match the loudness of flow.
const double kRadiationDerivGainBase = 5.0;      
const double kRadiationDerivGainRefSr = 22050.0; 

// Turbulence gating curvature when glottis is open.
const double kTurbulenceFlowPower = 1.5;

// Frication shaping
const double kFricNoiseScale = 0.175;
const double kFricSoftClipK = 0.18;
const double kBypassMinGain = 0.70;
const double kBypassVoicedDuck = 0.20;
const double kVoicedFricDuck = 0.18;
const double kVoicedFricDuckPower = 1.0;

// Pitch-synchronous voiced-fricative noise modulation.
// Turbulence at a supraglottal constriction tracks transglottal airflow, so
// /z/,/v/,/ʒ/,/ð/ frication should pulse with the glottal open phase — the
// buzzing-hiss signature that separates a voiced fricative from /s/, and that
// survives band-limiting because it lives in the (in-band) modulation rate,
// not the (out-of-band) HF spectral peak (Rabiner 1968; Stevens 1971).
//
// Applied as fricNoise *= (1 + depth * flowAC), where flowAC is the zero-mean
// glottal-flow envelope. Because flowAC averages to 0, the mean hiss energy
// (and loudness) is preserved — the noise buzzes without getting fainter.
// Gated on genuine voiced frication (voiceAmplitude AND fricationAmplitude
// above thresholds) so vowels and voiceless /s/ are untouched.
//   kVoicedFricModDepth: buzz strength (ear-tunable; peak flowAC ~0.6).
//   kVoicedFricProductThresh: gate on va*fricAmp, which is high only when
//     voicing AND frication are strong SIMULTANEOUSLY (true /z/,/v/ ~0.74).
//     A voiceless-fricative→vowel transition crosses over at va≈fricAmp≈0.5,
//     product ~0.23, so it stays below the gate — no leakage onto s+vowel
//     words. Using the product (not two separate thresholds) is what keeps
//     the effect localized to sustained voiced frication.
// Depth 0.5 keeps radiated modulation near m≈0.5, where real voiced-fricative
// AM saturates (Pincas & Jackson 2005; Klatt's classic 50% square ≈ 6 dB
// peak-to-trough). Higher reads as artificial "over-buzz".
const double kVoicedFricModDepth       = 0.5;
const double kVoicedFricProductThresh  = 0.35;

// Consonant clarity boost (Consonant–Vowel intensity Ratio, CVR).
// Band-limiting + mobile noise-suppression destroy consonant PLACE cues and
// duck quiet HF frication as "noise" (Miller & Nicely 1955); voicing/nasality
// survive, place does not. Raising sustained-fricative level relative to
// vowels restores audibility: +7–21% consonant recognition at low SNR
// (Gordon-Salant 1986/87), strongest for fricatives/affricates (Sroka &
// Braida 1989 — NOT voiceless stops, so stop bursts are gated OUT via
// burstiness). Vowels carry no frication so are untouched; this raises the
// consonant-to-vowel ratio exactly where the device is quietest.
// Value = linear boost at full (low-burstiness) frication:
//   0.4 ≈ +3 dB, 0.6 ≈ +4 dB, 1.0 ≈ +6 dB. Keep ≤~6 dB — more reads as
// unnatural loudness and can trip device AGC (which would re-duck us).
const double kConsonantClarityBoost    = 0.55;

// ------------------------------------------------------------
// Adaptive frication lowpass (targets stop bursts, preserves sustained fricatives)
// ------------------------------------------------------------
// For bursts (fast rise in fricationAmplitude): use a lower cutoff (more lowpass)
// to stop "everything turns into /t/".
// For sustained fricatives (stable frication): use a higher cutoff so /s/ stays crisp.
// This helps distinguish /k/ (more mid-weighted) from /t/ (sharper) by taking
// the top edge off only at the burst onset.

// Sample-rate-aware cutoff frequencies for frication
// At 11025 Hz, Nyquist is ~5512 Hz so we need lower cutoffs
const double kFricBurstFc_11k   = 3500.0;   // 11025 Hz (Nyquist ~5512)
const double kFricSustainFc_11k = 3500.0;
const double kFricBurstFc_16k   = 4500.0;   // 16000 Hz (Nyquist 8000)
const double kFricSustainFc_16k = 6500.0;
const double kFricBurstFc_22k   = 5000.0;   // 22050 Hz (Nyquist ~11025)
const double kFricSustainFc_22k = 8500.0;
const double kFricBurstFc_44k   = 5000.0;   // 44100 Hz (Nyquist ~22050) — plateau
const double kFricSustainFc_44k = 8500.0;   // plateau — speech fricatives peak ~8-9 kHz

// Sample-rate-aware cutoff frequencies for aspiration burst LP.
// Monotonic ladder: higher sample rate = more bandwidth = higher cutoff.
const double kAspBurstFc_11k = 2400.0;   // 11025 Hz
const double kAspBurstFc_16k = 3200.0;   // 16000 Hz
const double kAspBurstFc_22k = 3800.0;   // 22050 Hz
const double kAspBurstFc_44k = 4000.0;   // 44100 Hz — plateau

// Burstiness detection sensitivity (higher = more sensitive to fast rises)
const double kBurstinessScale = 25.0;

// ------------------------------------------------------------
// Breathiness macro tuning (per-frame tilt offset)
// ------------------------------------------------------------
// Breathiness already drives turbulence, OQ, and pulse shape.
// This adds per-frame spectral TILT offset for true airy voice quality.
// Without tilt, you get "noisy voicing" (hoarseness).
// With tilt, you get "breathy voicing" (airy, soft highs).

// Max tilt offset at breathiness=1.0 (positive = darker/softer highs for VOICED)
const double kBreathinessTiltMaxDb = 6.0;

// Max aspiration tilt offset at breathiness=1.0 (NEGATIVE = darker/softer noise)
// This makes the breath noise spectrally match the softened voice
const double kBreathinessAspTiltMaxDb = -8.0;

// Smoothing time constant to prevent clicks when breathiness changes
const double kBreathinessTiltSmoothMs = 8.0;

// ============================================================================
// FastRandom — thread-local PRNG (replaces stdlib rand())
// ============================================================================
// Linear Congruential Generator - fast, no locking, good enough spectral
// properties for audio noise. Each NoiseGenerator/VoiceGenerator instance
// gets its own state, eliminating thread contention.

class FastRandom {
private:
    uint32_t seed;

public:
    FastRandom(uint32_t s = 12345): seed(s) {}

    void setSeed(uint32_t s) { seed = s; }

    // Returns [0, 1)
    inline double nextDouble() {
        seed = seed * 1664525u + 1013904223u;  // LCG constants from Numerical Recipes
        return (double)(seed >> 1) * (1.0 / 2147483648.0);
    }

    // Returns [-1, 1)
    inline double nextBipolar() {
        seed = seed * 1664525u + 1013904223u;
        return (double)((int32_t)seed) * (1.0 / 2147483648.0);
    }
};

// ============================================================================
// NoiseGenerator — brownish and white noise
// ============================================================================

class NoiseGenerator {
private:
    double lastValue;
    FastRandom rng;

public:
    NoiseGenerator(): lastValue(0.0), rng(54321) {}

    void reset() {
        lastValue=0.0;
    }

    // Brownish noise (smoothed random) - original behavior for frication etc.
    double getNext() {
        lastValue=(rng.nextDouble()-0.5)+0.75*lastValue;
        return lastValue;
    }
    
    // White noise - flat spectrum, better for aspiration tilt to act on
    double white() {
        return rng.nextBipolar();
    }
};

// ============================================================================
// FrequencyGenerator — phase accumulator
// ============================================================================

class FrequencyGenerator {
private:
    int sampleRate;
    double lastCyclePos;

public:
    FrequencyGenerator(int sr): sampleRate(sr), lastCyclePos(0) {}

    void reset() {
        lastCyclePos=0;
    }

    double getNext(double frequency) {
        double cyclePos=fmod((frequency/sampleRate)+lastCyclePos,1);
        lastCyclePos=cyclePos;
        return cyclePos;
    }
};

// ============================================================================
// OnePoleLowpass — simple one-pole lowpass for adaptive frication filtering
// ============================================================================

class OnePoleLowpass {
private:
    int sampleRate;
    double alpha;
    double z;

public:
    OnePoleLowpass(int sr): sampleRate(sr), alpha(0.0), z(0.0) {}

    void setCutoffHz(double fcHz) {
        if (fcHz < 10.0) fcHz = 10.0;
        double nyq = 0.5 * (double)sampleRate;
        if (fcHz > nyq * 0.95) fcHz = nyq * 0.95;
        alpha = exp(-PITWO * fcHz / (double)sampleRate);
    }

    double process(double x) {
        z = (1.0 - alpha) * x + alpha * z;
        return z;
    }

    void reset() { z = 0.0; }
};

#endif // TGSPEECHBOX_DSPCOMMON_H
