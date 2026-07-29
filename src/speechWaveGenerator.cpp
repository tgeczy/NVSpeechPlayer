/*
TGSpeechBox — Main speech wave generator (orchestrator).
Copyright 2014 NV Access Limited.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include "debug.h"
#include "utils.h"
#include "speechWaveGenerator.h"

// Internal DSP modules (header-only, no Makefile changes needed)
#include "dspCommon.h"
#include "pitchModel.h"
#include "voiceGenerator.h"
#include "resonator.h"
#include "formantGenerator.h"

// ============================================================================
// Source-tract coupling strength
// ============================================================================
// Modulates glottal source spectral tilt as a function of F1 frequency.
// Low F1 (high vowels /i/, /u/) = heavier acoustic loading = steeper tilt.
// High F1 (open vowels /a/)     = lighter loading          = flatter spectrum.
// Also widens F1 bandwidth slightly at low F1 (subglottal coupling loss).
//
// 0.0 = off (current behavior, no coupling)
// 1.0 = full effect
const double kSourceTractCoupling = 0.55;

// ============================================================================
// Glottal-cycle F1 bandwidth modulation (Hz)
// ============================================================================
// Widens F1 bandwidth by this amount during the glottal open phase,
// snaps back during closed phase (with 2ms smoothing to avoid clicks).
// Models increased acoustic losses through the open glottis.
// Stacks with voicingTone.pitchSyncB1DeltaHz if that is also non-zero.
//
// 0.0  = off
// 30-50 = audible but natural range
const double kGlottalCycleBwDelta = 40.0;

// ============================================================================
// Subglottal coupling (Sg1 pole-zero pair)
// ============================================================================
// Models the first subglottal (tracheal) resonance coupling through the
// glottis into the vocal tract.  Creates a pole-zero pair near 600 Hz that
// adds "chest resonance" character — most audible on open vowels where F1
// is near Sg1.  Gated by glottal phase: strong coupling when open, weak
// when closed (glottis never perfectly seals).
//
// References: Lulich et al. 2012, Hanna et al. 2018, Klatt & Klatt 1990.
// Signal chain: Source → STC tilt → **Sg1 pole-zero** → Cascade (N0/NP → F6…F1)
const double kSubglottalCoupling    = 0.3;    // 0=off, 1=full
const double kSg1Freq               = 630.0;  // Hz, pole (resonance)
const double kSg1Bw                 = 70.0;   // Hz, pole bandwidth
const double kSgZ1Freq              = 590.0;  // Hz, zero (anti-resonance), 40 Hz below pole
const double kSgZ1Bw                = 90.0;   // Hz, zero bandwidth (wider = gentler dip)
const double kSgClosedPhaseLeak     = 0.20;   // coupling fraction during closed phase

class SpeechWaveGeneratorImpl: public SpeechWaveGenerator {
private:
    int sampleRate;
    VoiceGenerator voiceGenerator;
    NoiseGenerator fricGenerator;
    CascadeFormantGenerator cascade;
    ParallelFormantGenerator parallel;
    FrameManager* frameManager;
    double lastInput;
    double lastOutput;
    bool wasSilence;

    double smoothPreGain;
    double preGainAttackAlpha;
    double preGainReleaseAlpha;

    // Smooth frication amplitude to avoid sharp edges at fricative→vowel boundaries
    double smoothFricAmp;
    double fricAttackAlpha;
    double fricReleaseAlpha;

    // High-shelf EQ state for brightness
    double hsIn1, hsIn2, hsOut1, hsOut2;
    double hsB0, hsB1, hsB2, hsA1, hsA2;

    // Current voicing tone parameters (for high-shelf recalculation)
    speechPlayer_voicingTone_t currentTone;

    // ------------------------------------------------------------
    // Adaptive frication filtering (burst vs sustained)
    // ------------------------------------------------------------
    // Uses two parallel lowpass paths: one for burst (lower cutoff),
    // one for sustained frication (higher cutoff). We crossfade based
    // on "burstiness" (rate of change of fricationAmplitude).
    OnePoleLowpass fricBurstLp1, fricBurstLp2;
    OnePoleLowpass fricSustainLp1, fricSustainLp2;
    double lastTargetFricAmp;  // for transient (burst) detection - uses RAW target, not smoothed
    double lastTargetAspAmp;   // for aspiration burst detection (stops often use asp, not fric)
    double fricBurstFc;        // sample-rate-aware burst cutoff
    double fricSustainFc;      // sample-rate-aware sustain cutoff
    double burstEnv;           // 0..1, holds burstiness for a few ms
    double burstEnvDecayMul;   // per-sample decay multiplier

    // Aspiration lowpass: filters the aspiration noise that goes through the cascade
    // This is often the real culprit for "sharp" stop releases, not the frication path
    OnePoleLowpass aspLp1, aspLp2;
    double aspBurstFc;         // sample-rate-aware aspiration burst cutoff

    // Shelf ducking: reduce high-shelf boost during bursts to tame stop sharpness
    // without affecting voiced brightness baseline
    double shelfMix;           // 0..1, smoothed crossfade between unshelved and shelved
    double shelfMixAlpha;      // smoothing coefficient

    // Stop fade-out: when frames stop (interrupt), fade out over ~4ms to avoid click
    double lastBrightOut;      // store last post-shelf sample for fade tail
    int stopFadeRemaining;     // samples left in fade-out
    int stopFadeTotal;         // total fade-out length
    int startFadeRemaining;    // samples left in fade-in (prevents pop on speech start)
    int startFadeTotal;        // total fade-in length

    // Cascade duck smoother: prevents gain discontinuity at stop→vowel boundary.
    // The raw cascadeDuck can snap from 0.3→1.0 when va rises, causing a click.
    // Smoothing mirrors what shelfMix already does for the shelf duck.
    double smoothCascadeDuck;      // current smoothed duck value (1.0 = no duck)
    double cascadeDuckAlpha;       // per-sample smoothing coefficient

    // Lookahead peak limiter: catches amplitude spikes before they reach the
    // OS audio system. Lookahead lets the limiter see peaks BEFORE they arrive,
    // preventing pitch-synchronous pumping (issue #71). Hold time bridges
    // inter-pulse troughs so gain stays constant across pitch cycles.
    double limiterGain;        // current gain reduction (1.0 = no reduction)
    double limiterAttackAlpha; // per-sample attack coefficient
    double limiterReleaseAlpha;// per-sample release coefficient
    double limiterThreshold;   // signal level above which limiting kicks in
    int    limiterHoldCounter; // samples remaining in hold period
    int    limiterHoldSamples; // hold duration in samples (~5ms)
    static constexpr int kLimiterLookaheadMax = 220; // max ~10ms at 22050
    double limiterDelayBuf[kLimiterLookaheadMax];
    int    limiterDelayLen;    // actual lookahead length in samples
    int    limiterDelayIdx;    // write position in circular buffer

    // Source-tract coupling: one-pole tilt filter modulated by F1.
    double stcTiltState;       // IIR state for the coupling lowpass
    double stcSmoothPole;      // smoothed pole coefficient (avoids clicks on F1 jumps)
    double stcSmoothBwOff;     // smoothed F1 bandwidth offset (Hz)
    double stcAlpha;           // per-sample smoothing coefficient for pole/bw

    // Subglottal coupling: Sg1 pole-zero pair gated by glottal phase.
    Resonator sgPole;          // tracheal resonance (~630 Hz)
    Resonator sgZero;          // tracheal anti-resonance (~590 Hz)
    double sgCouplingSmooth;   // smoothed coupling strength (glottal-gated)
    double sgCouplingAlpha;    // per-sample smoothing coefficient

    // Output gain: platform-specific amplification applied before the limiter
    // so that clipping/limiting behavior is consistent across all platforms.
    // Default 1.0 (NVDA). Android=3.0, iOS=1.7.
    double outputGain;

    // Time-stretch: accelerated frame consumption for rate boost.
    // Instead of skipping output cycles (which creates discontinuities),
    // consume frames faster — advance the speech timeline by calling
    // getCurrentFrameWithEx() multiple times per output sample.
    // Every output sample is cleanly generated, no artifacts.
    double timeStretchFactor;  // 1.0 = off, 2.0 = 2x speedup
    double timeStretchAccum;   // fractional accumulator for sub-sample stepping

    void initHighShelf(double fc, double gainDb, double Q) {
        // Clamp inputs to prevent NaNs and weird filter behavior from bad UI values
        double nyq = 0.5 * (double)sampleRate;
        if (!std::isfinite(fc)) fc = 2000.0;
        if (!std::isfinite(gainDb)) gainDb = 0.0;
        if (!std::isfinite(Q)) Q = 0.7;
        if (fc < 20.0) fc = 20.0;
        if (fc > nyq * 0.95) fc = nyq * 0.95;
        if (Q < 0.1) Q = 0.1;
        if (Q > 4.0) Q = 4.0;
        if (gainDb < -24.0) gainDb = -24.0;
        if (gainDb > 24.0) gainDb = 24.0;

        double A = pow(10.0, gainDb / 40.0);
        double w0 = PITWO * fc / sampleRate;
        double cosw0 = cos(w0);
        double sinw0 = sin(w0);
        double alpha = sinw0 / (2.0 * Q);

        double a0 = (A+1) - (A-1)*cosw0 + 2*sqrt(A)*alpha;
        hsB0 = (A*((A+1) + (A-1)*cosw0 + 2*sqrt(A)*alpha)) / a0;
        hsB1 = (-2*A*((A-1) + (A+1)*cosw0)) / a0;
        hsB2 = (A*((A+1) + (A-1)*cosw0 - 2*sqrt(A)*alpha)) / a0;
        hsA1 = (2*((A-1) - (A+1)*cosw0)) / a0;
        hsA2 = ((A+1) - (A-1)*cosw0 - 2*sqrt(A)*alpha) / a0;
    }

    double applyHighShelf(double in) {
        double out = hsB0*in + hsB1*hsIn1 + hsB2*hsIn2 - hsA1*hsOut1 - hsA2*hsOut2;
        hsIn2 = hsIn1;
        hsIn1 = in;
        hsOut2 = hsOut1;
        hsOut1 = out;
        return out;
    }

public:
    SpeechWaveGeneratorImpl(int sr): sampleRate(sr), voiceGenerator(sr), fricGenerator(), cascade(sr), parallel(sr), frameManager(NULL), lastInput(0.0), lastOutput(0.0), wasSilence(true), smoothPreGain(0.0), preGainAttackAlpha(0.0), preGainReleaseAlpha(0.0), smoothFricAmp(0.0), fricAttackAlpha(0.0), fricReleaseAlpha(0.0), hsIn1(0), hsIn2(0), hsOut1(0), hsOut2(0), fricBurstLp1(sr), fricBurstLp2(sr), fricSustainLp1(sr), fricSustainLp2(sr), lastTargetFricAmp(0.0), lastTargetAspAmp(0.0), fricBurstFc(0.0), fricSustainFc(0.0), burstEnv(0.0), burstEnvDecayMul(1.0), aspLp1(sr), aspLp2(sr), aspBurstFc(0.0), shelfMix(1.0), shelfMixAlpha(0.0), lastBrightOut(0.0), stopFadeRemaining(0), stopFadeTotal(0), startFadeRemaining(0), startFadeTotal(0), limiterGain(1.0), limiterAttackAlpha(0.0), limiterReleaseAlpha(0.0), limiterThreshold(0.0), smoothCascadeDuck(1.0), cascadeDuckAlpha(0.0), stcTiltState(0.0), stcSmoothPole(0.0), stcSmoothBwOff(0.0), stcAlpha(0.0), sgPole(sr), sgZero(sr, true), sgCouplingSmooth(0.0), sgCouplingAlpha(0.0), outputGain(1.0), timeStretchFactor(1.0), timeStretchAccum(0.0) {
        const double attackMs = 1.0;
        const double releaseMs = 0.5;
        preGainAttackAlpha = 1.0 - exp(-1.0 / (sampleRate * (attackMs * 0.001)));
        preGainReleaseAlpha = 1.0 - exp(-1.0 / (sampleRate * (releaseMs * 0.001)));

        // Frication smoothing (ms)
        const double fricAttackMs = 0.8;
        const double fricReleaseMs = 1.2;
        fricAttackAlpha = 1.0 - exp(-1.0 / (sampleRate * (fricAttackMs * 0.001)));
        fricReleaseAlpha = 1.0 - exp(-1.0 / (sampleRate * (fricReleaseMs * 0.001)));

        // Hold burstiness for ~6 ms so the burst LP actually affects stop releases
        const double burstHoldMs = 6.0;
        burstEnvDecayMul = exp(-1.0 / (sampleRate * (burstHoldMs * 0.001)));

        // Smooth shelf mix changes to avoid clicks (fast-ish)
        const double shelfMixMs = 4.0;
        shelfMixAlpha = 1.0 - exp(-1.0 / (sampleRate * (shelfMixMs * 0.001)));

        // Lookahead peak limiter (issue #71): prevents pitch-synchronous
        // gain pumping that caused clicks at specific speech rates.
        // Lookahead: 5ms — the limiter sees peaks before they arrive,
        //   so gain reduction is symmetric around the peak (no asymmetric
        //   attack transient that deepens the post-peak trough).
        // Hold: 5ms — bridges inter-pulse troughs (zero crossings) so
        //   the limiter doesn't release between pitch pulses and re-attack
        //   on the next one. Covers one full pitch period at 200Hz.
        // Attack: 1ms — faster than old 2ms because lookahead removes the
        //   need for slow attack. 1ms catches burst transients within the
        //   lookahead window.
        // Release: 40ms — slightly faster than old 50ms because hold
        //   already prevents premature release.
        const double limiterAttackMs = 1.0;
        const double limiterReleaseMs = 40.0;
        const double limiterLookaheadMs = 5.0;
        const double limiterHoldMs = 5.0;
        limiterAttackAlpha = 1.0 - exp(-1.0 / (sampleRate * (limiterAttackMs * 0.001)));
        limiterReleaseAlpha = 1.0 - exp(-1.0 / (sampleRate * (limiterReleaseMs * 0.001)));
        limiterThreshold = 4.0;
        limiterHoldCounter = 0;
        limiterHoldSamples = (int)(limiterHoldMs * sampleRate * 0.001);
        limiterDelayLen = (int)(limiterLookaheadMs * sampleRate * 0.001);
        if (limiterDelayLen > kLimiterLookaheadMax) limiterDelayLen = kLimiterLookaheadMax;
        if (limiterDelayLen < 1) limiterDelayLen = 1;
        limiterDelayIdx = 0;
        memset(limiterDelayBuf, 0, sizeof(limiterDelayBuf));

        // Cascade duck smoother: ~3ms time constant.
        // Fast enough to engage during a burst (~6ms hold), slow enough
        // that the release back to 1.0 doesn't snap when va rises.
        const double cascadeDuckMs = 3.0;
        cascadeDuckAlpha = 1.0 - exp(-1.0 / (sampleRate * (cascadeDuckMs * 0.001)));

        // Source-tract coupling smoother: ~5ms time constant.
        // Prevents clicks when F1 changes abruptly between frames.
        const double stcSmoothMs = 5.0;
        stcAlpha = 1.0 - exp(-1.0 / (sampleRate * (stcSmoothMs * 0.001)));

        // Subglottal coupling smoother: ~1.5ms time constant.
        // Fast enough to track glottal cycles at typical F0 (~100-300 Hz,
        // period 3-10 ms), smooth enough to avoid clicks at phase edges.
        const double sgSmoothMs = 1.5;
        sgCouplingAlpha = 1.0 - exp(-1.0 / (sampleRate * (sgSmoothMs * 0.001)));

        // Initialize with default voicing tone
        currentTone = speechPlayer_getDefaultVoicingTone();

        // High shelf: use defaults from voicing tone
        initHighShelf(currentTone.highShelfFcHz, currentTone.highShelfGainDb, currentTone.highShelfQ);

        // Pitch-sync F1 modulation: include glottal-cycle BW delta
        cascade.setPitchSyncParams(currentTone.pitchSyncF1DeltaHz,
                                   currentTone.pitchSyncB1DeltaHz + kGlottalCycleBwDelta);

        // ------------------------------------------------------------
        // Adaptive frication lowpass: select cutoffs based on sample rate
        // ------------------------------------------------------------
        // Interpolate between known sample rates for smooth behavior
        if (sampleRate <= 11025) {
            fricBurstFc = kFricBurstFc_11k;
            fricSustainFc = kFricSustainFc_11k;
        } else if (sampleRate <= 16000) {
            // Interpolate between 11k and 16k
            double t = (double)(sampleRate - 11025) / (16000.0 - 11025.0);
            fricBurstFc = kFricBurstFc_11k + t * (kFricBurstFc_16k - kFricBurstFc_11k);
            fricSustainFc = kFricSustainFc_11k + t * (kFricSustainFc_16k - kFricSustainFc_11k);
        } else if (sampleRate <= 22050) {
            // Interpolate between 16k and 22k
            double t = (double)(sampleRate - 16000) / (22050.0 - 16000.0);
            fricBurstFc = kFricBurstFc_16k + t * (kFricBurstFc_22k - kFricBurstFc_16k);
            fricSustainFc = kFricSustainFc_16k + t * (kFricSustainFc_22k - kFricSustainFc_16k);
        } else if (sampleRate <= 44100) {
            // Interpolate between 22k and 44k
            double t = (double)(sampleRate - 22050) / (44100.0 - 22050.0);
            fricBurstFc = kFricBurstFc_22k + t * (kFricBurstFc_44k - kFricBurstFc_22k);
            fricSustainFc = kFricSustainFc_22k + t * (kFricSustainFc_44k - kFricSustainFc_22k);
        } else {
            // Above 44100: scale proportionally from 44k values
            double scale = (double)sampleRate / 44100.0;
            fricBurstFc = kFricBurstFc_44k * scale;
            fricSustainFc = kFricSustainFc_44k * scale;
        }

        // Set cutoff frequencies for the frication lowpass filters
        fricBurstLp1.setCutoffHz(fricBurstFc);
        fricBurstLp2.setCutoffHz(fricBurstFc);
        fricSustainLp1.setCutoffHz(fricSustainFc);
        fricSustainLp2.setCutoffHz(fricSustainFc);

        // ------------------------------------------------------------
        // Aspiration lowpass: this is often the real culprit for sharp stops
        // Use more aggressive cutoffs than frication since this is the "too sharp" path
        // Interpolate between known sample rates for smooth behavior
        // ------------------------------------------------------------
        if (sampleRate <= 11025) {
            aspBurstFc = kAspBurstFc_11k;
        } else if (sampleRate <= 16000) {
            double t = (double)(sampleRate - 11025) / (16000.0 - 11025.0);
            aspBurstFc = kAspBurstFc_11k + t * (kAspBurstFc_16k - kAspBurstFc_11k);
        } else if (sampleRate <= 22050) {
            double t = (double)(sampleRate - 16000) / (22050.0 - 16000.0);
            aspBurstFc = kAspBurstFc_16k + t * (kAspBurstFc_22k - kAspBurstFc_16k);
        } else if (sampleRate <= 44100) {
            double t = (double)(sampleRate - 22050) / (44100.0 - 22050.0);
            aspBurstFc = kAspBurstFc_22k + t * (kAspBurstFc_44k - kAspBurstFc_22k);
        } else {
            double scale = (double)sampleRate / 44100.0;
            aspBurstFc = kAspBurstFc_44k * scale;
        }
        aspLp1.setCutoffHz(aspBurstFc);
        aspLp2.setCutoffHz(aspBurstFc);
    }

    unsigned int generate(const unsigned int sampleCount, sample* sampleBuf) {
        return generateIndexAware(sampleCount, sampleBuf, NULL);
    }

    unsigned int generateIndexAware(const unsigned int sampleCount, sample* sampleBuf, int* indexReachedOut) {
        if(indexReachedOut) *indexReachedOut=-1;
        if(!frameManager) return 0;
        // Snapshot the user index at entry. lastIndex flips the moment a
        // marker frame (userIndex != -1) starts generating, so comparing
        // against this snapshot detects every marker crossed in this call.
        const int entryUserIndex=frameManager->getLastIndex();

        // Check if a purge happened — if so, trigger a fade-in to prevent pop
        // Note: We intentionally do NOT reset resonators here because that can cause
        // its own transient. The fade-in should mask any discontinuity.
        if(frameManager->checkAndClearPurgeFlag()) {
            // Only reset stateless things and trigger fade
            hsIn1 = 0.0; hsIn2 = 0.0; hsOut1 = 0.0; hsOut2 = 0.0;
            lastInput = 0.0;
            lastOutput = 0.0;
            // Trigger fade-in
            startFadeTotal = (int)(sampleRate * 0.004); // 6 ms - longer for higher sample rates
            if (startFadeTotal < 64) startFadeTotal = 64;
            startFadeRemaining = startFadeTotal;
            // Force full reset on next frame (clears Fujisaki IIR filter state)
            wasSilence = true;
        }
        
        // Index-aware early stop (speechPlayer_synthesize2): when a marker
        // frame (userIndex != -1) starts generating, finish emitting the
        // current sample and then stop, so the returned chunk ends at the
        // marker (its first, silent sample included). The check runs AFTER
        // each sample is produced — never between the frame-clock tick and
        // its sample — so concatenated synthesize2 output stays bit-identical
        // to an unsplit synthesize run: no tick is ever consumed without its
        // sample, and all DSP state evolves identically. The notification
        // lands one sample (0.045 ms) past the marker start — inaudible.
        // If time-stretch crosses several markers within one sample's tick
        // burst, only the newest index is reported; NVDA treats a reached
        // index as implying all earlier ones.
        for(unsigned int i=0;i<sampleCount;++i) {
            const speechPlayer_frameEx_t* frameEx = NULL;
            const speechPlayer_frame_t* frame=frameManager->getCurrentFrameWithEx(&frameEx);
            if(frame) {
                if(wasSilence) {
                    voiceGenerator.reset();
                    fricGenerator.reset();
                    cascade.reset();
                    parallel.reset();
                    lastInput=0.0;
                    lastOutput=0.0;
                    smoothPreGain=0.0;
                    smoothFricAmp=0.0;
                    // Reset adaptive frication/aspiration burst state on silence boundaries
                    lastTargetFricAmp=0.0;
                    lastTargetAspAmp=0.0;
                    burstEnv=0.0;
                    fricBurstLp1.reset(); fricBurstLp2.reset();
                    fricSustainLp1.reset(); fricSustainLp2.reset();
                    aspLp1.reset(); aspLp2.reset();
                    shelfMix = 1.0;
                    smoothCascadeDuck = 1.0;  // Reset duck (no ducking)
                    limiterGain = 1.0;  // Reset limiter (no gain reduction)
                    limiterHoldCounter = 0;
                    limiterDelayIdx = 0;
                    memset(limiterDelayBuf, 0, sizeof(limiterDelayBuf));
                    stcTiltState = 0.0;  // Reset source-tract coupling filter
                    stcSmoothPole = 0.0;
                    stcSmoothBwOff = 0.0;
                    sgPole.reset();     // Reset subglottal resonators
                    sgZero.reset();
                    sgCouplingSmooth = 0.0;
                    // Reset stop fade-out state
                    stopFadeTotal = 0;
                    stopFadeRemaining = 0;
                    // Reset high-shelf filter state to prevent pops from residual energy
                    hsIn1 = 0.0; hsIn2 = 0.0; hsOut1 = 0.0; hsOut2 = 0.0;
                    // Start a fade-in to prevent pops on speech start.
                    // 5ms gives cascade resonators time to reach steady state
                    // before excitation is fully ramped — at 730 Hz (typical F1),
                    // 2ms barely covers one oscillation cycle.
                    startFadeTotal = (int)(sampleRate * 0.005); // 5 ms
                    if (startFadeTotal < 16) startFadeTotal = 16;
                    startFadeRemaining = startFadeTotal;
                    wasSilence=false;
                }

                double targetPreGain = frame->preFormantGain;
                double prevSmooth = smoothPreGain;
                double alpha = (targetPreGain > smoothPreGain) ? preGainAttackAlpha : preGainReleaseAlpha;
                smoothPreGain += (targetPreGain - smoothPreGain) * alpha;

                // Drain resonator energy during silence.  When preFormantGain
                // is near zero (closure, word-boundary gap), the source is off
                // but IIR resonators still ring.  Real vocal tracts don't ring
                // through a closed glottis, so we decay the state each sample.
                // 0.95 ≈ 1ms time-constant at 22050 Hz — fast enough to drain
                // before the next phoneme, gentle enough to avoid clicks.
                if (smoothPreGain < 0.01) {
                    cascade.decay(0.95);
                    parallel.decay(0.95);
                    // The output DC blocker (0.9995 pole, tau~91ms) holds its
                    // last state through silence the same way — drain it too,
                    // or it ring-slides for hundreds of ms after the utterance.
                    lastInput *= 0.9;
                    lastOutput *= 0.9;
                }

                // If preFormantGain was near zero and is now rising, aggressively
                // damp any remaining resonator state so the previous phoneme's
                // formants don't color the new one.  A hard reset (zero state)
                // causes impulse-response overshoot — the cascade resonators
                // ring up from mathematical zero and overshoot steady-state by
                // 2-3x before settling.  A fast decay (0.3) cuts residual energy
                // to ~9% without creating the zero-state transient.
                if (prevSmooth < 0.005 && targetPreGain > 0.01) {
                    cascade.decay(0.3);
                    parallel.decay(0.3);
                }

                double voice=voiceGenerator.getNext(frame, frameEx);

                // ------------------------------------------------------------
                // Split voice into voiced + aspiration for separate filtering
                // The aspiration path through cascade is often the real culprit
                // for "sharp" stop releases (not the frication path)
                // ------------------------------------------------------------
                double asp = voiceGenerator.getLastAspOut();
                double voicedOnly = voice - asp;

                // ------------------------------------------------------------
                // Source-tract coupling: modulate voiced source tilt by F1
                // ------------------------------------------------------------
                if (kSourceTractCoupling > 0.0) {
                    double f1Hz = frame->cf1;
                    if (f1Hz < 150.0) f1Hz = 150.0;
                    if (f1Hz > 1000.0) f1Hz = 1000.0;

                    // Normalize: 0 = low F1 (max coupling), 1 = high F1 (none)
                    double f1Norm = (f1Hz - 150.0) / 850.0;

                    double couplingAmt = (1.0 - f1Norm) * kSourceTractCoupling;

                    // Tilt: one-pole LP, max pole ~0.35 at lowest F1 + full coupling.
                    // This adds a gentle spectral rolloff to the source for high vowels.
                    double targetPole = couplingAmt * 0.35;
                    stcSmoothPole += (targetPole - stcSmoothPole) * stcAlpha;
                    double p = stcSmoothPole;
                    double stcOut = (1.0 - p) * voicedOnly + p * stcTiltState;
                    stcTiltState = stcOut;
                    voicedOnly = stcOut;

                    // F1 bandwidth widening: up to +25 Hz at lowest F1.
                    // Models increased damping from subglottal coupling.
                    double targetBwOff = couplingAmt * 25.0;
                    stcSmoothBwOff += (targetBwOff - stcSmoothBwOff) * stcAlpha;
                    cascade.setF1BwOffset(stcSmoothBwOff);
                }

                // Smooth frication amplitude
                double targetFricAmp = frame->fricationAmplitude;
                double fricAlpha = (targetFricAmp > smoothFricAmp) ? fricAttackAlpha : fricReleaseAlpha;
                smoothFricAmp += (targetFricAmp - smoothFricAmp) * fricAlpha;

                double fricAmp = smoothFricAmp;

                if (kFricSoftClipK > 0.0) {
                    fricAmp = fricAmp * (1.0 - kFricSoftClipK * fricAmp);
                    if (fricAmp < 0.0) fricAmp = 0.0;
                }

                double bypass = frame->parallelBypass;
                if (bypass < 0.0) bypass = 0.0;
                if (bypass > 1.0) bypass = 1.0;
                double bypassGain = 1.0 - bypass * (1.0 - kBypassMinGain);

                double va = frame->voiceAmplitude;
                if (va < 0.0) va = 0.0;
                if (va > 1.0) va = 1.0;

                double bypassVoicedDuck = 1.0;
                if (bypass > 0.3 && va > 0.0) {
                    bypassVoicedDuck = 1.0 - kBypassVoicedDuck * va;
                }

                double voicedFricScale = 1.0;
                if (va > 0.0) {
                    voicedFricScale = 1.0 - kVoicedFricDuck * pow(va, kVoicedFricDuckPower);
                    if (voicedFricScale < 0.0) voicedFricScale = 0.0;
                }

                // ------------------------------------------------------------
                // Adaptive frication filtering (burst vs sustained)
                // ------------------------------------------------------------
                // Detect burst onset from RAW target (not smoothed!) so we catch
                // the actual transient. Using smoothFricAmp would make dFric tiny
                // per-sample and burstiness would stay near 0.
                
                // Frication burst
                double dFric = targetFricAmp - lastTargetFricAmp;
                lastTargetFricAmp = targetFricAmp;
                double instFric = 0.0;
                if (dFric > 0.0) {
                    // Scale by sample rate so behavior is similar across SRs
                    double srScale = (double)sampleRate / 22050.0;
                    instFric = dFric * kBurstinessScale * srScale;
                    if (instFric > 1.0) instFric = 1.0;
                }

                // Aspiration burst (frame param)
                // Stop releases often use aspirationAmplitude, not fricationAmplitude,
                // so we need to detect bursts from aspiration changes too.
                double dAsp = frame->aspirationAmplitude - lastTargetAspAmp;
                lastTargetAspAmp = frame->aspirationAmplitude;
                double instAsp = 0.0;
                if (dAsp > 0.0) {
                    // Separate scale; aspirationAmplitude often changes more subtly than fricationAmplitude
                    double srScale = (double)sampleRate / 22050.0;
                    instAsp = dAsp * 40.0 * srScale;   // higher scale since asp changes are subtler
                    if (instAsp > 1.0) instAsp = 1.0;
                }

                // Take the max of frication and aspiration burst
                double inst = instFric;
                if (instAsp > inst) inst = instAsp;

                // Prefer burst filtering when voicing is low (classic stop burst)
                // This helps /k/, /t/, /p/ bursts get more lowpass while leaving
                // voiced fricatives like /z/ or /v/ less affected
                inst *= (1.0 - va);

                // Hold/decay envelope: this is critical!
                // Without this, burstiness only fires for 1 sample (when frame changes),
                // which is inaudible. The envelope sustains it for ~6ms (where stop
                // releases actually live).
                burstEnv *= burstEnvDecayMul;
                if (inst > burstEnv) burstEnv = inst;
                double burstiness = burstEnv;

                // ------------------------------------------------------------
                // Filter aspiration noise using the same burst envelope
                // This is the key fix: aspiration through the cascade is often
                // the real source of "sharp" stop releases
                // ------------------------------------------------------------
                double aspFilt = aspLp2.process(aspLp1.process(asp));
                // Crossfade: during bursts, use filtered aspiration; otherwise use original
                asp = asp + burstiness * (aspFilt - asp);
                double voiceForCascade = voicedOnly + asp;

                // ------------------------------------------------------------
                // Subglottal coupling: Sg1 pole-zero pair, gated by glottal phase
                // ------------------------------------------------------------
                // The tracheal resonance couples through the open glottis.
                // Always run the resonators (keeps state continuous), then
                // crossfade between dry and coupled signal.
                if (kSubglottalCoupling > 0.0) {
                    double sgTarget = voiceGenerator.glottisOpen
                        ? kSubglottalCoupling
                        : kSubglottalCoupling * kSgClosedPhaseLeak;
                    sgCouplingSmooth += (sgTarget - sgCouplingSmooth) * sgCouplingAlpha;

                    // Anti-resonance (zero) then resonance (pole) — Klatt ordering
                    double sgFiltered = sgZero.resonate(voiceForCascade, kSgZ1Freq, kSgZ1Bw);
                    sgFiltered = sgPole.resonate(sgFiltered, kSg1Freq, kSg1Bw);

                    voiceForCascade += sgCouplingSmooth * (sgFiltered - voiceForCascade);
                }

                double cascadeOut = cascade.getNext(frame, frameEx, voiceGenerator.glottisOpen, voiceForCascade * smoothPreGain);

                // Generate raw frication noise
                // No sample-rate scaling here: brownish noise (getNext) already
                // partially self-adapts via its IIR feedback (0.75*lastValue).
                double fricNoise = fricGenerator.getNext() * kFricNoiseScale * fricAmp * bypassGain * bypassVoicedDuck * voicedFricScale;

                // Apply tilt filter to frication (same tilt as aspiration for now)
                fricNoise = voiceGenerator.applyFricationTilt(fricNoise);

                // Optional Klatt-style glottal-cycle AM for noise sources.
                // This is driven by the voiced pitch cycle and will be 1.0
                // when disabled/unvoiced.
                fricNoise *= voiceGenerator.getLastNoiseMod();

                // Pitch-synchronous voiced-fricative modulation (Rabiner 1968;
                // Stevens 1971). For voiced fricatives (/z/,/v/,/ʒ/,/ð/) the
                // turbulence noise tracks transglottal airflow, giving the
                // buzzing hiss that distinguishes them from voiceless /s/ and,
                // crucially, survives band-limiting on low-end devices (the cue
                // is a low-rate amplitude modulation, not an out-of-band peak).
                // Mean-preserving (uses the zero-mean flow AC) so the hiss
                // buzzes without losing loudness; gated on genuine voiced
                // frication so vowels and voiceless /s/ are untouched.
                if (va * fricAmp > kVoicedFricProductThresh) {
                    double flowAC = voiceGenerator.getFlowNormAC();     // ~[-0.4,+0.6]
                    double mod = 1.0 + kVoicedFricModDepth * flowAC;
                    if (mod < 0.0) mod = 0.0;
                    fricNoise *= mod;
                }

                // Apply adaptive lowpass filtering:
                // - Burst path (2-pole cascade at lower cutoff): removes harsh highs from stops
                // - Sustain path (2-pole cascade at higher cutoff): preserves sibilant crispness
                double fricBurst = fricBurstLp2.process(fricBurstLp1.process(fricNoise));
                double fricSustain = fricSustainLp2.process(fricSustainLp1.process(fricNoise));

                // Crossfade based on burstiness:
                // burstiness=1 -> use burst (darker), burstiness=0 -> use sustain (brighter)
                double fric = fricSustain + burstiness * (fricBurst - fricSustain);

                // Consonant clarity boost (CVR). Raise sustained-fricative level
                // so the consonant survives band-limiting + device noise-
                // suppression (Miller & Nicely 1955; Sroka & Braida 1989).
                // (1 - burstiness) gates the boost OUT of stop bursts (which
                // benefit from transitions, not level) and applies it fully to
                // sustained fricatives. Vowels carry fric≈0 so are untouched.
                fric *= 1.0 + kConsonantClarityBoost * (1.0 - burstiness);

                double parallelOut=parallel.getNext(frame, frameEx, voiceGenerator.glottisOpen, fric*smoothPreGain);

                // Duck cascade residual during stop bursts to prevent energy addition.
                // Mid-sentence, cascade resonators still ring from the previous vowel
                // when the parallel burst fires — the sum causes an amplitude spike.
                // burstEnv high + va low = voiceless stop burst = duck cascade by 70%.
                // Nasal-aware: when caNP > 0, cascade carries wanted nasal murmur —
                // don't duck it.  caNP fades to 0 during nasal→stop transition, so
                // by the time the burst fires the full duck applies naturally.
                double nasalProtect = 1.0 - frame->caNP;
                double targetCascadeDuck = 1.0 - 0.7 * burstEnv * (1.0 - va) * nasalProtect;
                // Smooth the duck to prevent gain snap at stop→vowel boundary.
                // Without this, cascadeDuck jumps from 0.3→1.0 when va rises,
                // tripling the cascade ringing amplitude in one sample → click.
                // (The shelf duck at line ~453 already does this via shelfMixAlpha.)
                smoothCascadeDuck += cascadeDuckAlpha * (targetCascadeDuck - smoothCascadeDuck);
                double out=(cascadeOut*smoothCascadeDuck+parallelOut)*frame->outputGain;

                // DC blocking
                double filteredOut=out-lastInput+0.9995*lastOutput;
                lastInput=out;
                lastOutput=filteredOut;

                // Apply high-shelf EQ with burst-aware ducking
                // Full shelf output (what we already had)
                double shelved = applyHighShelf(filteredOut);

                // Duck the shelf only during bursts.
                // burstEnv is already 0..1, and holds ~6ms.
                // kShelfDuckMax: 0.0 = no change, 0.7 means at burstEnv=1 you keep 30% of the shelf.
                const double kShelfDuckMax = 0.90;

                // Also respect voicing amount so vowels keep full shelf.
                // This keeps shelf strong when va is high even if burstEnv flickers.
                double vaGate = va;              // va already clamped 0..1 above
                double burstGate = burstEnv;     // 0..1

                // Target mix: mostly 1.0, dips during bursts when voicing is low.
                double targetShelfMix = 1.0 - kShelfDuckMax * burstGate * (1.0 - vaGate);

                // Smooth it to avoid clicks
                shelfMix += (targetShelfMix - shelfMix) * shelfMixAlpha;

                // Crossfade between unshelved and shelved
                double bright = filteredOut + shelfMix * (shelved - filteredOut);

                // Apply start fade-in if active (prevents pop on speech start)
                if (startFadeRemaining > 0) {
                    double fadeIn = 1.0 - ((double)startFadeRemaining / (double)startFadeTotal);
                    bright *= fadeIn;
                    startFadeRemaining--;
                }

                // Platform output gain: applied before the limiter so the
                // limiter can protect against clipping at all gain levels.
                bright *= outputGain;

                // Lookahead soft-knee peak limiter (issue #71).
                // Lookahead lets the limiter see peaks before they arrive,
                // preventing pitch-synchronous gain pumping. Hold bridges
                // inter-pulse troughs so gain stays constant across pitch cycles.
                //
                // Threshold scales with outputGain so normal speech passes
                // through cleanly at any platform gain level (issue #50).
                {
                    constexpr double kClipLevel = 32767.0 / 6000.0;  // ~5.46
                    double effThreshold = (limiterThreshold * outputGain < kClipLevel)
                                        ? limiterThreshold * outputGain
                                        : kClipLevel;
                    constexpr double kneeHalf = 1.0;
                    double kneeLo = effThreshold - kneeHalf;
                    double kneeHi = effThreshold + kneeHalf;

                    // Push current sample into delay line, retrieve delayed sample.
                    double delayed = limiterDelayBuf[limiterDelayIdx];
                    limiterDelayBuf[limiterDelayIdx] = bright;
                    limiterDelayIdx = (limiterDelayIdx + 1) % limiterDelayLen;

                    // Scan lookahead window for peak magnitude.
                    double peakAhead = 0.0;
                    for (int li = 0; li < limiterDelayLen; ++li) {
                        double mag = fabs(limiterDelayBuf[li]);
                        if (mag > peakAhead) peakAhead = mag;
                    }

                    // Soft-knee gain from lookahead peak.
                    double targetGain;
                    if (peakAhead <= kneeLo || peakAhead < 1e-12) {
                        targetGain = 1.0;
                    } else if (peakAhead >= kneeHi) {
                        targetGain = effThreshold / peakAhead;
                    } else {
                        double t = (peakAhead - kneeLo) / (kneeHi - kneeLo);
                        double fullGain = effThreshold / peakAhead;
                        targetGain = 1.0 + t * t * (fullGain - 1.0);
                    }

                    // Attack / hold / release.
                    if (targetGain < limiterGain) {
                        limiterGain += limiterAttackAlpha * (targetGain - limiterGain);
                        limiterHoldCounter = limiterHoldSamples;
                    } else if (limiterHoldCounter > 0) {
                        limiterHoldCounter--;
                        // Hold: maintain current gain, don't release yet.
                    } else {
                        limiterGain += limiterReleaseAlpha * (targetGain - limiterGain);
                    }

                    // Apply gain to delayed signal.
                    bright = delayed * limiterGain;
                }

                // Store for fade-out tail on interrupt
                lastBrightOut = bright;

                double scaled = bright * 6000.0;
                const double limit = 32767.0;
                if(scaled > limit) scaled = limit;
                if(scaled < -limit) scaled = -limit;

                // Time-stretch: accelerated frame consumption.
                // Advance the frame manager extra ticks so the speech
                // timeline moves faster. Every output sample is cleanly
                // generated — no cycle skipping, no discontinuities.
                if (timeStretchFactor > 1.01) {
                    timeStretchAccum += (timeStretchFactor - 1.0);
                    while (timeStretchAccum >= 1.0) {
                        // Consume an extra frame tick (advances speech timeline)
                        frameManager->getCurrentFrameWithEx(NULL);
                        timeStretchAccum -= 1.0;
                    }
                }

                sampleBuf[i].value = (int)scaled;
                if(indexReachedOut) {
                    const int curUserIndex=frameManager->getLastIndex();
                    if(curUserIndex!=entryUserIndex) {
                        *indexReachedOut=curUserIndex;
                        if(i+1<sampleCount) memset(&sampleBuf[i+1], 0, (sampleCount-(i+1))*sizeof(sample));
                        return i+1;
                    }
                }
            } else {
                // No frame available - handle stop/interrupt with fade-out to avoid click
                
                // If we were mid-speech, fade out for a few ms to avoid click on interrupt
                if (!wasSilence) {
                    if (stopFadeTotal == 0) {
                        stopFadeTotal = (int)(sampleRate * 0.004); // 4 ms
                        if (stopFadeTotal < 16) stopFadeTotal = 16;
                        stopFadeRemaining = stopFadeTotal;
                    }
                    if (stopFadeRemaining > 0) {
                        // Fade from 1.0 to 0.0 inclusive (last sample is exactly 0)
                        double t = (double)(stopFadeRemaining - 1) / (double)(stopFadeTotal - 1);
                        double tail = lastBrightOut * t;
                        double scaled = tail * 6000.0;
                        const double limit = 32767.0;
                        if (scaled > limit) scaled = limit;
                        if (scaled < -limit) scaled = -limit;
                        sampleBuf[i].value = (int)scaled;
                        stopFadeRemaining--;
                        if(indexReachedOut) {
                            const int curUserIndex=frameManager->getLastIndex();
                            if(curUserIndex!=entryUserIndex) {
                                *indexReachedOut=curUserIndex;
                                if(i+1<sampleCount) memset(&sampleBuf[i+1], 0, (sampleCount-(i+1))*sizeof(sample));
                                return i+1;
                            }
                        }
                        // Keep going; we might fill the rest of the buffer with the tail
                        continue;
                    }
                }
                // Fade finished (or we were already silent)
                wasSilence = true;
                stopFadeTotal = 0;
                stopFadeRemaining = 0;
                // Reset high-shelf biquad state so next utterance starts clean
                hsIn1 = hsIn2 = hsOut1 = hsOut2 = 0.0;
                // Zero-fill remainder of buffer to prevent garbage audio if caller
                // plays the full buffer regardless of return value
                if (i < sampleCount) {
                    memset(&sampleBuf[i], 0, (sampleCount - i) * sizeof(sample));
                }
                // Legacy synthesize also returns mid-buffer here without a
                // sample for this tick, so parity holds. Report a marker that
                // flipped on the way into this silence (e.g. a trailing index
                // right before the queue drained).
                if(indexReachedOut) {
                    const int curUserIndex=frameManager->getLastIndex();
                    if(curUserIndex!=entryUserIndex) *indexReachedOut=curUserIndex;
                }
                return i;
            }
        }
        return sampleCount;
    }

    void setFrameManager(FrameManager* frameManager) {
        this->frameManager=frameManager;
    }

    void setVoicingTone(const speechPlayer_voicingTone_t* tone) {
        // Legacy ABI: the original voicingTone struct was just 7 doubles.
        // We still accept that layout when the magic header doesn't match.
        struct speechPlayer_voicingTone_v1_t {
            double voicingPeakPos;
            double voicedPreEmphA;
            double voicedPreEmphMix;
            double highShelfGainDb;
            double highShelfFcHz;
            double highShelfQ;
            double voicedTiltDbPerOct;
        };

        speechPlayer_voicingTone_t merged = speechPlayer_getDefaultVoicingTone();

        if (tone) {
            // Accept v2+ structs: check magic and that structSize at least covers the header.
            // This allows future v3/v4 structs (with appended fields) to work without
            // falling back to the legacy 7-double path.
            const bool looksLikeHeader = (tone->magic == SPEECHPLAYER_VOICINGTONE_MAGIC &&
                                          tone->structSize >= sizeof(uint32_t) * 4);

            if (looksLikeHeader) {
                // Caller is providing the v2 struct layout.
                size_t copySize = (size_t)tone->structSize;

                // If the caller forgot to set structSize, but did set the header,
                // assume they're passing the full struct.
                if (copySize < sizeof(uint32_t) * 4) copySize = sizeof(speechPlayer_voicingTone_t);

                // Basic sanity clamps in case a mismatched caller passes garbage.
                if (copySize > sizeof(speechPlayer_voicingTone_t)) copySize = sizeof(speechPlayer_voicingTone_t);

                // Start from defaults so missing tail fields get sane values.
                memcpy(&merged, tone, copySize);
            } else {
                // Legacy (v1) layout: 7 doubles, no header.
                const speechPlayer_voicingTone_v1_t* v1 = (const speechPlayer_voicingTone_v1_t*)tone;
                merged.voicingPeakPos = v1->voicingPeakPos;
                merged.voicedPreEmphA = v1->voicedPreEmphA;
                merged.voicedPreEmphMix = v1->voicedPreEmphMix;
                merged.highShelfGainDb = v1->highShelfGainDb;
                merged.highShelfFcHz = v1->highShelfFcHz;
                merged.highShelfQ = v1->highShelfQ;
                merged.voicedTiltDbPerOct = v1->voicedTiltDbPerOct;
                // newer fields stay at default (0.0)
            }
        }

        // Always normalize the header values to what this build implements.
        merged.magic = SPEECHPLAYER_VOICINGTONE_MAGIC;
        merged.structSize = (uint32_t)sizeof(speechPlayer_voicingTone_t);
        merged.structVersion = SPEECHPLAYER_VOICINGTONE_VERSION;
        merged.dspVersion = SPEECHPLAYER_DSP_VERSION;

        currentTone = merged;

        voiceGenerator.setVoicingParams(
            currentTone.voicingPeakPos,
            currentTone.voicedPreEmphA,
            currentTone.voicedPreEmphMix,
            currentTone.voicedTiltDbPerOct,
            currentTone.noiseGlottalModDepth,
            currentTone.speedQuotient,
            currentTone.aspirationTiltDbPerOct
        );

        // Update high-shelf coefficients (do not reset state)
        initHighShelf(currentTone.highShelfFcHz, currentTone.highShelfGainDb, currentTone.highShelfQ);
        
        // Update pitch-sync F1 modulation params (include glottal-cycle BW delta)
        cascade.setPitchSyncParams(currentTone.pitchSyncF1DeltaHz,
                                   currentTone.pitchSyncB1DeltaHz + kGlottalCycleBwDelta);
        cascade.setCascadeBwScale(currentTone.cascadeBwScale);

        // Vocal tract shape parameters (v4)
        cascade.setNasalBwScale(currentTone.nasalBwScale);
        cascade.setF4FreqScale(currentTone.f4FreqScale);
        cascade.setNasalGainScale(currentTone.nasalGainScale);
        parallel.setF4FreqScale(currentTone.f4FreqScale);

        // Update tremor depth for elderly/shaky voice
        voiceGenerator.setTremorDepth(currentTone.tremorDepth);

        // Dual-oscillator chorus for vocal fold asymmetry
        voiceGenerator.setChorusParams(currentTone.chorusDepth, currentTone.chorusDetuneHz);
    }

    void getVoicingTone(speechPlayer_voicingTone_t* tone) {
        if (!tone) return;

        // Legacy ABI: if the caller didn't set magic, assume they're using
        // the old 7-double layout and only write those fields.
        struct speechPlayer_voicingTone_v1_t {
            double voicingPeakPos;
            double voicedPreEmphA;
            double voicedPreEmphMix;
            double highShelfGainDb;
            double highShelfFcHz;
            double highShelfQ;
            double voicedTiltDbPerOct;
        };

        // Accept v2+ callers: check magic and that structSize at least covers the header
        const bool callerWantsHeader = (tone->magic == SPEECHPLAYER_VOICINGTONE_MAGIC &&
                                        tone->structSize >= sizeof(uint32_t) * 4);

        if (!callerWantsHeader) {
            speechPlayer_voicingTone_v1_t* v1 = (speechPlayer_voicingTone_v1_t*)tone;
            v1->voicingPeakPos = currentTone.voicingPeakPos;
            v1->voicedPreEmphA = currentTone.voicedPreEmphA;
            v1->voicedPreEmphMix = currentTone.voicedPreEmphMix;
            v1->highShelfGainDb = currentTone.highShelfGainDb;
            v1->highShelfFcHz = currentTone.highShelfFcHz;
            v1->highShelfQ = currentTone.highShelfQ;
            v1->voicedTiltDbPerOct = currentTone.voicedTiltDbPerOct;
            return;
        }

        // V2/V3: respect the caller-provided buffer size.
        speechPlayer_voicingTone_t tmp = currentTone;
        tmp.magic = SPEECHPLAYER_VOICINGTONE_MAGIC;
        tmp.structSize = (uint32_t)sizeof(speechPlayer_voicingTone_t);
        tmp.structVersion = SPEECHPLAYER_VOICINGTONE_VERSION;
        tmp.dspVersion = SPEECHPLAYER_DSP_VERSION;

        size_t writeSize = sizeof(speechPlayer_voicingTone_t);
        if (tone->structSize >= sizeof(uint32_t) * 4 && tone->structSize < writeSize) {
            writeSize = (size_t)tone->structSize;
        }
        memcpy(tone, &tmp, writeSize);
    }

    void setOutputGain(double gain) {
        if (gain < 0.0) gain = 0.0;
        if (gain > 10.0) gain = 10.0;
        outputGain = gain;
    }

    void setTimeStretch(double factor) {
        if (factor < 1.0) factor = 1.0;
        if (factor > 8.0) factor = 8.0;
        timeStretchFactor = factor;
        timeStretchAccum = 0.0;
    }
};

SpeechWaveGenerator* SpeechWaveGenerator::create(int sampleRate) {return new SpeechWaveGeneratorImpl(sampleRate); }