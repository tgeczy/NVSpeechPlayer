/*
TGSpeechBox — LF glottal source with tilt, breathiness, and tremor.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSPEECHBOX_VOICEGENERATOR_H
#define TGSPEECHBOX_VOICEGENERATOR_H

#include "dspCommon.h"
#include "pitchModel.h"
#include "frame.h"
#include "voicingTone.h"

class VoiceGenerator {
private:
    int sampleRate;
    FrequencyGenerator pitchGen;
    FrequencyGenerator vibratoGen;
    FrequencyGenerator tremorGen;  // Slow LFO for elderly/shaky voice (~5.5 Hz)
    NoiseGenerator aspirationGen;

    // Optional Fujisaki-Bartman pitch contour model (DSP v6+)
    FujisakiBartmanPitch fujisakiPitch;
    bool fujisakiWasEnabled;
    double lastFujisakiReset;
    double lastFujisakiPhraseAmp;
    double lastFujisakiAccentAmp;
    double lastFlow;
    double lastVoicedIn;
    double lastVoicedOut;
    double lastVoicedSrc;
    double lastAspOut;  // for exposing aspiration to caller

    // Optional noise AM on the glottal cycle (aspiration + frication).
    double noiseGlottalModDepth;
    double lastNoiseMod;

    // Normalized glottal flow (0..1), for pitch-synchronous modulation of
    // voiced-fricative frication noise. Turbulence at a supraglottal
    // constriction tracks transglottal airflow (Rabiner 1968; Stevens 1971),
    // so /z/,/v/,/ʒ/,/ð/ noise should pulse with the glottal open phase —
    // the "buzzing hiss" that a voiceless /s/ never has. flowPeakEnv is a
    // decaying peak tracker used to normalize; flowNormMean is a slow running
    // mean so callers can modulate around it (energy-preserving AC component)
    // rather than only attenuating (which would just make the hiss fainter).
    double flowNorm;
    double flowPeakEnv;
    double flowNormMean;

    // Tremor: slow amplitude modulation for shaky/elderly voice
    double tremorDepth;
    double tremorDepthSmooth;  // Smoothed to prevent clicks on slider change
    double lastTremorSin;      // Stored sin value for both pitch and amp modulation

    // Smooth aspiration gain to avoid clicks when aspirationAmplitude changes quickly.
    double smoothAspAmp;
    bool smoothAspAmpInit;
    double aspAttackCoeff;
    double aspReleaseCoeff;

    // Voiced anti-alias lowpass: prevents harmonic energy near Nyquist from
    // exciting the resonator bank into BLT-warped ringing.  2-pole (12 dB/oct),
    // sample-rate-dependent cutoff.  Bypassed at 44100+ Hz where warping is negligible.
    OnePoleLowpass voicedAntiAliasLp1;
    OnePoleLowpass voicedAntiAliasLp2;
    bool voicedAntiAliasActive;  // false at high SRs where it's not needed

    // Per-frame voice-quality modulation (DSP v5+)
    double lastCyclePos;
    double jitterMul;
    double shimmerMul;
    FastRandom jitterShimmerRng;  // dedicated PRNG for jitter/shimmer

    double voicingPeakPos;
    double voicedPreEmphA;
    double voicedPreEmphMix;

    // Speed quotient: glottal pulse asymmetry (V3 voicingTone)
    double speedQuotient;

    // Dual-oscillator chorus (V5 voicingTone): second phase accumulator
    // at slightly detuned pitch for vocal fold asymmetry simulation.
    double chorusDepth;       // 0.0 = off, 1.0 = full 50/50 blend
    double chorusDetuneHz;    // pitch offset of second oscillator (Hz)
    double chorusPhase;       // second oscillator phase accumulator (0..1)

    // Spectral tilt (Bipolar) for voiced signal
    double tiltTargetTlDb;
    double tiltTlDb;

    double tiltPole;
    double tiltPoleTarget;
    double tiltState;

    double tiltTlAlpha;
    double tiltPoleAlpha;

    double tiltRefHz;
    double tiltLastTlForTargets;

    // Per-frame tilt offset from breathiness (stacks with global tilt)
    double perFrameTiltOffset;        // current smoothed value
    double perFrameTiltOffsetTarget;  // target from current frame's breathiness
    double perFrameTiltOffsetAlpha;   // smoothing coefficient

    // Aspiration/frication tilt (LP/HP crossfade for noise color)
    double aspTiltTargetDb;      // target from slider
    double aspTiltSmoothedDb;    // smoothed value (prevents clicks)
    double aspTiltSmoothAlpha;   // smoothing coefficient
    double aspLpState;           // lowpass state for aspiration tilt filter
    double fricLpState;          // lowpass state for frication tilt (same tilt value)
    
    // Per-frame aspiration tilt offset from breathiness (makes noise softer too)
    double perFrameAspTiltOffset;
    double perFrameAspTiltOffsetTarget;
    double perFrameAspTiltOffsetAlpha;

    // Radiation Gain (Applied ONLY to dFlow)
    double radiationDerivGain;
    
    // Radiation Mix: 0.0 = Flow (Warm), 1.0 = Derivative (Bright)
    double radiationMix;

    // Noise amplitude compensation for sample rate.
    // White noise spectral density drops at higher sample rates (same energy
    // spread over wider bandwidth), making aspiration sound thinner at 44100 Hz.
    // Scale by sqrt(sr/22050) to maintain consistent spectral density through
    // the resonator bank.
    double noiseAmplitudeScale;

    // 2x source oversampling: computes glottal pulse at twice the output rate
    // for cleaner harmonics (less aliasing from sharp LF closure).
    // Active when sampleRate < 44100. The oversampled computation uses
    // sharpness/blend settings appropriate for 2x the output rate.
    bool sourceOversampleActive;
    double osBaseSharpness;      // baseSharpness at 2x sample rate
    double osLfBlendBase;        // lfBlend base at 2x sample rate
    double osLfCap;              // lfCap at 2x sample rate
    double outputBaseSharpness;  // baseSharpness at output sample rate
    double outputLfBlendBase;    // lfBlend base at output sample rate
    double outputLfCap;          // lfCap at output sample rate

    static double clampDouble(double v, double lo, double hi) {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }

    double calcPoleForTiltDb(double refHz, double tiltDb) const {
        if (fabs(tiltDb) < 1e-5) return 0.0;
        
        // POSITIVE TILT (Darken): Solve for attenuation
        if (tiltDb > 0.0) { 
            double nyq = 0.5 * (double)sampleRate;
            if (refHz < 1.0) refHz = 1.0;
            if (refHz > nyq * 0.95) refHz = nyq * 0.95;
            double g = pow(10.0, -tiltDb / 20.0);
            double g2 = g * g;
            double w = PITWO * refHz / (double)sampleRate;
            double cosw = cos(w);
            double A = g2 - 1.0;
            double B = 2.0 * (1.0 - g2 * cosw);
            double disc = B*B - 4.0*A*A; 
            
            if (disc < 0.0) return 0.0; 
            double sqrtDisc = sqrt(disc);
            double denom = 2.0 * A;
            if (fabs(denom) < 1e-18) return 0.0;

            double a1 = (-B + sqrtDisc) / denom;
            double a2 = (-B - sqrtDisc) / denom;
            double a = 0.0;
            bool ok1 = (a1 >= 0.0 && a1 < 1.0);
            bool ok2 = (a2 >= 0.0 && a2 < 1.0);
              if (ok1 && ok2) a = (a1 < a2) ? a1 : a2;
              else if (ok1) a = a1;
              else if (ok2) a = a2;
            else a = a1; // fallback
return clampDouble(a, 0.0, 0.9999);
        }
        // NEGATIVE TILT (Brighten): Solve for boost at Nyquist
        else {
            double targetGain = pow(10.0, -tiltDb / 20.0);
            double a = (1.0 - targetGain) / (1.0 + targetGain);
            return clampDouble(a, -0.9, -0.0001);
        }
    }

    void updateTiltTargets(double tlDbNow) {
        double tl = clampDouble(tlDbNow, -24.0, 24.0);
        tiltPoleTarget = calcPoleForTiltDb(tiltRefHz, tl);

        // RADIATION MIX — models lip radiation (+6 dB/oct differentiator).
        //
        // Additive mode: the derivative is ADDED to flow, not crossfaded.
        // This preserves all bass warmth while layering in upper presence.
        // Baseline at tilt=0: flow + 0.30*derivative (at 16 kHz+).
        //
        // Negative tilt (brightening): ramp boost toward 1.0.
        // Full derivative adds ~+6 dB/oct on top of flow — bright AND warm.
        //
        // Positive tilt (darkening): fade boost toward zero.
        // Pure flow at -12 dB/oct — very dark, no presence.

        // Scale mix by sample rate: at low SR, fewer harmonics exist above F2,
        // so derivative energy crowds near Nyquist and sounds swirly/airy.
        // 16 kHz+ gets full 0.30.  11025 Hz gets ~0.21 (close to old crossfade).
        const double kBaseRadiationMixMax = 0.30;
        const double kRadiationMixSrRef = 16000.0;
        const double kBaseRadiationMix = kBaseRadiationMixMax
            * std::min(1.0, (double)sampleRate / kRadiationMixSrRef);

        if (tl < 0.0) {
            // Brighten: ramp boost from baseline to 1.0 over 10 dB.
            // With additive mode, this adds presence WITHOUT subtracting warmth.
            // At tilt=-10: flow + 1.0*deriv (very bright, still warm).
            double bright = -tl / 10.0;  // 0..1 over -10..-20 dB range
            radiationMix = clampDouble(
                kBaseRadiationMix + bright * (1.0 - kBaseRadiationMix),
                kBaseRadiationMix, 1.0);
        } else {
            // Darken: fade boost to 0 over 12 dB.
            // At tilt=+12: pure flow, no derivative = very dark.
            radiationMix = clampDouble(
                kBaseRadiationMix * (1.0 - tl / 12.0),
                0.0, kBaseRadiationMix);
        }
    }

    double applyTilt(double in) {
        // Smooth the per-frame tilt offset (prevents clicks when breathiness changes)
        perFrameTiltOffset += (perFrameTiltOffsetTarget - perFrameTiltOffset) * perFrameTiltOffsetAlpha;
        
        // Effective tilt = global (speaker identity) + per-frame offset (phonation state)
        double effectiveTilt = tiltTargetTlDb + perFrameTiltOffset;
        
        tiltTlDb += (effectiveTilt - tiltTlDb) * tiltTlAlpha;
        if (fabs(tiltTlDb - tiltLastTlForTargets) > 0.01) {
            updateTiltTargets(tiltTlDb);
            tiltLastTlForTargets = tiltTlDb;
        }
        tiltPole += (tiltPoleTarget - tiltPole) * tiltPoleAlpha;
        double out = (1.0 - tiltPole) * in + tiltPole * tiltState;
        tiltState = out;
        return out;
    }

    // Helper: compute one-pole lowpass alpha from cutoff frequency
    double onePoleAlphaFromFc(double fcHz) const {
        double fc = fcHz;
        double nyq = 0.5 * (double)sampleRate;
        if (fc < 20.0) fc = 20.0;
        if (fc > nyq * 0.95) fc = nyq * 0.95;
        return exp(-PITWO * fc / (double)sampleRate);
    }

    // Compute raw glottal flow at a given cycle phase position.
    // Encapsulates the cosine/LF hybrid waveform and blend logic.
    // Parameters baseSharp, lfBlendB, lfCapV select the sharpness tier
    // (output rate or oversampled rate).
    double flowAtPhase(double cyclePos, double effectiveOQ, double peakPos,
                       double pitchHz, double frameExSharpness,
                       double baseSharp, double lfBlendB, double lfCapV) const {
        if (cyclePos < effectiveOQ) return 0.0;  // Closed phase

        double openLen = 1.0 - effectiveOQ;
        if (openLen < 0.0001) openLen = 0.0001;

        double dt = (pitchHz > 0.0) ? pitchHz / (double)sampleRate : 0.0;
        double denom = openLen - dt;
        if (denom < 0.0001) denom = 0.0001;
        double phase = (cyclePos - effectiveOQ) / denom;
        if (phase < 0.0) phase = 0.0;
        if (phase > 1.0) phase = 1.0;

        // Symmetric cosine flow (original SpeechPlayer)
        double flowCosine;
        if (phase < peakPos) {
            flowCosine = 0.5 * (1.0 - cos(phase * M_PI / peakPos));
        } else {
            flowCosine = 0.5 * (1.0 + cos((phase - peakPos) * M_PI / (1.0 - peakPos)));
        }

        // LF-inspired asymmetric flow
        double flowLF;
        if (phase < peakPos) {
            double t = phase / peakPos;
            double openPower = 2.0 + (speedQuotient - 2.0) * 0.5;
            if (openPower < 1.0) openPower = 1.0;
            if (openPower > 4.0) openPower = 4.0;
            flowLF = pow(t, openPower) * (3.0 - 2.0 * t);
        } else {
            double t = (phase - peakPos) / (1.0 - peakPos);
            double bs = baseSharp;
            if (frameExSharpness > 0.0) {
                bs *= frameExSharpness;
                if (bs < 1.0) bs = 1.0;
                if (bs > 15.0) bs = 15.0;
            }
            double sqFactor = 0.4 + (speedQuotient - 0.5) * (0.6 / 1.5);
            if (sqFactor < 0.3) sqFactor = 0.3;
            if (sqFactor > 2.0) sqFactor = 2.0;
            flowLF = pow(1.0 - t, bs * sqFactor);
        }

        // Cosine/LF blend with frameExSharpness modulation
        double lfBlend = lfBlendB;
        const double sharpMul = (frameExSharpness > 0.0) ? frameExSharpness : 1.0;
        const double lfScale = pow(clampDouble(sharpMul, 0.25, 3.0), 0.25);
        lfBlend = clampDouble(lfBlend * lfScale, 0.0, lfCapV);

        return (1.0 - lfBlend) * flowCosine + lfBlend * flowLF;
    }

    // Aspiration/frication tilt: LP/HP crossfade for noise color
    // Negative = darker, Positive = brighter
    // Uses smoothing to prevent clicks on parameter changes
    void setAspirationTiltDbPerOct(double tiltDb) {
        aspTiltTargetDb = clampDouble(tiltDb, -24.0, 24.0);
    }

    double applyAspirationTilt(double x) {
        // Smooth the per-frame aspiration tilt offset (from breathiness)
        perFrameAspTiltOffset += (perFrameAspTiltOffsetTarget - perFrameAspTiltOffset) * perFrameAspTiltOffsetAlpha;
        
        // Smooth the global tilt parameter (prevents clicks from instant slider changes)
        aspTiltSmoothedDb += (aspTiltTargetDb - aspTiltSmoothedDb) * aspTiltSmoothAlpha;
        
        // Effective tilt = global (speaker setting) + per-frame (breathiness)
        double t = aspTiltSmoothedDb + perFrameAspTiltOffset;

        // Effect amount 0..1, with perceptual curve
        double amt = clampDouble(fabs(t) / 18.0, 0.0, 1.0);
        amt = pow(amt, 0.65);

        // Cutoff based on magnitude only (continuous at t=0, no jump)
        double fc = 6000.0 - 4500.0 * amt;  // 6k -> 1.5k as amt rises
        double a = onePoleAlphaFromFc(fc);

        // Always update filter state (prevents state freeze clicks)
        aspLpState = (1.0 - a) * x + a * aspLpState;
        double lp = aspLpState;
        double hp = x - lp;

        // Branchless: darken subtracts hp, brighten adds hp
        double brightAmt = (t > 0.0) ? amt : 0.0;
        double darkAmt   = (t < 0.0) ? amt : 0.0;
        const double kBright = 1.25;
        return x + hp * (kBright * brightAmt - darkAmt);
    }

public:
    bool glottisOpen;
    bool cycleJustWrapped;  // true for one sample at the start of each new glottal cycle

    // Frication tilt: same algorithm, separate state, shares smoothed tilt value
    double applyFricationTilt(double x) {
        // Use the already-smoothed tilt value from aspiration
        double t = aspTiltSmoothedDb;

        double amt = clampDouble(fabs(t) / 18.0, 0.0, 1.0);
        amt = pow(amt, 0.65);

        double fc = 6000.0 - 4500.0 * amt;
        double a = onePoleAlphaFromFc(fc);

        // Always update filter state
        fricLpState = (1.0 - a) * x + a * fricLpState;
        double lp = fricLpState;
        double hp = x - lp;

        double brightAmt = (t > 0.0) ? amt : 0.0;
        double darkAmt   = (t < 0.0) ? amt : 0.0;
        const double kBright = 1.25;
        return x + hp * (kBright * brightAmt - darkAmt);
    }

    VoiceGenerator(int sr): sampleRate(sr), pitchGen(sr), vibratoGen(sr), tremorGen(sr), aspirationGen(),
        fujisakiPitch(sr), fujisakiWasEnabled(false),
        lastFujisakiReset(0.0), lastFujisakiPhraseAmp(0.0), lastFujisakiAccentAmp(0.0),
        lastFlow(0.0), lastVoicedIn(0.0), lastVoicedOut(0.0), lastVoicedSrc(0.0), lastAspOut(0.0),
        noiseGlottalModDepth(0.0), lastNoiseMod(1.0),
        flowNorm(0.0), flowPeakEnv(0.0), flowNormMean(0.0),
        tremorDepth(0.0), tremorDepthSmooth(0.0), lastTremorSin(0.0),
        smoothAspAmp(0.0), smoothAspAmpInit(false),
        aspAttackCoeff(0.0), aspReleaseCoeff(0.0),
        voicedAntiAliasLp1(sr), voicedAntiAliasLp2(sr), voicedAntiAliasActive(false),
        lastCyclePos(0.0), jitterMul(1.0), shimmerMul(1.0), jitterShimmerRng(98765),
        glottisOpen(false),
        voicingPeakPos(0.91), voicedPreEmphA(0.92), voicedPreEmphMix(0.35),
        tiltTargetTlDb(0.0), tiltTlDb(0.0),
        tiltPole(0.0), tiltPoleTarget(0.0), tiltState(0.0),
        tiltTlAlpha(0.0), tiltPoleAlpha(0.0),
        tiltRefHz(3000.0),
        tiltLastTlForTargets(1e9),
        perFrameTiltOffset(0.0), perFrameTiltOffsetTarget(0.0), perFrameTiltOffsetAlpha(0.0),
        aspTiltTargetDb(0.0), aspTiltSmoothedDb(0.0), aspTiltSmoothAlpha(0.0),
        aspLpState(0.0), fricLpState(0.0),
        perFrameAspTiltOffset(0.0), perFrameAspTiltOffsetTarget(0.0), perFrameAspTiltOffsetAlpha(0.0),
        radiationDerivGain(1.0),
        radiationMix(0.0) {

        const double tlSmoothMs = 8.0;
        const double poleSmoothMs = 5.0;

        tiltTlAlpha = 1.0 - exp(-1.0 / (sampleRate * (tlSmoothMs * 0.001)));
        tiltPoleAlpha = 1.0 - exp(-1.0 / (sampleRate * (poleSmoothMs * 0.001)));
        
        // Per-frame tilt offset smoothing (for breathiness on both voice and aspiration)
        perFrameTiltOffsetAlpha = 1.0 - exp(-1.0 / (sampleRate * (kBreathinessTiltSmoothMs * 0.001)));
        perFrameAspTiltOffsetAlpha = 1.0 - exp(-1.0 / (sampleRate * (kBreathinessTiltSmoothMs * 0.001)));

        // Aspiration tilt smoothing (10ms removes clicks without feeling laggy)
        const double aspTiltSmoothMs = 10.0;
        aspTiltSmoothAlpha = 1.0 - exp(-1.0 / (sampleRate * (aspTiltSmoothMs * 0.001)));

        // Aspiration gain smoothing (attack/release in ms).
        // This helps avoid random clicks when aspirationAmplitude changes quickly.
        const double kAspAmpAttackMs = 1.0;
        const double kAspAmpReleaseMs = 12.0;
        aspAttackCoeff = 1.0 - exp(-1.0 / (0.001 * kAspAmpAttackMs * sampleRate));
        aspReleaseCoeff = 1.0 - exp(-1.0 / (0.001 * kAspAmpReleaseMs * sampleRate));

        // Voiced anti-alias lowpass: sample-rate-dependent cutoff.
        // Prevents harmonic energy near Nyquist from exciting resonators
        // into BLT-warped ringing (trapezoidal SVF has same warping as BLT).
        // At 44100+ Hz the warping is negligible, so we bypass entirely.
        // Disable voiced anti-alias LP at 22050+ Hz.  The cascade
        // Nyquist fade already protects resonators from BLT warping,
        // and at sharpness 3.0 the harmonic energy near Nyquist is low.
        // At the old 6500 Hz cutoff, F7/F8 inputs lost 3-5 dB.
        if (sampleRate < 22050) {
            voicedAntiAliasActive = true;
            double aaFc;
            if (sampleRate <= 11025) {
                aaFc = 4000.0;       // aggressive — Nyquist is only 5512
            } else if (sampleRate <= 16000) {
                double t = (double)(sampleRate - 11025) / (16000.0 - 11025.0);
                aaFc = 4000.0 + t * 1000.0;   // 4000 -> 5000
            } else {
                double t = (double)(sampleRate - 16000) / (22050.0 - 16000.0);
                aaFc = 5000.0 + t * 1500.0;   // 5000 -> 6500
                if (t > 1.0) aaFc = 6500.0;
            }
            voicedAntiAliasLp1.setCutoffHz(aaFc);
            voicedAntiAliasLp2.setCutoffHz(aaFc);
        } else {
            voicedAntiAliasActive = false;
        }

        double nyq = 0.5 * (double)sampleRate;
        if (tiltRefHz > nyq * 0.95) tiltRefHz = nyq * 0.95;
        if (tiltRefHz < 500.0) tiltRefHz = 500.0;

        radiationDerivGain = kRadiationDerivGainBase * ((double)sampleRate / kRadiationDerivGainRefSr);

        // Fourth root: gentler than sqrt. At 44100 Hz gives ~1.19× (19% boost)
        // instead of sqrt's 1.41× (41%) which over-thickened stop bursts.
        noiseAmplitudeScale = pow((double)sampleRate / 22050.0, 0.25);

        // 2x source oversampling: precompute sharpness/blend at both output
        // and oversampled (2x) rates.  The oversampled path uses sharper LF
        // closure because it has the bandwidth to represent the harmonics;
        // the decimation LP then removes energy above the output Nyquist.
        {
            // Output-rate settings (also used when oversampling is off).
            // Values reduced from pre-F7/F8 defaults because the higher
            // cascade formants now resonate sharp closure harmonics that
            // previously fell into a spectral gap.  Slider 50 (multiplier
            // 1.0) should sound like old slider ~30 at 44100.
            if (sampleRate >= 44100)      outputBaseSharpness = 6.0;
            else if (sampleRate >= 32000) outputBaseSharpness = 4.5;
            else if (sampleRate >= 22050) outputBaseSharpness = 3.0;
            else if (sampleRate >= 16000) outputBaseSharpness = 1.5;
            else                          outputBaseSharpness = 1.3;

            if (sampleRate <= 11025)      { outputLfBlendBase = 0.30; outputLfCap = 0.35; }
            else if (sampleRate >= 16000) { outputLfBlendBase = 1.0;  outputLfCap = 1.0;  }
            else {
                outputLfBlendBase = 0.30 + 0.70 * (double)(sampleRate - 11025) / (16000.0 - 11025.0);
                outputLfCap = 0.85;
            }

            // Oversampled (2x) settings — slightly below output ladder
            // to compensate for half-band decimation filter leakage.
            // 22050→44100 OS would otherwise match native 44100 sharpness,
            // but the simple average doesn't fully suppress aliased harmonics.
            // Disable 2x oversampling at 22050+ Hz.  The half-band
            // decimation (0.5 average) creates -3 dB at Nyquist/2 (~5.5 kHz),
            // starving cascade F7/F8 of harmonic input and producing a
            // "behind a wall" quality.  At sharpness 3.0 the aliased energy
            // from harmonics folding at 11025 Hz is negligible.
            if (sampleRate < 22050) {
                sourceOversampleActive = true;
                int esr = 2 * sampleRate;
                if (esr >= 44100)      osBaseSharpness = 4.5;
                else if (esr >= 32000) osBaseSharpness = 3.5;
                else if (esr >= 22050) osBaseSharpness = 3.5;
                else if (esr >= 16000) osBaseSharpness = 1.5;
                else                    osBaseSharpness = 1.3;

                if (esr <= 11025)      { osLfBlendBase = 0.30; osLfCap = 0.35; }
                else if (esr >= 16000) { osLfBlendBase = 1.0;  osLfCap = 1.0;  }
                else {
                    osLfBlendBase = 0.30 + 0.70 * (double)(esr - 11025) / (16000.0 - 11025.0);
                    osLfCap = 0.85;
                }
            } else {
                sourceOversampleActive = false;
                osBaseSharpness  = outputBaseSharpness;
                osLfBlendBase    = outputLfBlendBase;
                osLfCap          = outputLfCap;
            }
        }

        speechPlayer_voicingTone_t defaults = SPEECHPLAYER_VOICINGTONE_DEFAULTS;
        voicingPeakPos = defaults.voicingPeakPos;
        voicedPreEmphA = defaults.voicedPreEmphA;
        voicedPreEmphMix = defaults.voicedPreEmphMix;
        noiseGlottalModDepth = clampDouble(defaults.noiseGlottalModDepth, 0.0, 1.0);
        speedQuotient = clampDouble(defaults.speedQuotient, 0.5, 4.0);
        chorusDepth = clampDouble(defaults.chorusDepth, 0.0, 1.0);
        chorusDetuneHz = clampDouble(defaults.chorusDetuneHz, 0.5, 5.0);
        chorusPhase = 0.0;
        setTiltDbPerOct(defaults.voicedTiltDbPerOct);
        setAspirationTiltDbPerOct(defaults.aspirationTiltDbPerOct);

        tiltTlDb = tiltTargetTlDb;
        updateTiltTargets(tiltTlDb);
        tiltPole = tiltPoleTarget;
        tiltLastTlForTargets = tiltTlDb;
    }

    void reset() {
        pitchGen.reset();
        vibratoGen.reset();
        aspirationGen.reset();

        // Reset Fujisaki pitch model state so new utterances start clean.
        fujisakiPitch.resetPast();
        fujisakiWasEnabled = false;
        lastFujisakiReset = 0.0;
        lastFujisakiPhraseAmp = 0.0;
        lastFujisakiAccentAmp = 0.0;

        lastFlow=0.0;
        lastVoicedIn=0.0;
        lastVoicedOut=0.0;
        lastVoicedSrc=0.0;
        lastAspOut=0.0;
        lastNoiseMod=1.0;
        flowNorm=0.0;
        flowPeakEnv=0.0;
        flowNormMean=0.0;
        smoothAspAmp = 0.0;
        smoothAspAmpInit = false;
        lastCyclePos = 0.0;
        jitterMul = 1.0;
        shimmerMul = 1.0;
        glottisOpen=false;
        chorusPhase = 0.0;
        aspLpState = 0.0;
        fricLpState = 0.0;
        voicedAntiAliasLp1.reset();
        voicedAntiAliasLp2.reset();
        aspTiltSmoothedDb = aspTiltTargetDb;  // Snap to target on reset
        tiltState = 0.0;  // Reset voiced tilt IIR state to prevent transient
        perFrameTiltOffset = 0.0;
        perFrameTiltOffsetTarget = 0.0;
        perFrameAspTiltOffset = 0.0;
        perFrameAspTiltOffsetTarget = 0.0;
    }

    void setTiltDbPerOct(double tiltVal) {
        tiltTargetTlDb = clampDouble(tiltVal, -24.0, 24.0);
    }

    void setVoicingParams(double peakPos, double preEmphA, double preEmphMix, double tiltDb, double noiseModDepth, double sq, double aspTiltDb) {
        voicingPeakPos = peakPos;
        voicedPreEmphA = preEmphA;
        voicedPreEmphMix = preEmphMix;
        noiseGlottalModDepth = clampDouble(noiseModDepth, 0.0, 1.0);
        speedQuotient = clampDouble(sq, 0.5, 4.0);
        setTiltDbPerOct(tiltDb);
        setAspirationTiltDbPerOct(aspTiltDb);
    }

    void getVoicingParams(double* peakPos, double* preEmphA, double* preEmphMix, double* tiltDb, double* noiseModDepth, double* sq, double* aspTiltDb) const {
        if (peakPos) *peakPos = voicingPeakPos;
        if (preEmphA) *preEmphA = voicedPreEmphA;
        if (preEmphMix) *preEmphMix = voicedPreEmphMix;
        if (tiltDb) *tiltDb = tiltTargetTlDb;
        if (noiseModDepth) *noiseModDepth = noiseGlottalModDepth;
        if (sq) *sq = speedQuotient;
        if (aspTiltDb) *aspTiltDb = aspTiltTargetDb;
    }

    void setSpeedQuotient(double sq) {
        speedQuotient = clampDouble(sq, 0.5, 4.0);
    }

    double getSpeedQuotient() const {
        return speedQuotient;
    }

    void setTremorDepth(double depth) {
        tremorDepth = clampDouble(depth, 0.0, 0.5);
    }

    double getTremorDepth() const {
        return tremorDepth;
    }

    void setChorusParams(double depth, double detuneHz) {
        chorusDepth = clampDouble(depth, 0.0, 1.0);
        chorusDetuneHz = clampDouble(detuneHz, 0.5, 5.0);
    }

    double getChorusDepth() const { return chorusDepth; }
    double getChorusDetuneHz() const { return chorusDetuneHz; }

    double getLastNoiseMod() const { return lastNoiseMod; }

    // Normalized glottal flow (0..1) for pitch-synchronous voiced-fricative
    // noise modulation. 0 when unvoiced (pitchHz<=0) or during closure.
    double getFlowNorm() const { return flowNorm; }

    // AC (zero-mean) component of the normalized glottal flow. Positive near
    // the flow peak, negative during closure. Callers modulate frication as
    // (1 + gain*AC) to add buzz without net loudness change. ~0 when unvoiced.
    double getFlowNormAC() const { return flowNorm - flowNormMean; }

    double getNext(const speechPlayer_frame_t* frame, const speechPlayer_frameEx_t* frameEx) {
        // Optional per-frame voice quality (DSP v5+). If frameEx is NULL, all effects are disabled.
        double creakiness = 0.0;
        double breathiness = 0.0;
        double jitter = 0.0;
        double shimmer = 0.0;
        double frameExSharpness = 0.0;  // 0 = use SR default, >0 = override
        if (frameEx) {
            creakiness = frameEx->creakiness;
            breathiness = frameEx->breathiness;
            jitter = frameEx->jitter;
            shimmer = frameEx->shimmer;
            frameExSharpness = frameEx->sharpness;

            if (!std::isfinite(creakiness)) creakiness = 0.0;
            if (!std::isfinite(breathiness)) breathiness = 0.0;
            if (!std::isfinite(jitter)) jitter = 0.0;
            if (!std::isfinite(shimmer)) shimmer = 0.0;
            if (!std::isfinite(frameExSharpness)) frameExSharpness = 0.0;

            creakiness = clampDouble(creakiness, 0.0, 1.0);
            breathiness = clampDouble(breathiness, 0.0, 1.0);
            jitter = clampDouble(jitter, 0.0, 1.0);
            shimmer = clampDouble(shimmer, 0.0, 1.0);
            frameExSharpness = clampDouble(frameExSharpness, 0.0, 15.0);  // Allow up to 15 for extreme effects
            
            // Perceptual curve for breathiness: makes 0.2–0.6 slider range useful
            if (breathiness > 0.0) {
                breathiness = pow(breathiness, 0.55);
            }
            
            // Breathiness drives per-frame tilt offset (softer highs = airy quality)
            // VOICED: Positive tilt = darker/softer. breathiness 1.0 -> +6 dB/oct darker
            perFrameTiltOffsetTarget = breathiness * kBreathinessTiltMaxDb;
            
            // ASPIRATION/NOISE: Negative tilt = darker. breathiness 1.0 -> -8 dB/oct darker
            // This makes the breath noise spectrally match the softened voice
            perFrameAspTiltOffsetTarget = breathiness * kBreathinessAspTiltMaxDb;
        } else {
            // No frameEx: reset tilt offsets to zero
            perFrameTiltOffsetTarget = 0.0;
            perFrameAspTiltOffsetTarget = 0.0;
        }

        // ------------------------------------------------------------
        // Pitch (F0)
        // ------------------------------------------------------------
        // Base pitch comes from the frame (and can still be linearly ramped via
        // endVoicePitch in FrameManager). Optionally, we can modulate that base
        // pitch with the Fujisaki-Bartman pitch contour model.

        double basePitchHz = frame->voicePitch;
        if (!std::isfinite(basePitchHz) || basePitchHz < 0.0) basePitchHz = 0.0;

        // Fujisaki-Bartman pitch contour (optional)
        double pitchContourMul = 1.0;
        bool useFujisaki = false;
        if (frameEx) {
            double en = frameEx->fujisakiEnabled;
            if (std::isfinite(en) && en > 0.5) {
                useFujisaki = true;
            }
        }

        if (useFujisaki) {
            // Reset model state on rising edge.
            double resetVal = frameEx ? frameEx->fujisakiReset : 0.0;
            if (!std::isfinite(resetVal)) resetVal = 0.0;
            if (resetVal > 0.5 && lastFujisakiReset <= 0.5) {
                fujisakiPitch.resetPast();
                lastFujisakiPhraseAmp = 0.0;
                lastFujisakiAccentAmp = 0.0;
            }
            lastFujisakiReset = resetVal;

            // Phrase trigger: rising edge of fujisakiPhraseAmp.
            double phraseAmp = frameEx ? frameEx->fujisakiPhraseAmp : 0.0;
            if (!std::isfinite(phraseAmp)) phraseAmp = 0.0;
            if (phraseAmp > 0.0 && lastFujisakiPhraseAmp <= 0.0) {
                double pl = frameEx ? frameEx->fujisakiPhraseLen : 0.0;
                if (!std::isfinite(pl)) pl = 0.0;
                int plSamples = (pl > 0.0) ? (int)floor(pl + 0.5) : 0;
                fujisakiPitch.phrase(phraseAmp, plSamples);
            }
            lastFujisakiPhraseAmp = phraseAmp;

            // Accent trigger: rising edge of fujisakiAccentAmp.
            double accentAmp = frameEx ? frameEx->fujisakiAccentAmp : 0.0;
            if (!std::isfinite(accentAmp)) accentAmp = 0.0;
            if (accentAmp > 0.0 && lastFujisakiAccentAmp <= 0.0) {
                double d = frameEx ? frameEx->fujisakiAccentDur : 0.0;
                double al = frameEx ? frameEx->fujisakiAccentLen : 0.0;
                if (!std::isfinite(d)) d = 0.0;
                if (!std::isfinite(al)) al = 0.0;
                int dSamples = (d > 0.0) ? (int)floor(d + 0.5) : 0;
                int alSamples = (al > 0.0) ? (int)floor(al + 0.5) : 0;
                fujisakiPitch.accent(accentAmp, dSamples, alSamples);
            }
            lastFujisakiAccentAmp = accentAmp;

            pitchContourMul = fujisakiPitch.processMultiplier();
            if (!std::isfinite(pitchContourMul) || pitchContourMul <= 0.0) {
                pitchContourMul = 1.0;
            }
            fujisakiWasEnabled = true;
        } else {
            // If the model was previously enabled and is now disabled, clear state so
            // the next enable starts from a clean history.
            if (fujisakiWasEnabled) {
                fujisakiPitch.resetPast();
                fujisakiWasEnabled = false;
                lastFujisakiReset = 0.0;
                lastFujisakiPhraseAmp = 0.0;
                lastFujisakiAccentAmp = 0.0;
            }
        }

        // Vibrato (fraction of a semitone)
        double vibrato=(sin(vibratoGen.getNext(frame->vibratoSpeed)*PITWO)*0.06*frame->vibratoPitchOffset)+1;

        // Tremor: modulation for elderly/shaky voice (4-12 Hz range).
        // Research shows tremor involves F0, amplitude, AND formant instability.
        // Use fast smoothing (only for slider changes, not the tremor itself!)
        const double tremorSmoothAlpha = 0.01;  // Fast: ~6ms at 16kHz (was 0.0002 = 300ms!)
        tremorDepthSmooth += (tremorDepth - tremorDepthSmooth) * tremorSmoothAlpha;
        double tremorPitchMod = 1.0;
        if (tremorDepthSmooth > 0.001) {
            // 5 Hz - slower so each wobble is distinct (research: 4-6 Hz typical)
            double tremorPhase = tremorGen.getNext(5.0);
            lastTremorSin = sin(tremorPhase * PITWO);
            // Add slight irregularity using the jitter RNG for organic feel
            double irregularity = 1.0 + (jitterShimmerRng.nextBipolar()) * 0.15 * tremorDepthSmooth;
            // Pitch tremor: ±35% F0 at full depth - MAXIMUM elderly voice shake!
            tremorPitchMod = 1.0 + (tremorDepthSmooth * 0.70 * lastTremorSin * irregularity);
        } else {
            lastTremorSin = 0.0;
        }

        double pitchHz = basePitchHz * pitchContourMul * vibrato * tremorPitchMod;
        if (!std::isfinite(pitchHz) || pitchHz < 0.0) pitchHz = 0.0;

        // Creaky voice tends to have slightly lower F0 and more irregularity.
        if (creakiness > 0.0) {
            pitchHz *= (1.0 - 0.12 * creakiness);
        }

        // If we are unvoiced, reset per-cycle multipliers so voiced segments restart clean.
        if (pitchHz <= 0.0) {
            jitterMul = 1.0;
            shimmerMul = 1.0;
        }

        // Apply per-cycle jitter multiplier (updated on cycle wraps).
        pitchHz *= jitterMul;

        double cyclePos = pitchGen.getNext(pitchHz > 0.0 ? pitchHz : 0.0);

        // Detect start of a new glottal cycle.
        const bool cycleWrapped = (pitchHz > 0.0) && (cyclePos < lastCyclePos);
        cycleJustWrapped = cycleWrapped;
        lastCyclePos = cyclePos;

        if (cycleWrapped) {
            // Map [0..1] to perceptible ranges.
            // - jitter: relative F0 variation (0.02 = realistic, but inaudible; use 0.15 for testing)
            // - shimmer: relative amplitude variation
            double jitterRel = (jitter * 0.15) + (creakiness * 0.05);
            if (jitterRel > 0.0) {
                double r = jitterShimmerRng.nextBipolar();
                jitterMul = 1.0 + (r * jitterRel);
                if (jitterMul < 0.2) jitterMul = 0.2;
            } else {
                jitterMul = 1.0;
            }

            double shimmerRel = (shimmer * 0.70) + (creakiness * 0.12);
            if (shimmerRel > 0.0) {
                double r = jitterShimmerRng.nextBipolar();
                shimmerMul = 1.0 + (r * shimmerRel);
                if (shimmerMul < 0.0) shimmerMul = 0.0;
            } else {
                shimmerMul = 1.0;
            }
        }

        // Optional Klatt-style glottal-cycle AM for noise sources.
        // When enabled, the second half of the cycle is attenuated.
        // We normalize mean gain to 1.0 so existing amplitude tuning stays sane.
        double noiseMod = 1.0;
        if (noiseGlottalModDepth > 0.0 && pitchHz > 0.0) {
            const double halfCycleAtten = 0.5 * noiseGlottalModDepth; // depth 1.0 => 0.5 attenuation
            noiseMod = (cyclePos < 0.5) ? 1.0 : (1.0 - halfCycleAtten);
            double meanGain = 1.0 - 0.25 * noiseGlottalModDepth;
            if (meanGain < 0.001) meanGain = 0.001;
            noiseMod /= meanGain;
        }
        lastNoiseMod = noiseMod;

        // Aspiration noise: blend white and brownish depending on sample rate.
        // At lower SRs the voiced signal has fewer harmonics (lower sharpness),
        // making white aspiration noise disproportionately prominent through
        // the cascade formants.  Brownish noise (-6 dB/oct rolloff from IIR
        // feedback) naturally reduces high-frequency cascade excitation.
        //   44100+ Hz: pure white (voiced signal is bright enough)
        //   22050 Hz:  70/30 white/brown blend
        //   16000 Hz:  50/50 blend
        //   11025 Hz:  30/70 blend
        double aspWhiteFrac;
        if (sampleRate >= 44100)      aspWhiteFrac = 1.0;
        else if (sampleRate >= 22050) aspWhiteFrac = 0.70;
        else if (sampleRate >= 16000) aspWhiteFrac = 0.50;
        else                          aspWhiteFrac = 0.30;
        double aspNoise = aspWhiteFrac * aspirationGen.white()
                        + (1.0 - aspWhiteFrac) * aspirationGen.getNext();
        double aspBase = 0.10 + (0.15 * breathiness);
        double aspiration = aspNoise * aspBase * noiseAmplitudeScale * noiseMod;
        
        // Apply tilt filter to aspiration (color the noise)
        aspiration = applyAspirationTilt(aspiration);

        double effectiveOQ = frame->glottalOpenQuotient;
        if (effectiveOQ <= 0.0) effectiveOQ = 0.4;
        if (effectiveOQ < 0.10) effectiveOQ = 0.10;
        if (effectiveOQ > 0.95) effectiveOQ = 0.95;

        // Tremor: modulate open quotient for "voice bending" quality change.
        // When vocal fold tension trembles, OQ oscillates between
        // slightly pressed (shorter open) and slightly breathy (longer open).
        // This creates the characteristic tremor "wobble in voice character".
        if (tremorDepthSmooth > 0.001) {
            // At full depth: ±0.15 OQ variation - strong voice quality wobble
            double oqTremorMod = tremorDepthSmooth * 0.30 * lastTremorSin;
            effectiveOQ += oqTremorMod;
            // Keep in valid range
            if (effectiveOQ < 0.10) effectiveOQ = 0.10;
            if (effectiveOQ > 0.95) effectiveOQ = 0.95;
        }

        // Creakiness: shorter open phase (more closed time) in this model.
        if (creakiness > 0.0) {
            effectiveOQ += 0.10 * creakiness;
            if (effectiveOQ > 0.95) effectiveOQ = 0.95;
        }
        
        // Breathiness: much longer open phase (glottis barely closes)
        // True breathy voice = glottis open 85-95% of cycle, not just 70%
        if (breathiness > 0.0) {
            // Push OQ down toward 0.05 at full breathiness
            // From 0.4 default: 0.4 - (0.35 * 1.0) = 0.05
            effectiveOQ -= 0.35 * breathiness;
            // Allow very low OQ for breathiness (nearly always open)
            if (effectiveOQ < 0.05) effectiveOQ = 0.05;
        }

        glottisOpen = (pitchHz > 0.0) && (cyclePos >= effectiveOQ);

        double flow = 0.0;
        // peakPos declared here (not inside glottisOpen) so the chorus
        // oscillator can also use it after the main flow block.
        double peakPos = voicingPeakPos;
        if(glottisOpen) {
            double openLen = 1.0 - effectiveOQ;
            if (openLen < 0.0001) openLen = 0.0001;

            // Per-frame voice quality tweaks to pulse shape:
            // - breathiness nudges the peak later (softer/relaxed)
            // - creakiness nudges the peak earlier (tenser/pressed)
            // - speedQuotient shifts peak position (the real LF model effect!)
            //
            // In Fant's LF model, SQ determines where the flow peaks within
            // the open phase: peakPos = SQ / (1 + SQ).  Our voicingPeakPos
            // (default 0.91) was tuned as if SQ ≈ 10, so we treat SQ=2.0
            // as neutral (no shift) and map deviations to a peak delta:
            //
            //   SQ=0.5  → peak shifts ~-0.20 (more symmetric, softer, breathy)
            //   SQ=1.0  → peak shifts ~-0.10
            //   SQ=2.0  → peak shift  = 0.0  (default, backward compatible)
            //   SQ=3.0  → peak shifts ~+0.05
            //   SQ=4.0  → peak shifts ~+0.08
            //
            // The nonlinear mapping uses SQ/(1+SQ) so the effect is stronger
            // on the "softer" end (where it matters perceptually) and gentle
            // on the "pressed" end (where we're already near the limit).
            double sqPeakDelta = 0.0;
            if (speedQuotient != 2.0) {
                double refPeak = 2.0 / 3.0;                           // 0.6667
                double sqPeak = speedQuotient / (1.0 + speedQuotient); // LF model
                double rawDelta = sqPeak - refPeak;  // negative for SQ<2, positive for SQ>2
                // Scale 0.6: maps LF range into ~±0.20 around voicingPeakPos
                sqPeakDelta = rawDelta * 0.6;
            }
            peakPos = voicingPeakPos + sqPeakDelta
                    + (0.02 * breathiness) - (0.05 * creakiness);

            const double minCloseSamples = 2.0;
            if (pitchHz > 0.0) {
                double periodSamples = (double)sampleRate / pitchHz;
                double minCloseFrac = minCloseSamples / (periodSamples * openLen);
                if (minCloseFrac > 0.5) minCloseFrac = 0.5;
                double limitPeakPos = 1.0 - minCloseFrac;
                if (limitPeakPos < peakPos) peakPos = limitPeakPos;
                if (peakPos < 0.50) peakPos = 0.50;
            }

            // 2x source oversampling: evaluate the glottal waveform at two phase
            // points per output sample using sharper closure (oversampled-rate
            // sharpness), then average for anti-alias decimation.  This lets the
            // LF model produce richer harmonics without aliasing at 22050/16000/11025.
            if (sourceOversampleActive && pitchHz > 0.0) {
                double phaseInc = pitchHz / (double)sampleRate;
                double midCyclePos = cyclePos - phaseInc * 0.5;
                if (midCyclePos < 0.0) midCyclePos += 1.0;

                double flowMid = flowAtPhase(midCyclePos, effectiveOQ, peakPos,
                                             pitchHz, frameExSharpness,
                                             osBaseSharpness, osLfBlendBase, osLfCap);
                double flowCur = flowAtPhase(cyclePos, effectiveOQ, peakPos,
                                             pitchHz, frameExSharpness,
                                             osBaseSharpness, osLfBlendBase, osLfCap);
                // Half-band decimation: null at original Nyquist
                flow = 0.5 * (flowMid + flowCur);
            } else {
                flow = flowAtPhase(cyclePos, effectiveOQ, peakPos,
                                   pitchHz, frameExSharpness,
                                   outputBaseSharpness, outputLfBlendBase, outputLfCap);
            }
        }

        // Dual-oscillator chorus: blend a second phase accumulator at
        // slightly detuned pitch for natural vocal fold asymmetry.
        // The detune is tiny (1-3 Hz), creating subtle cycle-to-cycle
        // variation that thickens the voice without doubling it.
        if (chorusDepth > 0.001 && pitchHz > 0.0) {
            double chorusPitchHz = pitchHz + chorusDetuneHz;
            chorusPhase = fmod(chorusPhase + chorusPitchHz / (double)sampleRate, 1.0);

            double chorusFlow;
            if (sourceOversampleActive) {
                double cInc = chorusPitchHz / (double)sampleRate;
                double cMid = chorusPhase - cInc * 0.5;
                if (cMid < 0.0) cMid += 1.0;
                double cFlowMid = flowAtPhase(cMid, effectiveOQ, peakPos,
                                              pitchHz, frameExSharpness,
                                              osBaseSharpness, osLfBlendBase, osLfCap);
                double cFlowCur = flowAtPhase(chorusPhase, effectiveOQ, peakPos,
                                              pitchHz, frameExSharpness,
                                              osBaseSharpness, osLfBlendBase, osLfCap);
                chorusFlow = 0.5 * (cFlowMid + cFlowCur);
            } else {
                chorusFlow = flowAtPhase(chorusPhase, effectiveOQ, peakPos,
                                         pitchHz, frameExSharpness,
                                         outputBaseSharpness, outputLfBlendBase, outputLfCap);
            }

            // Blend: depth 0 = pure original, depth 1 = 50/50 mix
            double blend = chorusDepth * 0.5;
            flow = flow * (1.0 - blend) + chorusFlow * blend;
        } else if (pitchHz <= 0.0) {
            // Keep chorus oscillator synced when unvoiced
            chorusPhase = 0.0;
        }

        const double flowScale = 1.6;
        flow *= flowScale;

        // Normalized glottal flow for voiced-fricative noise modulation.
        // flow is ~0 during the closed phase and peaks mid-open-phase, so a
        // peak-normalized copy is a ready-made pitch-synchronous envelope that
        // is OQ-aware and needs no separate oscillator. Decaying peak tracker
        // keeps it in [0,1] across pitch/amplitude changes. Unvoiced frames
        // (pitchHz<=0) leave flow at 0, so callers must gate on voicing.
        {
            double af = (flow > 0.0) ? flow : 0.0;
            if (af > flowPeakEnv) flowPeakEnv = af;
            else                  flowPeakEnv *= 0.9997;
            flowNorm = (flowPeakEnv > 1e-6) ? (af / flowPeakEnv) : 0.0;
            if (flowNorm > 1.0) flowNorm = 1.0;
            // Slow running mean (~20 ms) so getFlowNormAC() is centered on 0.
            // Modulating frication by the AC part adds pitch-rate "buzz" while
            // leaving the average hiss energy (and thus loudness) unchanged.
            const double meanAlpha = 1.0 - exp(-1.0 / (sampleRate * 0.020));
            flowNormMean += (flowNorm - flowNormMean) * meanAlpha;
        }

        double dFlow = flow - lastFlow;
        lastFlow = flow;

        // ------------------------------------------------------------
        // Radiation Characteristic (Additive):
        // ------------------------------------------------------------
        // Real lip radiation adds +6 dB/oct to the source — it doesn't
        // subtract low frequencies.  The old crossfade replaced flow
        // energy with derivative energy, making brightness and warmth
        // a zero-sum game.  Additive mode keeps ALL the flow (warmth)
        // and layers derivative energy (presence) on top.
        //
        // radiationMix now controls how much derivative is ADDED:
        //   0.0  = pure flow (-12 dB/oct, very dark)
        //   0.3  = gentle presence (natural conversational speech)
        //   0.5  = clear presence (broadcast speech)
        //   1.0  = full derivative added (very bright, still warm)
        // The limiter catches any peaks from the summed signal.

        double srcDeriv = dFlow * radiationDerivGain;

        // Soft-limit the derivative to tame glottal closure transients.
        // Steady-state harmonics (small dFlow) pass through linearly — they
        // carry the +6 dB/oct spectral tilt we want for presence.
        // Closure spikes (large dFlow) get squashed by tanh — prevents
        // additive radiation from amplifying glottal sharpness.
        const double kDerivSaturation = 0.6;
        srcDeriv = kDerivSaturation * tanh(srcDeriv / kDerivSaturation);

        // Energy compensation: adding derivative increases total energy.
        // Scale down gently so negative tilt brightens without pumping volume.
        // At mix=0.15: divisor=1.075 (barely audible).
        // At mix=1.0:  divisor=1.5 (keeps full-bright from clipping).
        double voicedSrc = (flow + radiationMix * srcDeriv) / (1.0 + radiationMix * 0.5);

        // Voiced-only pre-emphasis
        double pre = voicedSrc - (voicedPreEmphA * lastVoicedSrc);
        lastVoicedSrc = voicedSrc;
        voicedSrc = (1.0 - voicedPreEmphMix) * voicedSrc + voicedPreEmphMix * pre;

        // Klatt TL (Bipolar)
        voicedSrc = applyTilt(voicedSrc);

        // Breathiness adds extra turbulence during the open phase.
        double voiceTurbAmp = frame->voiceTurbulenceAmplitude;
        if (!std::isfinite(voiceTurbAmp)) voiceTurbAmp = 0.0;
        voiceTurbAmp = clampDouble(voiceTurbAmp, 0.0, 1.0);
        if (breathiness > 0.0) {
            // Moderate turbulence increase (glottal-gated noise is the key breathy component)
            // Reduced from 1.0 to 0.5 - we want "weak airy voice" not "noise drowning voice"
            voiceTurbAmp = clampDouble(voiceTurbAmp + (0.5 * breathiness), 0.0, 1.0);
        }

        double turbulence = aspiration * voiceTurbAmp;
        if(glottisOpen) {
            double flow01 = flow / flowScale;
            if(flow01 < 0.0) flow01 = 0.0;
            if(flow01 > 1.0) flow01 = 1.0;
            turbulence *= pow(flow01, kTurbulenceFlowPower);
        } else {
            turbulence = 0.0;
        }

        // Voice amplitude with optional shimmer/creakiness/breathiness scaling.
        double voiceAmp = frame->voiceAmplitude;
        if (!std::isfinite(voiceAmp)) voiceAmp = 0.0;
        voiceAmp = clampDouble(voiceAmp, 0.0, 1.0);
        if (creakiness > 0.0) {
            voiceAmp *= (1.0 - (0.35 * creakiness));
        }
        if (breathiness > 0.0) {
            // TRUE breathy voice: the voiced component nearly disappears.
            // At full breathiness, only 15% of voice remains (85% reduction).
            // This makes turbulent noise the PRIMARY sound, not an additive layer.
            voiceAmp *= (1.0 - (0.98 * breathiness));
        }
        voiceAmp *= shimmerMul;

        // Tremor amplitude modulation - subtle, let pitch and OQ do the heavy lifting
        // The "shake" should come from voice quality changes, not volume pumping
        if (tremorDepthSmooth > 0.001) {
            // Reduced: ±25% amplitude at full depth (was ±60%)
            double ampIrregularity = 1.0 + (jitterShimmerRng.nextBipolar()) * 0.1 * tremorDepthSmooth;
            double tremorAmpMod = 1.0 + (tremorDepthSmooth * 0.5 * lastTremorSin * ampIrregularity);
            voiceAmp *= tremorAmpMod;
        }

        // CRITICAL: Apply voiceAmp ONLY to the voiced pulse, NOT to turbulence.
        // For breathiness: voice gets quiet while turbulence stays strong.
        // OLD: (voicedSrc + turbulence) * voiceAmp  <-- killed turbulence too!
        // NEW: (voicedSrc * voiceAmp) + turbulence  <-- turbulence independent
        double voicedIn = (voicedSrc * voiceAmp) + turbulence;
        const double dcPole = 0.9995;
        double voiced = voicedIn - lastVoicedIn + (dcPole * lastVoicedOut);
        lastVoicedIn = voicedIn;
        lastVoicedOut = voiced;

        // Anti-alias lowpass on voiced signal: attenuates harmonics near Nyquist
        // that would cause BLT warping artifacts in the resonator bank.
        // Applied after DC block, before combining with aspiration (noise doesn't alias).
        if (voicedAntiAliasActive) {
            voiced = voicedAntiAliasLp2.process(voicedAntiAliasLp1.process(voiced));
        }

        // Smooth aspirationAmplitude (fast attack, slower release) to avoid clicks.
        double targetAspAmp = frame->aspirationAmplitude;
        if (!std::isfinite(targetAspAmp)) targetAspAmp = 0.0;
        if (targetAspAmp < 0.0) targetAspAmp = 0.0;
        if (targetAspAmp > 1.0) targetAspAmp = 1.0;

        if (breathiness > 0.0) {
            targetAspAmp = clampDouble(targetAspAmp + (1.0 * breathiness), 0.0, 1.0);
        }

        if (!smoothAspAmpInit) {
            smoothAspAmp = targetAspAmp;
            smoothAspAmpInit = true;
        } else {
            const double coeff = (targetAspAmp > smoothAspAmp) ? aspAttackCoeff : aspReleaseCoeff;
            smoothAspAmp += (targetAspAmp - smoothAspAmp) * coeff;
        }

        double aspOut = aspiration * smoothAspAmp;
        lastAspOut = aspOut;
        return aspOut + voiced;
    }

    double getLastAspOut() const { return lastAspOut; }
    double getNoiseAmplitudeScale() const { return noiseAmplitudeScale; }
};

#endif // TGSPEECHBOX_VOICEGENERATOR_H