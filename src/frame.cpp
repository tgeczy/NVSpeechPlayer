/*
TGSpeechBox — Frame interpolation and queue management.
Copyright 2014 NV Access Limited.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include <queue>
#include <cstring>
#include <cstddef>  // offsetof
#include "utils.h"
#include "frame.h"

using namespace std;

// Identifies which speechPlayer_frame_t parameter indices represent Hz frequencies.
// These get log-domain interpolation; everything else gets linear.
// Uses offsetof so it stays correct if the struct layout ever changes.
static inline bool isFrequencyParam(int idx) {
	const int szP = sizeof(speechPlayer_frameParam_t);
	// voicePitch and endVoicePitch
	if(idx == (int)(offsetof(speechPlayer_frame_t, voicePitch) / szP)) return true;
	if(idx == (int)(offsetof(speechPlayer_frame_t, endVoicePitch) / szP)) return true;
	// Cascade formant frequencies: cf1 through cfNP
	int cfFirst = (int)(offsetof(speechPlayer_frame_t, cf1) / szP);
	int cfLast  = (int)(offsetof(speechPlayer_frame_t, cfNP) / szP);
	if(idx >= cfFirst && idx <= cfLast) return true;
	// Parallel formant frequencies: pf1 through pf6
	int pfFirst = (int)(offsetof(speechPlayer_frame_t, pf1) / szP);
	int pfLast  = (int)(offsetof(speechPlayer_frame_t, pf6) / szP);
	if(idx >= pfFirst && idx <= pfLast) return true;
	return false;
}

// Identifies which frame parameter indices belong to the F1 group.
// Used for per-parameter transition speed scaling.
static inline bool isF1Param(int idx) {
	const int szP = sizeof(speechPlayer_frameParam_t);
	int i;
	i = (int)(offsetof(speechPlayer_frame_t, cf1) / szP); if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, pf1) / szP); if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, cb1) / szP); if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, pb1) / szP); if(idx == i) return true;
	return false;
}

static inline bool isF2Param(int idx) {
	const int szP = sizeof(speechPlayer_frameParam_t);
	int i;
	i = (int)(offsetof(speechPlayer_frame_t, cf2) / szP); if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, pf2) / szP); if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, cb2) / szP); if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, pb2) / szP); if(idx == i) return true;
	return false;
}

static inline bool isF3Param(int idx) {
	const int szP = sizeof(speechPlayer_frameParam_t);
	int i;
	i = (int)(offsetof(speechPlayer_frame_t, cf3) / szP); if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, pf3) / szP); if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, cb3) / szP); if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, pb3) / szP); if(idx == i) return true;
	return false;
}

static inline bool isNasalParam(int idx) {
	const int szP = sizeof(speechPlayer_frameParam_t);
	int i;
	i = (int)(offsetof(speechPlayer_frame_t, cfN0) / szP); if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, cfNP) / szP); if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, cbN0) / szP); if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, cbNP) / szP); if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, caNP) / szP); if(idx == i) return true;
	return false;
}

// Identifies which frame parameters are amplitude/gain sources.
// These get equal-power crossfade when transAmplitudeMode > 0.5,
// preventing energy dips at voiced→voiceless transitions.
// Intentionally does NOT include:
//   - outputGain (master volume, not a source)
//   - pa1-pa6 (parallel amplitudes track their formants, not sources)
//   - caNP (nasal pole amplitude — tracks nasal formant, not a source)
static inline bool isAmplitudeParam(int idx) {
	const int szP = sizeof(speechPlayer_frameParam_t);
	int i;
	i = (int)(offsetof(speechPlayer_frame_t, voiceAmplitude) / szP);
	if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, aspirationAmplitude) / szP);
	if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, fricationAmplitude) / szP);
	if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, voiceTurbulenceAmplitude) / szP);
	if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, preFormantGain) / szP);
	if(idx == i) return true;
	return false;
}

// Noise source amplitudes — these get delayed fadeout when
// transSourceHoldRatio > 0 to create temporal overlap with voicing.
// Does NOT include voiceAmplitude (voicing uses normal timing).
static inline bool isNoiseSourceParam(int idx) {
	const int szP = sizeof(speechPlayer_frameParam_t);
	int i;
	i = (int)(offsetof(speechPlayer_frame_t, aspirationAmplitude) / szP);
	if(idx == i) return true;
	i = (int)(offsetof(speechPlayer_frame_t, fricationAmplitude) / szP);
	if(idx == i) return true;
	return false;
}

// Voicing amplitude — gets delayed onset when transVoicingHoldRatio > 0.
// Keeps voiceAmplitude at old value (e.g. zero after affricate) for the
// first fraction of the crossfade, then ramps to new value.
static inline bool isVoicingParam(int idx) {
	const int szP = sizeof(speechPlayer_frameParam_t);
	int i = (int)(offsetof(speechPlayer_frame_t, voiceAmplitude) / szP);
	return (idx == i);
}

// Amplitude sources complete their move within this many ms of a fade's
// start. The Klatt lineage treats amplitude changes as near-step events
// (voicing onsets abrupt, amplitude spans <= ~45ms) while formants glide
// across the full transition. Fades shorter than this span are untouched,
// so normal-rate output (fades <= ~30ms) is bit-identical; only the long
// fades that survive the frontend's slow-rate 130ms cap engage it.
static const double kAmpSpanMs = 45.0;

struct frameRequest_t {
	unsigned int minNumSamples;
	unsigned int numFadeSamples;
	bool NULLFrame;

	// Optional per-frame voice quality params (DSP v5+)
	bool hasFrameEx;
	speechPlayer_frameEx_t frameEx;

	speechPlayer_frame_t frame;
	double voicePitchInc;
	
	// Formant end targets for exponential smoothing (DECTalk-style transitions)
	// NAN = no ramping for that formant
	double endCf1, endCf2, endCf3;  // Cascade formant targets (Hz)
	double endPf1, endPf2, endPf3;  // Parallel formant targets (Hz)
	double formantAlpha;            // Exponential smoothing coefficient
	
	int userIndex;
};

class FrameManagerImpl: public FrameManager {
	private:
	LockableObject frameLock;
	queue<frameRequest_t*> frameRequestQueue;
	frameRequest_t* oldFrameRequest;
	frameRequest_t* newFrameRequest;
	speechPlayer_frame_t curFrame;
	speechPlayer_frameEx_t curFrameEx;
	bool curFrameIsNULL;
	bool curHasFrameEx;
	unsigned int sampleCounter;
	int lastUserIndex;
	bool purgeFlag;  // Set on purge, cleared when checked
	int sampleRate;  // Needed to express kAmpSpanMs in samples

	void updateCurrentFrame() {
		sampleCounter++;
		if(newFrameRequest) {
			if(sampleCounter>(newFrameRequest->numFadeSamples)) {
				delete oldFrameRequest;
				oldFrameRequest=newFrameRequest;
				newFrameRequest=NULL;
				// Ensure curFrame is updated even when numFadeSamples==0.
				memcpy(&curFrame, &(oldFrameRequest->frame), sizeof(speechPlayer_frame_t));
				memcpy(&curFrameEx, &(oldFrameRequest->frameEx), sizeof(speechPlayer_frameEx_t));
				curHasFrameEx = oldFrameRequest->hasFrameEx;
			} else {
				double linearRatio=(double)sampleCounter/(newFrameRequest->numFadeSamples);

				// Compressed clock for amplitude sources (see kAmpSpanMs):
				// they finish their old->new move within the span while
				// spectral params glide across the whole fade. Held params
				// (source/voicing holds) keep their own deliberate timing
				// and never use this.
				double ampRatio = linearRatio;
				{
					const double ampSpanSamples = kAmpSpanMs * (double)sampleRate / 1000.0;
					if ((double)(newFrameRequest->numFadeSamples) > ampSpanSamples) {
						ampRatio = (double)sampleCounter / ampSpanSamples;
						if (ampRatio > 1.0) ampRatio = 1.0;
					}
				}

				// When the incoming frame has no formant ramp targets (all
				// endCf = NAN), the frontend has already handled the formant
				// trajectory (e.g. diphthong staircase micro-frames).  Use
				// simple linear interpolation — no cosine smoothing, no
				// per-parameter acceleration, no transition BW widening.
				// The sophisticated crossfade features cause shimmer on
				// staircase steps by making formants linger at harmonic-
				// formant crossings (cosine S-curve) and pumping Q
				// (transition BW widening).
				const bool noFormantTargets = newFrameRequest->hasFrameEx &&
					!std::isfinite(newFrameRequest->endCf1) &&
					!std::isfinite(newFrameRequest->endCf2) &&
					!std::isfinite(newFrameRequest->endCf3);

				if (noFormantTargets) {
					// Simple linear crossfade — no cosine smoothing, no BW
					// widening (those cause shimmer on staircase steps).
					// But DO apply per-parameter amplitude timing: source
					// hold and voicing hold are pure amplitude operations
					// that don't cause shimmer.
					double sourceHoldSimple = newFrameRequest->hasFrameEx ? newFrameRequest->frameEx.transSourceHoldRatio : 0.0;
					if (sourceHoldSimple > 0.99) sourceHoldSimple = 0.99;
					double voicingHoldSimple = newFrameRequest->hasFrameEx ? newFrameRequest->frameEx.transVoicingHoldRatio : 0.0;
					if (voicingHoldSimple > 0.99) voicingHoldSimple = 0.99;

					for(int i=0;i<speechPlayer_frame_numParams;++i) {
						double oldVal = ((speechPlayer_frameParam_t*)&(oldFrameRequest->frame))[i];
						double newVal = ((speechPlayer_frameParam_t*)&(newFrameRequest->frame))[i];
						if(isNoiseSourceParam(i) && sourceHoldSimple > 0.0) {
							double dr = (linearRatio <= sourceHoldSimple) ? 0.0
								: (linearRatio - sourceHoldSimple) / (1.0 - sourceHoldSimple);
							double oldContrib = oldVal * (1.0 - dr);
							double newContrib = newVal * linearRatio;
							double val = oldContrib + newContrib;
							double maxVal = (oldVal > newVal) ? oldVal : newVal;
							if(val > maxVal) val = maxVal;
							((speechPlayer_frameParam_t*)&curFrame)[i] = val;
						} else if(isVoicingParam(i) && voicingHoldSimple > 0.0) {
							double dr = (linearRatio <= voicingHoldSimple) ? 0.0
								: (linearRatio - voicingHoldSimple) / (1.0 - voicingHoldSimple);
							double oldContrib = oldVal * (1.0 - linearRatio);
							double newContrib = newVal * dr;
							double val = oldContrib + newContrib;
							double maxVal = (oldVal > newVal) ? oldVal : newVal;
							if(val > maxVal) val = maxVal;
							((speechPlayer_frameParam_t*)&curFrame)[i] = val;
						} else if(isAmplitudeParam(i)) {
							((speechPlayer_frameParam_t*)&curFrame)[i]=calculateValueAtFadePosition(oldVal, newVal, ampRatio);
						} else {
							((speechPlayer_frameParam_t*)&curFrame)[i]=calculateValueAtFadePosition(oldVal, newVal, linearRatio);
						}
					}
				} else {

				// Cosine ease-in/ease-out for spectral parameters only.
				// Amplitude/gain parameters stay linear so that energy crossfades
				// are monotonic — the S-curve can create brief energy dips at
				// source transitions (e.g. voiced stop → aspiration) that sound
				// like pops.
				double cosineRatio = cosineSmooth(linearRatio);

				// Per-parameter transition scales: if the new frame carries
				// a non-zero scale for a formant group, that group fades
				// faster (or slower) than the base rate.  Scale < 1 means
				// "reach target in that fraction of the fade, then hold."
				// We read from the NEW frame's FrameEx because it represents
				// the segment we're transitioning INTO — its transition
				// metadata describes how to ARRIVE at its targets.
				const double scF1 = newFrameRequest->hasFrameEx ? newFrameRequest->frameEx.transF1Scale : 0.0;
				const double scF2 = newFrameRequest->hasFrameEx ? newFrameRequest->frameEx.transF2Scale : 0.0;
				const double scF3 = newFrameRequest->hasFrameEx ? newFrameRequest->frameEx.transF3Scale : 0.0;
				const double scN  = newFrameRequest->hasFrameEx ? newFrameRequest->frameEx.transNasalScale : 0.0;
				const double ampMode = newFrameRequest->hasFrameEx ? newFrameRequest->frameEx.transAmplitudeMode : 0.0;
				double sourceHoldRatio = newFrameRequest->hasFrameEx ? newFrameRequest->frameEx.transSourceHoldRatio : 0.0;
				if (sourceHoldRatio > 0.99) sourceHoldRatio = 0.99;  // prevent division by zero
				double voicingHoldRatio = newFrameRequest->hasFrameEx ? newFrameRequest->frameEx.transVoicingHoldRatio : 0.0;
				if (voicingHoldRatio > 0.99) voicingHoldRatio = 0.99;

				for(int i=0;i<speechPlayer_frame_numParams;++i) {
					double oldVal = ((speechPlayer_frameParam_t*)&(oldFrameRequest->frame))[i];
					double newVal = ((speechPlayer_frameParam_t*)&(newFrameRequest->frame))[i];

					// Determine this parameter's effective fade ratio.
					double paramLinear = linearRatio;
					double scale = 0.0;
					if(scF1 > 0.0 && isF1Param(i))      scale = scF1;
					else if(scF2 > 0.0 && isF2Param(i))  scale = scF2;
					else if(scF3 > 0.0 && isF3Param(i))  scale = scF3;
					else if(scN > 0.0 && isNasalParam(i)) scale = scN;

					if(scale > 0.0 && scale != 1.0) {
						// Accelerate (or decelerate) this parameter's fade.
						// At scale=0.3, the parameter reaches its target when
						// linearRatio hits 0.3, then holds for the remainder.
						paramLinear = linearRatio / scale;
						if(paramLinear > 1.0) paramLinear = 1.0;
					}

					if(isFrequencyParam(i)) {
						double paramCosine = cosineSmooth(paramLinear);
						((speechPlayer_frameParam_t*)&curFrame)[i]=calculateFreqAtFadePosition(oldVal, newVal, paramCosine);
					} else if(isNoiseSourceParam(i) && sourceHoldRatio > 0.0) {
						// Delayed noise fadeout: old noise holds, then fades.
						// Voicing uses normal ratio — creates temporal overlap
						// where both frication and voicing are active.
						double delayedRatio = (paramLinear <= sourceHoldRatio) ? 0.0
							: (paramLinear - sourceHoldRatio) / (1.0 - sourceHoldRatio);
						if(ampMode > 0.5) {
							// Equal-power with delayed ratio for old, normal for new.
							double theta = delayedRatio * 1.5707963267948966;
							double thetaNew = paramLinear * 1.5707963267948966;
							double val = oldVal * cos(theta) + newVal * sin(thetaNew);
							((speechPlayer_frameParam_t*)&curFrame)[i] = val;
						} else {
							double oldContrib = oldVal * (1.0 - delayedRatio);
							double newContrib = newVal * paramLinear;
							double val = oldContrib + newContrib;
							double maxVal = (oldVal > newVal) ? oldVal : newVal;
							if(val > maxVal) val = maxVal;
							((speechPlayer_frameParam_t*)&curFrame)[i] = val;
						}
					} else if(isVoicingParam(i) && voicingHoldRatio > 0.0) {
						// Delayed voicing onset: new voicing holds at old value,
						// then ramps.  Mirror of noise hold — old fades normally,
						// new ramps late.  Creates temporal separation between
						// affricate release (frication) and vowel onset (voicing).
						double delayedRatio = (paramLinear <= voicingHoldRatio) ? 0.0
							: (paramLinear - voicingHoldRatio) / (1.0 - voicingHoldRatio);
						if(ampMode > 0.5) {
							// Equal-power: old fades normally, new ramps delayed.
							double theta = paramLinear * 1.5707963267948966;
							double thetaNew = delayedRatio * 1.5707963267948966;
							double val = oldVal * cos(theta) + newVal * sin(thetaNew);
							((speechPlayer_frameParam_t*)&curFrame)[i] = val;
						} else {
							double oldContrib = oldVal * (1.0 - paramLinear);
							double newContrib = newVal * delayedRatio;
							double val = oldContrib + newContrib;
							double maxVal = (oldVal > newVal) ? oldVal : newVal;
							if(val > maxVal) val = maxVal;
							((speechPlayer_frameParam_t*)&curFrame)[i] = val;
						}
					} else if(isAmplitudeParam(i) && ampMode > 0.5) {
						// Equal-power crossfade: sin²(θ) + cos²(θ) = 1
						// Total energy stays constant across source transitions.
						// Runs on the compressed amplitude clock (kAmpSpanMs).
						double theta = ampRatio * 1.5707963267948966;
						double val = oldVal * cos(theta) + newVal * sin(theta);
						((speechPlayer_frameParam_t*)&curFrame)[i] = val;
					} else if(isAmplitudeParam(i)) {
						((speechPlayer_frameParam_t*)&curFrame)[i]=calculateValueAtFadePosition(oldVal, newVal, ampRatio);
					} else {
						((speechPlayer_frameParam_t*)&curFrame)[i]=calculateValueAtFadePosition(oldVal, newVal, paramLinear);
					}
				}

				// ── Transition bandwidth widening ──
				// During crossfades with large formant frequency changes,
				// temporarily widen cascade/parallel bandwidths so IIR
				// resonators can track without storing transient energy.
				// Sine envelope: peaks at mid-transition, zero at endpoints.
				{
					const int szP = (int)sizeof(speechPlayer_frameParam_t);
					auto widenForDelta = [&](int cfIdx, int cbIdx) {
						double oldF = ((speechPlayer_frameParam_t*)&(oldFrameRequest->frame))[cfIdx];
						double newF = ((speechPlayer_frameParam_t*)&(newFrameRequest->frame))[cfIdx];
						double deltaHz = fabs(newF - oldF);
						// Threshold lowered from 400→150 Hz and factor raised
						// from 0.25→0.40 so moderate formant jumps (affricate→vowel,
						// nasal→stop) get enough resonator damping to avoid
						// "stringy" chirp artifacts from IIR transient energy.
						if(deltaHz > 150.0) {
							double env = sin(linearRatio * 3.14159265);
							double extraBw = env * (deltaHz - 150.0) * 0.40;
							((speechPlayer_frameParam_t*)&curFrame)[cbIdx] += extraBw;
						}
					};
					// F1 widening added for nasal→stop transitions where
					// F1 jumps 300→150 Hz (velopharyngeal port closure).
					widenForDelta((int)(offsetof(speechPlayer_frame_t,cf1)/szP),
					              (int)(offsetof(speechPlayer_frame_t,cb1)/szP));
					widenForDelta((int)(offsetof(speechPlayer_frame_t,cf2)/szP),
					              (int)(offsetof(speechPlayer_frame_t,cb2)/szP));
					widenForDelta((int)(offsetof(speechPlayer_frame_t,cf3)/szP),
					              (int)(offsetof(speechPlayer_frame_t,cb3)/szP));
					widenForDelta((int)(offsetof(speechPlayer_frame_t,pf1)/szP),
					              (int)(offsetof(speechPlayer_frame_t,pb1)/szP));
					widenForDelta((int)(offsetof(speechPlayer_frame_t,pf2)/szP),
					              (int)(offsetof(speechPlayer_frame_t,pb2)/szP));
					widenForDelta((int)(offsetof(speechPlayer_frame_t,pf3)/szP),
					              (int)(offsetof(speechPlayer_frame_t,pb3)/szP));
				}
				} // end sophisticated crossfade

				if(oldFrameRequest->hasFrameEx || newFrameRequest->hasFrameEx) {
					curHasFrameEx = true;

					// Some FrameEx fields are *command-like* and must not be interpolated.
					// In particular, the Fujisaki pitch model triggers (amp/len/dur) must be
					// applied with their exact values at the start of a transition; otherwise
					// fades would scale them down and cause incorrect trigger timing.
					const int pitchStartIdx = (int)(offsetof(speechPlayer_frameEx_t, fujisakiEnabled) / sizeof(double));
					for(int i=0;i<speechPlayer_frameEx_numParams;++i) {
						if (i >= pitchStartIdx) {
							// Step to the NEW values immediately (no interpolation).
							((double*)&curFrameEx)[i]=((double*)&(newFrameRequest->frameEx))[i];
						} else {
							((double*)&curFrameEx)[i]=calculateValueAtFadePosition(((double*)&(oldFrameRequest->frameEx))[i],((double*)&(newFrameRequest->frameEx))[i],linearRatio);
						}
					}
					} else {
						curHasFrameEx = false;
						curFrameEx = speechPlayer_frameEx_defaults;
					}
			}
		} else if(sampleCounter>(oldFrameRequest->minNumSamples)) {
			if(!frameRequestQueue.empty()) {
				bool wasFromSilence = curFrameIsNULL || oldFrameRequest->NULLFrame;
				curFrameIsNULL=false;
				newFrameRequest=frameRequestQueue.front();
				frameRequestQueue.pop();
				if(newFrameRequest->NULLFrame) {
					double oldAsp = oldFrameRequest->frame.aspirationAmplitude;
					memcpy(&(newFrameRequest->frame),&(oldFrameRequest->frame),sizeof(speechPlayer_frame_t));
					// If old frame had aspiration (stops, /h/), keep a residual
					// preFormantGain so the aspiration tail survives through cascade.
					// For all other phonemes (vowels, fricatives), oldAsp ≈ 0 so
					// this has no effect — they get the same preFormantGain=0 as before.
					if (oldAsp > 0.01) {
						newFrameRequest->frame.preFormantGain = 0.3;
						// Keep aspirationAmplitude at old value — aspiration persists
						// while frication dies. This creates temporal separation.
					} else {
						newFrameRequest->frame.preFormantGain = 0;
						newFrameRequest->frame.aspirationAmplitude = 0;
					}
					newFrameRequest->frame.voiceAmplitude=0;
					newFrameRequest->frame.fricationAmplitude=0;
					newFrameRequest->frame.voiceTurbulenceAmplitude=0;
					newFrameRequest->frame.voicePitch=curFrame.voicePitch;
					newFrameRequest->voicePitchInc=0;

					// Carry frameEx through silence fades so transitions stay smooth.
					memcpy(&(newFrameRequest->frameEx),&(oldFrameRequest->frameEx),sizeof(speechPlayer_frameEx_t));
					newFrameRequest->hasFrameEx = oldFrameRequest->hasFrameEx;
				} else if(oldFrameRequest->NULLFrame) {
					memcpy(&(oldFrameRequest->frame),&(newFrameRequest->frame),sizeof(speechPlayer_frame_t));
					oldFrameRequest->frame.preFormantGain=0;
					// Zero source amplitudes so they fade in WITH preFormantGain.
					// Without this, noise/voicing hits the resonators at full strength
					// on sample 1, exciting residual IIR state → pop/click.
					oldFrameRequest->frame.voiceAmplitude=0;
					oldFrameRequest->frame.aspirationAmplitude=0;
					oldFrameRequest->frame.fricationAmplitude=0;
					oldFrameRequest->frame.voiceTurbulenceAmplitude=0;
					// FIX: We are transitioning from silence to real audio.
					// Mark the old request as non-NULL so subsequent transitions don't keep
					// taking the "from silence" path with stale state.
					oldFrameRequest->NULLFrame=false;

					memcpy(&(oldFrameRequest->frameEx),&(newFrameRequest->frameEx),sizeof(speechPlayer_frameEx_t));
					oldFrameRequest->hasFrameEx = newFrameRequest->hasFrameEx;
				}
				if(newFrameRequest) {
					if(newFrameRequest->userIndex!=-1) lastUserIndex=newFrameRequest->userIndex;
					sampleCounter=0;
					// Process the start of the transition immediately (sample 0), so the
					// first sample of a new segment can't use stale/garbage parameters.
					if(wasFromSilence) {
						memcpy(&curFrame, &(oldFrameRequest->frame), sizeof(speechPlayer_frame_t));
						memcpy(&curFrameEx, &(oldFrameRequest->frameEx), sizeof(speechPlayer_frameEx_t));
						curHasFrameEx = oldFrameRequest->hasFrameEx;
					}
					newFrameRequest->frame.voicePitch+=(newFrameRequest->voicePitchInc*newFrameRequest->numFadeSamples);
				}
			} else {
				curFrameIsNULL=true;
				curHasFrameEx=false;
				curFrameEx = speechPlayer_frameEx_defaults;
				// FIX: We have run out of frames. Mark the old request as NULL (Silence).
				// This ensures that when a new frame eventually arrives, the engine treats it
				// as a "Start from Silence" (triggering the 0-gain fade-in logic) rather than
				// trying to interpolate from the stale state of the last utterance.
				oldFrameRequest->NULLFrame = true;
			}
		} else {
			// Per-sample pitch ramping (linear)
			curFrame.voicePitch+=oldFrameRequest->voicePitchInc;
			oldFrameRequest->frame.voicePitch=curFrame.voicePitch;
			
			// Per-sample formant ramping with exponential smoothing
			// This mimics articulatory inertia - fast initial movement, gentle settling
			double alpha = oldFrameRequest->formantAlpha;
			if(std::isfinite(oldFrameRequest->endCf1)) {
				curFrame.cf1 += alpha * (oldFrameRequest->endCf1 - curFrame.cf1);
				oldFrameRequest->frame.cf1 = curFrame.cf1;
			}
			if(std::isfinite(oldFrameRequest->endCf2)) {
				curFrame.cf2 += alpha * (oldFrameRequest->endCf2 - curFrame.cf2);
				oldFrameRequest->frame.cf2 = curFrame.cf2;
			}
			if(std::isfinite(oldFrameRequest->endCf3)) {
				curFrame.cf3 += alpha * (oldFrameRequest->endCf3 - curFrame.cf3);
				oldFrameRequest->frame.cf3 = curFrame.cf3;
			}
			if(std::isfinite(oldFrameRequest->endPf1)) {
				curFrame.pf1 += alpha * (oldFrameRequest->endPf1 - curFrame.pf1);
				oldFrameRequest->frame.pf1 = curFrame.pf1;
			}
			if(std::isfinite(oldFrameRequest->endPf2)) {
				curFrame.pf2 += alpha * (oldFrameRequest->endPf2 - curFrame.pf2);
				oldFrameRequest->frame.pf2 = curFrame.pf2;
			}
			if(std::isfinite(oldFrameRequest->endPf3)) {
				curFrame.pf3 += alpha * (oldFrameRequest->endPf3 - curFrame.pf3);
				oldFrameRequest->frame.pf3 = curFrame.pf3;
			}
		}
	}


	public:

	FrameManagerImpl(int sampleRateHz): curFrame(), curFrameEx(), curFrameIsNULL(true), curHasFrameEx(false), sampleCounter(0), newFrameRequest(NULL), lastUserIndex(-1), purgeFlag(false), sampleRate(sampleRateHz)  {
		// speechPlayer_frame_t is a plain C struct; ensure it starts from a known state.
		memset(&curFrame, 0, sizeof(speechPlayer_frame_t));
		curFrameEx = speechPlayer_frameEx_defaults;

		oldFrameRequest=new frameRequest_t();
		oldFrameRequest->minNumSamples=0;
		oldFrameRequest->numFadeSamples=0;
		oldFrameRequest->NULLFrame=true;
		oldFrameRequest->hasFrameEx=false;
		memset(&(oldFrameRequest->frame), 0, sizeof(speechPlayer_frame_t));
		oldFrameRequest->frameEx = speechPlayer_frameEx_defaults;
		oldFrameRequest->voicePitchInc=0;
		oldFrameRequest->endCf1=NAN;
		oldFrameRequest->endCf2=NAN;
		oldFrameRequest->endCf3=NAN;
		oldFrameRequest->endPf1=NAN;
		oldFrameRequest->endPf2=NAN;
		oldFrameRequest->endPf3=NAN;
		oldFrameRequest->formantAlpha=0;
		oldFrameRequest->userIndex=-1;
	}

	void queueFrame(speechPlayer_frame_t* frame, unsigned int minNumSamples, unsigned int numFadeSamples, int userIndex, bool purgeQueue) override {
		queueFrameEx(frame, NULL, 0, minNumSamples, numFadeSamples, userIndex, purgeQueue);
	}

	void queueFrameEx(speechPlayer_frame_t* frame, const speechPlayer_frameEx_t* frameEx, unsigned int frameExSize, unsigned int minNumSamples, unsigned int numFadeSamples, int userIndex, bool purgeQueue) override {
		frameLock.acquire();
		frameRequest_t* frameRequest=new frameRequest_t;
		frameRequest->minNumSamples=minNumSamples;
		// Enforce minimum of 1 to prevent divide-by-zero in updateCurrentFrame().
		// This makes the class self-protecting rather than relying on callers.
		frameRequest->numFadeSamples=numFadeSamples > 0 ? numFadeSamples : 1;
		if(frame) {
			frameRequest->NULLFrame=false;
			memcpy(&(frameRequest->frame),frame,sizeof(speechPlayer_frame_t));
			frameRequest->voicePitchInc=(frameRequest->minNumSamples>0)?((frame->endVoicePitch-frame->voicePitch)/frameRequest->minNumSamples):0;
		} else {
			frameRequest->NULLFrame=true;
			memset(&(frameRequest->frame), 0, sizeof(speechPlayer_frame_t));
			frameRequest->voicePitchInc=0;
		}
		
		// Initialize formant end targets to NAN (no ramping by default)
		frameRequest->endCf1 = NAN;
		frameRequest->endCf2 = NAN;
		frameRequest->endCf3 = NAN;
		frameRequest->endPf1 = NAN;
		frameRequest->endPf2 = NAN;
		frameRequest->endPf3 = NAN;
		frameRequest->formantAlpha = 0.0;

		// Copy frameEx safely: start with defaults, then overlay caller's data.
		// This allows older callers with smaller structs to work with newer DLLs,
		// and ensures new parameters get sensible defaults (not just zeros).
		if(frameEx && frameExSize > 0) {
			frameRequest->hasFrameEx=true;
			frameRequest->frameEx = speechPlayer_frameEx_defaults;
			unsigned int copySize = frameExSize < sizeof(speechPlayer_frameEx_t) ? frameExSize : sizeof(speechPlayer_frameEx_t);
			memcpy(&(frameRequest->frameEx), frameEx, copySize);
			
			// Store formant end targets for exponential smoothing
			// Tau of ~10ms gives smooth articulatory movement that mimics real speech
			// At 22050 Hz: tau=220 samples; at 44100 Hz: tau=440 samples
			// We use a fixed alpha that works well across sample rates
			const double kFormantAlpha = 0.004;  // ~10-15ms time constant
			
			bool hasAnyFormantTarget = false;
			if(std::isfinite(frameRequest->frameEx.endCf1)) {
				frameRequest->endCf1 = frameRequest->frameEx.endCf1;
				hasAnyFormantTarget = true;
			}
			if(std::isfinite(frameRequest->frameEx.endCf2)) {
				frameRequest->endCf2 = frameRequest->frameEx.endCf2;
				hasAnyFormantTarget = true;
			}
			if(std::isfinite(frameRequest->frameEx.endCf3)) {
				frameRequest->endCf3 = frameRequest->frameEx.endCf3;
				hasAnyFormantTarget = true;
			}
			if(std::isfinite(frameRequest->frameEx.endPf1)) {
				frameRequest->endPf1 = frameRequest->frameEx.endPf1;
				hasAnyFormantTarget = true;
			}
			if(std::isfinite(frameRequest->frameEx.endPf2)) {
				frameRequest->endPf2 = frameRequest->frameEx.endPf2;
				hasAnyFormantTarget = true;
			}
			if(std::isfinite(frameRequest->frameEx.endPf3)) {
				frameRequest->endPf3 = frameRequest->frameEx.endPf3;
				hasAnyFormantTarget = true;
			}
			
			if(hasAnyFormantTarget) {
				// Scale alpha so formant ramps reach their target even in short
				// (high-speed) frames.  At normal speed (~2200 samples for a vowel),
				// kFormantAlpha gives a ~250-sample time constant — plenty of time.
				// At 3x speed (~700 samples), the same alpha covers only 1/3 of
				// the trajectory.  We scale alpha up so the effective time constant
				// never exceeds 1/4 of the frame duration.
				double alpha = kFormantAlpha;
				if (minNumSamples > 0) {
					const double maxTau = minNumSamples * 0.10;  // settle within 10% of frame
					const double baseTau = 1.0 / kFormantAlpha;  // ~250 samples
					if (baseTau > maxTau && maxTau > 0.0) {
						alpha = 1.0 / maxTau;
					}
				}
				frameRequest->formantAlpha = alpha;
			}
		} else {
			frameRequest->hasFrameEx=false;
			frameRequest->frameEx = speechPlayer_frameEx_defaults;
		}

		frameRequest->userIndex=userIndex;
		if(purgeQueue) {
			for(;!frameRequestQueue.empty();frameRequestQueue.pop()) delete frameRequestQueue.front();
			sampleCounter=oldFrameRequest->minNumSamples;
			// Mark as coming from silence so the next frame triggers the from-silence
			// transition path (line 253): new frame's formants/pitch used on BOTH sides
			// of the crossfade, with gain ramping from zero.  Without this, the old
			// curFrame snapshot (which may carry low pitch from an utterance-final
			// declination) bleeds into the crossfade, exciting freshly-reset resonators
			// at a sweeping pitch and producing audible shimmer on the new utterance.
			oldFrameRequest->NULLFrame=true;
			curFrameIsNULL=true;
			if(newFrameRequest) {
				delete newFrameRequest;
				newFrameRequest=NULL;
			}
			purgeFlag = true;  // Signal to wave generator that a purge happened
		}
		frameRequestQueue.push(frameRequest);
		frameLock.release();
	}

	const int getLastIndex() override {
		return lastUserIndex;
	}

	bool checkAndClearPurgeFlag() override {
		frameLock.acquire();
		bool wasPurged = purgeFlag;
		purgeFlag = false;
		frameLock.release();
		return wasPurged;
	}

	const speechPlayer_frame_t* const getCurrentFrameWithEx(const speechPlayer_frameEx_t** outFrameEx) override {
		frameLock.acquire();
		updateCurrentFrame();
		if(outFrameEx) {
			if(curFrameIsNULL || !curHasFrameEx) *outFrameEx=NULL;
			else *outFrameEx=&curFrameEx;
		}
		frameLock.release();
		return curFrameIsNULL?NULL:&curFrame;
	}

	~FrameManagerImpl() override {
		// Acquire lock during teardown to ensure audio thread isn't mid-read.
		// Caller should have stopped audio callbacks first, but this is defensive.
		frameLock.acquire();
		if(oldFrameRequest) delete oldFrameRequest;
		if(newFrameRequest) delete newFrameRequest;
		while(!frameRequestQueue.empty()) {
			delete frameRequestQueue.front();
			frameRequestQueue.pop();
		}
		frameLock.release();
	}

};

FrameManager* FrameManager::create(int sampleRate) { return new FrameManagerImpl(sampleRate); }