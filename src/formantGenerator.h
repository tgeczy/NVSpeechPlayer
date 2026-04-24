/*
TGSpeechBox — Cascade and parallel formant filter topologies.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSPEECHBOX_FORMANTGENERATOR_H
#define TGSPEECHBOX_FORMANTGENERATOR_H

#include "dspCommon.h"
#include "resonator.h"
#include "frame.h"
#include "utils.h"

class CascadeFormantGenerator { 
private:
    int sampleRate;
    PitchSyncResonator r1;  // F1 gets pitch-sync treatment
    Resonator r2, r3, r4, r5, r6, r7, r8, rN0, rNP;
    
    // Pitch-sync params from voicingTone
    double pitchSyncF1Delta;
    double pitchSyncB1Delta;
    double bwScale;  // Global cascade bandwidth multiplier from voicingTone
    double f1BwOffset;  // Source-tract coupling: extra F1 bandwidth (Hz)
    double nasalBwScale;   // Nasal resonator bandwidth multiplier
    double f4FreqScale;    // F4 frequency multiplier (pharynx length)
    double nasalGainScale; // Nasal pole coupling amplitude multiplier

public:
    CascadeFormantGenerator(int sr): sampleRate(sr), r1(sr), r2(sr), r3(sr), r4(sr), r5(sr), r6(sr), r7(sr), r8(sr), rN0(sr,true), rNP(sr),
        pitchSyncF1Delta(0.0), pitchSyncB1Delta(0.0), bwScale(1.0), f1BwOffset(0.0),
        nasalBwScale(1.0), f4FreqScale(1.0), nasalGainScale(1.0) {}

    void reset() {
        r1.reset(); r2.reset(); r3.reset(); r4.reset(); r5.reset(); r6.reset(); r7.reset(); r8.reset(); rN0.reset(); rNP.reset();
    }

    void decay(double factor) {
        r1.decay(factor); r2.decay(factor); r3.decay(factor);
        r4.decay(factor); r5.decay(factor); r6.decay(factor);
        r7.decay(factor); r8.decay(factor);
        rN0.decay(factor); rNP.decay(factor);
    }

    void setPitchSyncParams(double f1DeltaHz, double b1DeltaHz) {
        pitchSyncF1Delta = f1DeltaHz;
        pitchSyncB1Delta = b1DeltaHz;
        r1.setPitchSyncParams(f1DeltaHz, b1DeltaHz);
    }

void setCascadeBwScale(double scale) {
        // Clamp to safe range: too narrow risks instability, too wide loses vowel identity
        if (scale < 0.3) scale = 0.3;
        if (scale > 2.0) scale = 2.0;
        bwScale = scale;
    }

    void setNasalBwScale(double scale) {
        if (scale < 0.5) scale = 0.5;
        if (scale > 2.0) scale = 2.0;
        nasalBwScale = scale;
    }

    void setF4FreqScale(double scale) {
        if (scale < 0.7) scale = 0.7;
        if (scale > 1.5) scale = 1.5;
        f4FreqScale = scale;
    }

    void setNasalGainScale(double scale) {
        if (scale < 0.5) scale = 0.5;
        if (scale > 1.5) scale = 1.5;
        nasalGainScale = scale;
    }

    void setF1BwOffset(double offset) {
        f1BwOffset = offset;
    }

    double getNext(const speechPlayer_frame_t* frame, const speechPlayer_frameEx_t* frameEx, bool glottisOpen, double input) {
        input/=2.0;
        // Klatt cascade: N0 (antiresonator) -> NP (resonator), then cascade formants.
        // NOTE: Our phoneme tables were tuned with the classic high-to-low cascade order
        // (F6 -> F1). Even though Klatt 1980 notes some flexibility, changing the order
        // can audibly affect transitions (and can introduce clicks). So we preserve it.

        // Simple nasal fade: caNP crossfades between direct path and the NZ/NP path.
        // nasalBwScale widens/narrows nasal resonator bandwidths (female = wider).
        // nasalGainScale adjusts coupling amplitude (how much nasal bleeds through).
        const double n0Output = rN0.resonate(input, frame->cfN0, frame->cbN0 * nasalBwScale);

        double scaledCaNP = frame->caNP * nasalGainScale;
        if (scaledCaNP > 1.0) scaledCaNP = 1.0;
        double output = calculateValueAtFadePosition(
            input,
            rNP.resonate(n0Output, frame->cfNP, frame->cbNP * nasalBwScale),
            scaledCaNP
        );

        // During within-phoneme formant sweeps (diphthongs), widen bandwidth as needed
        // to keep resonators from becoming ultra-high-Q as formants move upward.
        double cb1 = frame->cb1;
        double cb2 = frame->cb2;
        double cb3 = frame->cb3;
        if (frameEx) {
            if (std::isfinite(frameEx->endCf1)) cb1 = bandwidthForSweep(frame->cf1, cb1, kSweepQMaxF1, kSweepBwMinF1, kSweepBwMax);
            if (std::isfinite(frameEx->endCf2)) cb2 = bandwidthForSweep(frame->cf2, cb2, kSweepQMaxF2, kSweepBwMinF2, kSweepBwMax);
            if (std::isfinite(frameEx->endCf3)) cb3 = bandwidthForSweep(frame->cf3, cb3, kSweepQMaxF3, kSweepBwMinF3, kSweepBwMax);
        }

        // Source-tract coupling: widen F1 bandwidth when F1 is low.
        cb1 += f1BwOffset;

        // --- Per-formant cascade bandwidth scaling ---
        // F1 gets a tighter scale to sharpen its peak and reduce the nasal-murmur
        // dominance that the series cascade architecture creates.
        // Upper formants (F2-F6) use the global scale so they don't ring.
        const double cascadeBwScale = bwScale;
        const double f1BwScale = cascadeBwScale * 0.75;  // tighter F1 — sharper vowel identity
        const double f2BwScale = cascadeBwScale * 0.88;  // tighter F2 — deepens F1-F2 valley (anti-nasal)
        cb1 *= f1BwScale;
        cb2 *= f2BwScale;
        cb3 *= cascadeBwScale;
        double cb4 = frame->cb4 * cascadeBwScale;
        double cb5 = frame->cb5 * cascadeBwScale;
        double cb6 = frame->cb6 * cascadeBwScale;
        // --- Nyquist-proximity fade for upper cascade formants ---
        // At low sample rates (e.g. 11025 Hz, Nyquist = 5512 Hz), the cascade
        // resonators for F5/F6 sit close to Nyquist and amplify harmonic energy
        // by 12-21 dB at the folding frequency.  Because voiced sounds are
        // periodic, this aliased energy creates audible beating ("swirly" /
        // "cell phone" artifacts).
        //
        // Critically, this is ONLY applied to the CASCADE path (voiced sounds).
        // The PARALLEL path (fricatives) is left untouched because fricative
        // noise is aperiodic — aliased noise is still noise, with no beating.
        // This is why DECTalk sounds clean at 11025: its cascade has only 5
        // formants (no F6), and its parallel branch has independent gains.
        //
        // Fade: ratio = cf/nyquist.  <0.65 → full, >0.85 → bypass, linear between.
        // At 22050+ Hz all fades are 1.0 → zero cost / unchanged behaviour.
        const double nyquist = 0.5 * (double)sampleRate;
        auto cascadeFade = [&](double cf) -> double {
            if (cf <= 0.0 || !std::isfinite(cf)) return 1.0;
            double ratio = cf / nyquist;
            if (ratio < 0.65) return 1.0;
            if (ratio > 0.85) return 0.0;
            return 1.0 - (ratio - 0.65) / 0.20;
        };

        // Graduated head-size scaling: F4-F6 get full scale, F3 mostly,
        // F2 moderate, F1 minimal.  Models vocal tract length uniformly
        // without destroying vowel identity carried by F1/F2.
        const double hs1 = (f4FreqScale != 1.0) ? pow(f4FreqScale, 0.2) : 1.0;
        const double hs2 = (f4FreqScale != 1.0) ? pow(f4FreqScale, 0.5) : 1.0;
        const double hs3 = (f4FreqScale != 1.0) ? pow(f4FreqScale, 0.8) : 1.0;

        // Higher cascade formants F7/F8 (DSP v8).
        // Fills the spectral gap above F6 at sample rates >= 22050 Hz.
        // Defaults from Rabiner 1968, as cited in QLatt
        // (https://github.com/nicclase/qlatt, notes-formant-research.md).
        // At low SRs the Nyquist fade naturally bypasses them.
        {
            const double defaultCf7 = 6500.0, defaultCb7 = 720.0;
            const double defaultCf8 = 7500.0, defaultCb8 = 1250.0;
            double fcf8 = frameEx ? frameEx->cf8 : defaultCf8;
            double fcb8 = frameEx ? frameEx->cb8 : defaultCb8;
            double fcf7 = frameEx ? frameEx->cf7 : defaultCf7;
            double fcb7 = frameEx ? frameEx->cb7 : defaultCb7;
            if (!std::isfinite(fcf8) || fcf8 <= 0.0) fcf8 = defaultCf8;
            if (!std::isfinite(fcb8) || fcb8 <= 0.0) fcb8 = defaultCb8;
            if (!std::isfinite(fcf7) || fcf7 <= 0.0) fcf7 = defaultCf7;
            if (!std::isfinite(fcb7) || fcb7 <= 0.0) fcb7 = defaultCb7;

            // Full head-size scaling (same tier as F4-F6)
            fcf8 *= f4FreqScale;
            fcf7 *= f4FreqScale;

            double preR8 = output;
            output = r8.resonate(output, fcf8, fcb8 * cascadeBwScale);
            double fade8 = cascadeFade(fcf8);
            output = preR8 + fade8 * (output - preR8);

            double preR7 = output;
            output = r7.resonate(output, fcf7, fcb7 * cascadeBwScale);
            double fade7 = cascadeFade(fcf7);
            output = preR7 + fade7 * (output - preR7);
        }

        double preR6 = output;
        output = r6.resonate(output, frame->cf6 * f4FreqScale, cb6);
        double fade6 = cascadeFade(frame->cf6 * f4FreqScale);
        output = preR6 + fade6 * (output - preR6);

        double preR5 = output;
        output = r5.resonate(output, frame->cf5 * f4FreqScale, cb5);
        double fade5 = cascadeFade(frame->cf5 * f4FreqScale);
        output = preR5 + fade5 * (output - preR5);

        double preR4 = output;
        output = r4.resonate(output, frame->cf4 * f4FreqScale, cb4);
        double fade4 = cascadeFade(frame->cf4 * f4FreqScale);
        output = preR4 + fade4 * (output - preR4);
        output = r3.resonate(output, frame->cf3 * hs3, cb3);
        output = r2.resonate(output, frame->cf2 * hs2, cb2);
        // F1 uses pitch-synchronous resonator
        output = r1.resonate(output, frame->cf1 * hs1, cb1, glottisOpen);
        return output;
    }
};

class ParallelFormantGenerator {
private:
    int sampleRate;
    Resonator r1, r2, r3, r4, r5, r6, r7, r8;
    double f4FreqScale;

public:
    ParallelFormantGenerator(int sr): sampleRate(sr), r1(sr), r2(sr), r3(sr), r4(sr), r5(sr), r6(sr), r7(sr), r8(sr), f4FreqScale(1.0) {}

    void setF4FreqScale(double scale) {
        if (scale < 0.7) scale = 0.7;
        if (scale > 1.5) scale = 1.5;
        f4FreqScale = scale;
    }

    void reset() {
        r1.reset(); r2.reset(); r3.reset(); r4.reset(); r5.reset(); r6.reset(); r7.reset(); r8.reset();
    }

    void decay(double factor) {
        r1.decay(factor); r2.decay(factor); r3.decay(factor);
        r4.decay(factor); r5.decay(factor); r6.decay(factor);
        r7.decay(factor); r8.decay(factor);
    }

    double getNext(const speechPlayer_frame_t* frame, const speechPlayer_frameEx_t* frameEx, bool glottisOpen, double input) {
        input/=2.0;
        (void)glottisOpen;
        double output=0;

        // Same Q-capping logic for parallel formants when their frequencies are swept.
        double pb1 = frame->pb1;
        double pb2 = frame->pb2;
        double pb3 = frame->pb3;
        if (frameEx) {
            if (std::isfinite(frameEx->endPf1)) pb1 = bandwidthForSweep(frame->pf1, pb1, kSweepQMaxF1, kSweepBwMinF1, kSweepBwMax);
            if (std::isfinite(frameEx->endPf2)) pb2 = bandwidthForSweep(frame->pf2, pb2, kSweepQMaxF2, kSweepBwMinF2, kSweepBwMax);
            if (std::isfinite(frameEx->endPf3)) pb3 = bandwidthForSweep(frame->pf3, pb3, kSweepQMaxF3, kSweepBwMinF3, kSweepBwMax);
        }

        // Graduated head-size scaling (same as cascade path)
        const double hs1 = (f4FreqScale != 1.0) ? pow(f4FreqScale, 0.2) : 1.0;
        const double hs2 = (f4FreqScale != 1.0) ? pow(f4FreqScale, 0.5) : 1.0;
        const double hs3 = (f4FreqScale != 1.0) ? pow(f4FreqScale, 0.8) : 1.0;

        // DSP v9: frication spectral tilt.
        // Scales each parallel amplitude by a frequency-dependent factor when
        // frameEx->fricationTiltDb is non-zero. Pivot 1500 Hz, slope 3000 Hz
        // per dB unit. Only attenuates above pivot (asymmetric rolloff) — pa1
        // and pa2 are untouched, pa3 barely touched, pa4/pa5/pa6 progressively
        // darkened. Preserves the velar F3 signature while removing the
        // high-frequency tap-click that causes /ɡ/→/ɾ/ misperception at fast
        // rates (issue #95 Bug 1, pegue/lago rate 1.3+).
        const double tilt = (frameEx) ? frameEx->fricationTiltDb : 0.0;
        auto tiltScale = [tilt](double fHz) -> double {
            if (tilt == 0.0) return 1.0;
            constexpr double pivotHz = 1500.0, slopeHz = 3000.0;
            const double excess = fHz - pivotHz;
            if (excess <= 0.0) return 1.0;
            return std::pow(10.0, tilt * excess / (20.0 * slopeHz));
        };

        const double pf1e = frame->pf1 * hs1;
        const double pf2e = frame->pf2 * hs2;
        const double pf3e = frame->pf3 * hs3;
        const double pf4e = frame->pf4 * f4FreqScale;
        const double pf5e = frame->pf5 * f4FreqScale;
        const double pf6e = frame->pf6 * f4FreqScale;

        output+=(r1.resonate(input,pf1e,pb1)-input)*frame->pa1*tiltScale(pf1e);
        output+=(r2.resonate(input,pf2e,pb2)-input)*frame->pa2*tiltScale(pf2e);
        output+=(r3.resonate(input,pf3e,pb3)-input)*frame->pa3*tiltScale(pf3e);
        output+=(r4.resonate(input,pf4e,frame->pb4)-input)*frame->pa4*tiltScale(pf4e);
        output+=(r5.resonate(input,pf5e,frame->pb5)-input)*frame->pa5*tiltScale(pf5e);
        output+=(r6.resonate(input,pf6e,frame->pb6)-input)*frame->pa6*tiltScale(pf6e);

        // Higher parallel formants F7/F8 (DSP v8).
        // Extend fricative spectral envelope above F6 for "presence" and "air."
        // Frequencies/bandwidths shared with cascade (frameEx cf7/cb7/cf8/cb8).
        // Fixed amplitude rolloff — frication above 6.5 kHz isn't phonemically
        // contrastive, so per-phoneme control is unnecessary.  When input is
        // zero (vowels), these produce zero output regardless of amplitude.
        // Rolloff from Rabiner 1968 ndbScale progression (QLatt extrapolation).
        if (frameEx) {
            constexpr double kParF7Amp = 0.15;
            constexpr double kParF8Amp = 0.07;
            constexpr double defaultCf7 = 6500.0, defaultCb7 = 720.0;
            constexpr double defaultCf8 = 7500.0, defaultCb8 = 1250.0;
            double pf7 = frameEx->cf7, pb7v = frameEx->cb7;
            double pf8 = frameEx->cf8, pb8v = frameEx->cb8;
            if (!std::isfinite(pf7) || pf7 <= 0.0) pf7 = defaultCf7;
            if (!std::isfinite(pb7v) || pb7v <= 0.0) pb7v = defaultCb7;
            if (!std::isfinite(pf8) || pf8 <= 0.0) pf8 = defaultCf8;
            if (!std::isfinite(pb8v) || pb8v <= 0.0) pb8v = defaultCb8;
            pf7 *= f4FreqScale;
            pf8 *= f4FreqScale;
            // Nyquist fade for parallel F7/F8: at low SRs these sit near
            // the folding frequency, and even aperiodic noise aliases badly
            // when a resonator peaks right at Nyquist.
            const double nyq = 0.5 * (double)sampleRate;
            auto parFade = [&](double cf) -> double {
                double ratio = cf / nyq;
                if (ratio < 0.65) return 1.0;
                if (ratio > 0.85) return 0.0;
                return 1.0 - (ratio - 0.65) / 0.20;
            };
            double fade7 = parFade(pf7);
            double fade8 = parFade(pf8);
            if (fade7 > 0.0) output += (r7.resonate(input, pf7, pb7v) - input) * kParF7Amp * fade7;
            if (fade8 > 0.0) output += (r8.resonate(input, pf8, pb8v) - input) * kParF8Amp * fade8;
        }

        return calculateValueAtFadePosition(output,input,frame->parallelBypass);
    }
};

#endif // TGSPEECHBOX_FORMANTGENERATOR_H
