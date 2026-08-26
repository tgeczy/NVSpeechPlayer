/*
TGSpeechBox — Token-to-FrameEx conversion and emission.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

// frame_emit.cpp - TGSpeechBox — Frame emission for frontend
//
// Extracted from ipa_engine.cpp to reduce file size.
// Contains generateAcousticEvents() template and thin emitFrames()/emitFramesEx() wrappers.

#include "ipa_engine.h"
#include "passes/pass_common.h"
#include "utf8.h"
#include "../utils.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <type_traits>

// Debug logging for frame emission investigation.
// Set to 1 to enable, 0 to disable.
#define FEMIT_DEBUG_LOG 0
#if FEMIT_DEBUG_LOG
#include <cstdio>
#include <cstdlib>
static FILE* femitLogFile() {
  static FILE* f = nullptr;
  if (!f) {
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = std::getenv("TMP");
    if (!tmp) tmp = "/tmp";
    std::string path = std::string(tmp) + "/tgsb_frame_emit.log";
    f = std::fopen(path.c_str(), "a");
  }
  return f;
}
#define FELOG(...) do { FILE* _f = femitLogFile(); if (_f) { std::fprintf(_f, __VA_ARGS__); std::fflush(_f); } } while(0)
#else
#define FELOG(...) ((void)0)
#endif

namespace nvsp_frontend {

// Helper to clamp a value to [0, 1]
static inline double clamp01(double v) {
  if (v < 0.0) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

// Helper to clamp sharpness multiplier to reasonable range
static inline double clampSharpness(double v) {
  if (v < 0.1) return 0.1;
  if (v > 5.0) return 5.0;
  return v;
}

// Adaptive onset hold for diphthong glides.
//
// The base onsetHoldExponent (e.g. 1.3) works well for narrow sweeps
// like FACE /eɪ/ (~300 Hz F2 delta), but crushes the offglide on wide
// sweeps like PRICE /ɑɪ/ (~600+ Hz F2 delta).  This function scales
// the exponent down for wide sweeps and enforces a minimum offglide
// duration so the diphthong identity isn't lost perceptually.
//
// References: Gay 1968, Lindblom & Studdert-Kennedy 1967 — ~40-50 ms
// of audible formant motion needed for diphthong offglide perception.
static double adaptiveOnsetHold(
    double baseHold,
    double startCf1, double endCf1,
    double startCf2, double endCf2,
    double totalDurMs,
    const Token* nextToken)  // nullptr if no following token
{
  double hold = baseHold;
  if (hold <= 1.0) return hold;

  // 1. Sweep-width scaling: wider sweeps need earlier motion onset.
  //    F2 is the primary perceptual cue for diphthong identity.
  const double sweepF2 = fabs(endCf2 - startCf2);
  const double sweepF1 = fabs(endCf1 - startCf1);
  const double sweepMax = std::max(sweepF2, sweepF1);

  // Narrow (<300 Hz): full hold.  Wide (>600 Hz): hold → floor.
  // Floor of 1.12 ensures wide diphthongs still linger briefly at onset
  // so the open vowel quality registers before the sweep begins — without
  // this, PRICE /aɪ/ sounds "chesty" at rates above ~70 because the F1/F2
  // sweep starts immediately and the onset never settles.
  constexpr double kMinHold = 1.12;
  if (sweepMax > 300.0) {
    double widthFrac = std::min((sweepMax - 300.0) / 300.0, 1.0);
    hold = hold + (kMinHold - hold) * widthFrac;
  }

  // 2. Following-context check: stops, glottal stops, and silence give
  //    zero coarticulatory runway — the offglide must complete in-token.
  bool nextIsAbrupt = false;
  if (!nextToken || nextToken->silence || nextToken->preStopGap) {
    nextIsAbrupt = true;
  } else if (nextToken->def &&
             (nextToken->def->flags & kIsStop)) {
    nextIsAbrupt = true;
  }

  // 3. Minimum offglide duration: ~40 ms of formant motion needed for
  //    the ear to register the diphthong.  If the hold eats too much
  //    time, reduce it so the sweep portion meets the floor.
  const double minOffglideMs = 40.0;
  if (totalDurMs > minOffglideMs && hold > 1.0 &&
      (nextIsAbrupt || sweepMax > 400.0)) {
    // With pow(frac, hold), meaningful sweep starts around frac=0.15^(1/hold).
    double sweepStart = pow(0.15, 1.0 / std::max(hold, 1.001));
    double effectiveSweepMs = totalDurMs * (1.0 - sweepStart);
    if (effectiveSweepMs < minOffglideMs) {
      // Back-solve: what exponent gives us minOffglideMs of sweep?
      double targetStart = 1.0 - (minOffglideMs / totalDurMs);
      if (targetStart > 0.01 && targetStart < 0.99) {
        hold = log(0.15) / log(targetStart);
        if (hold < 1.0) hold = 1.0;
      }
    }
  }

  return hold;
}

// =================================================================
// generateAcousticEvents — unified frame emission pipeline
// =================================================================
// Templated on the emitter callable so both legacy (Frame-only) and
// modern (Frame + FrameEx) callbacks share identical acoustic math.
// FrameEx fields are always computed (the cost is negligible); the
// emitter decides whether to forward them to the DSP.
template <typename Emitter>
static void generateAcousticEvents(
  const PackSet& pack,
  const std::vector<Token>& tokens,
  int userIndexBase,
  double speed,
  const nvspFrontend_FrameEx& frameExDefaults,
  TrajectoryState* trajectoryState,
  Emitter rawEmitFn,
  std::vector<tgsb_data::FrameTraceEntry>* traceSink = nullptr,
  const int* traceFrameCounter = nullptr
) {
  // Transitions are articulatory events, not rate-elastic ones: the Klatt
  // lineage keeps transition spans in absolute time (~130 ms max) instead
  // of scaling them with 1/speed. Below-normal speeds otherwise stretch
  // fades to 150-360 ms, smearing every segment's onset across its
  // neighbor. Slow speech = longer steady states, not slower articulators.
  // Speeds >= 1.0 are bit-identical (fast-side fades already shrink).
  constexpr double kSlowRateFadeCapMs = 130.0;
  auto emitFn = [&](const nvspFrontend_Frame* f, const nvspFrontend_FrameEx* fEx,
                    double durationMs, double fadeMs) {
    if (speed < 1.0 && fadeMs > kSlowRateFadeCapMs) fadeMs = kSlowRateFadeCapMs;
    rawEmitFn(f, fEx, durationMs, fadeMs);
  };

  // Compile-time layout guarantees
  static_assert(sizeof(nvspFrontend_Frame) == sizeof(double) * kFrameFieldCount,
                "nvspFrontend_Frame must remain exactly kFrameFieldCount doubles with no padding");
  static_assert(std::is_standard_layout<nvspFrontend_Frame>::value,
                "nvspFrontend_Frame must remain standard-layout");
  static_assert(std::is_trivially_copyable<nvspFrontend_Frame>::value,
                "nvspFrontend_Frame must remain trivially copyable");

  const bool trillEnabled = (pack.lang.trillModulationMs > 0.0);

  const int vp = static_cast<int>(FieldId::voicePitch);
  const int evp = static_cast<int>(FieldId::endVoicePitch);
  const int va = static_cast<int>(FieldId::voiceAmplitude);
  const int fa = static_cast<int>(FieldId::fricationAmplitude);

  // Trill modulation constants
  constexpr double kTrillCloseFactor = 0.0;   // full gating — square wave amplitude envelope
  constexpr double kTrillCloseFrac = 0.50;    // 50% duty cycle
  constexpr double kTrillFricFloor = 0.12;
  constexpr double kMinPhaseMs = 0.25;

  // Trajectory limiting state (per-handle, reset at utterance start)
  const LanguagePack& lang = pack.lang;
  trajectoryState->hasPrevFrame = false;
  trajectoryState->hasPrevBase = false;
  trajectoryState->hasPrevFrameEx = false;

  // Track whether we've emitted at least one real frame
  bool hadPrevFrame = false;
  bool prevTokenWasStop = false;
  bool prevTokenWasTap = false;
  const double wbDipMs = lang.wordBoundaryDipMs;

  for (const Token& t : tokens) {
    // Phoneme-boundary trace: one entry per token, captures frame index of
    // this phoneme's first emission. Silence/gap tokens (def==nullptr) are
    // skipped so callers see only real phonemes.
    if (traceSink && traceFrameCounter && t.def) {
      traceSink->push_back({*traceFrameCounter, u32ToUtf8(t.def->key)});
    }

    // ============================================
    // VOICE BAR EMISSION (voiced stop closures)
    // ============================================
    if (t.voicedClosure && hadPrevFrame) {
      double vbFadeMs = t.fadeMs;
      if (vbFadeMs < 8.0) vbFadeMs = 8.0;

      if (trajectoryState->hasPrevBase) {
        double vb[kFrameFieldCount];
        std::memcpy(vb, trajectoryState->prevBase, sizeof(vb));

        double vbAmp = (t.def && t.def->hasVoiceBarAmplitude) ? t.def->voiceBarAmplitude : 0.3;
        double vbF1 = (t.def && t.def->hasVoiceBarF1) ? t.def->voiceBarF1 : 150.0;

        vb[va] = vbAmp;
        vb[fa] = 0.0;
        vb[static_cast<int>(FieldId::aspirationAmplitude)] = 0.0;
        vb[static_cast<int>(FieldId::cf1)] = vbF1;
        vb[static_cast<int>(FieldId::pf1)] = vbF1;
        vb[static_cast<int>(FieldId::preFormantGain)] = vbAmp;

        // Pre-position F2/F3 at the stop's own formants.
        if (t.def) {
          auto vbField = [&](FieldId id) -> double {
            int idx = static_cast<int>(id);
            return (t.def->setMask & (1ULL << idx)) ? t.def->field[idx] : 0.0;
          };
          double sCf2 = vbField(FieldId::cf2);
          double sCf3 = vbField(FieldId::cf3);
          double sPf2 = vbField(FieldId::pf2);
          double sPf3 = vbField(FieldId::pf3);
          if (sCf2 > 0.0) vb[static_cast<int>(FieldId::cf2)] = sCf2;
          if (sCf3 > 0.0) vb[static_cast<int>(FieldId::cf3)] = sCf3;
          if (sPf2 > 0.0) vb[static_cast<int>(FieldId::pf2)] = sPf2;
          if (sPf3 > 0.0) vb[static_cast<int>(FieldId::pf3)] = sPf3;
        }

        nvspFrontend_Frame vbFrame;
        std::memcpy(&vbFrame, vb, sizeof(vbFrame));

        nvspFrontend_FrameEx vbFrameEx = trajectoryState->hasPrevFrameEx
            ? trajectoryState->prevFrameEx
            : frameExDefaults;
        vbFrameEx.transAmplitudeMode = 1.0;  // equal-power crossfade
        // Keep Fujisaki model running (don't reset IIR state) but don't re-fire commands.
        vbFrameEx.fujisakiPhraseAmp = 0.0;
        vbFrameEx.fujisakiAccentAmp = 0.0;
        vbFrameEx.fujisakiReset = 0.0;
        // Never inherit the previous phoneme's formant ramp targets: stale
        // endCf/endPf steer the closure's spectrum back toward the phoneme
        // it just left (overriding the F2/F3 pre-positioning above), which
        // slow rates stretch into an audible ghost of the prior phoneme.
        vbFrameEx.endCf1 = vbFrameEx.endCf2 = vbFrameEx.endCf3 = NAN;
        vbFrameEx.endPf1 = vbFrameEx.endPf2 = vbFrameEx.endPf3 = NAN;
        emitFn(&vbFrame, &vbFrameEx, t.durationMs, vbFadeMs);
      } else {
        emitFn(nullptr, nullptr, t.durationMs, vbFadeMs);
      }
      continue;
    }

    // ============================================
    // CODA NOISE TAPER (fricative→stop closure)
    // ============================================
    if (t.preStopGap && t.codaFricStopBlend && !t.voicedClosure && hadPrevFrame) {
      if (trajectoryState->hasPrevBase) {
        const double totalDur = t.durationMs;
        const double earlyDur = totalDur * 0.45;
        const double lateDur = totalDur - earlyDur;
        const double prevFric = trajectoryState->prevBase[fa];
        const int aspIdx = static_cast<int>(FieldId::aspirationAmplitude);
        const int pgIdx = static_cast<int>(FieldId::preFormantGain);

        nvspFrontend_FrameEx taperFrameEx = trajectoryState->hasPrevFrameEx
            ? trajectoryState->prevFrameEx
            : frameExDefaults;
        taperFrameEx.fujisakiPhraseAmp = 0.0;
        taperFrameEx.fujisakiAccentAmp = 0.0;
        // Same guard as the voice bar: stale inherited formant ramp targets
        // would override the taper's own place blending below.
        taperFrameEx.endCf1 = taperFrameEx.endCf2 = taperFrameEx.endCf3 = NAN;
        taperFrameEx.endPf1 = taperFrameEx.endPf2 = taperFrameEx.endPf3 = NAN;

        // --- Early taper: sibilant tail ---
        double early[kFrameFieldCount];
        std::memcpy(early, trajectoryState->prevBase, sizeof(early));
        early[va] = 0.0;
        early[fa] = prevFric * lang.codaNoiseTaperEarlyFricScale;
        early[aspIdx] = lang.codaNoiseTaperEarlyAspAmp;
        early[pgIdx] = lang.codaNoiseTaperPreGain;

        nvspFrontend_Frame earlyFrame;
        std::memcpy(&earlyFrame, early, sizeof(earlyFrame));
        emitFn(&earlyFrame, &taperFrameEx, earlyDur, t.fadeMs);
        hadPrevFrame = true;

        // --- Late taper: aspirated transition ---
        double late[kFrameFieldCount];
        std::memcpy(late, trajectoryState->prevBase, sizeof(late));
        late[va] = 0.0;
        late[fa] = prevFric * lang.codaNoiseTaperLateFricScale;
        late[aspIdx] = lang.codaNoiseTaperLateAspAmp;
        late[pgIdx] = lang.codaNoiseTaperPreGain;

        // Blend formants toward the stop's place
        if (t.def) {
          const int cf2i = static_cast<int>(FieldId::cf2);
          const int cf3i = static_cast<int>(FieldId::cf3);
          const int pf2i = static_cast<int>(FieldId::pf2);
          const int pf3i = static_cast<int>(FieldId::pf3);
          constexpr double blend = 0.40;
          if (t.def->setMask & (1ull << cf2i))
            late[cf2i] += (t.def->field[cf2i] - late[cf2i]) * blend;
          if (t.def->setMask & (1ull << cf3i))
            late[cf3i] += (t.def->field[cf3i] - late[cf3i]) * blend;
          if (t.def->setMask & (1ull << pf2i))
            late[pf2i] += (t.def->field[pf2i] - late[pf2i]) * blend;
          if (t.def->setMask & (1ull << pf3i))
            late[pf3i] += (t.def->field[pf3i] - late[pf3i]) * blend;
        }

        nvspFrontend_Frame lateFrame;
        std::memcpy(&lateFrame, late, sizeof(lateFrame));
        double lateFade = std::max(earlyDur * 0.5, lateDur * 0.4);
        emitFn(&lateFrame, &taperFrameEx, lateDur, lateFade);

        continue;
      }
    }

    if (t.silence || !t.def) {
      emitFn(nullptr, nullptr, t.durationMs, t.fadeMs);
      continue;
    }

    // Build base frame
    double base[kFrameFieldCount] = {};
    const std::uint64_t mask = t.setMask;
    for (int f = 0; f < kFrameFieldCount; ++f) {
      if ((mask & (1ull << f)) == 0) continue;
      base[f] = t.field[f];
    }

    // Save full base for voice bar emission on the next voiced closure.
    std::memcpy(trajectoryState->prevBase, base, sizeof(base));
    trajectoryState->hasPrevBase = true;

    // Rate-adaptive bandwidth widening.
    {
      const double hrT = pack.lang.highRateThreshold;
      const double bwF = pack.lang.highRateBandwidthWideningFactor;
      if (hrT > 0.0 && bwF > 1.0 && speed > hrT) {
        const double ceiling = hrT * 1.8;
        const double ramp = std::min((speed - hrT) / (ceiling - hrT), 1.0);
        const double bwScale = 1.0 + ramp * (bwF - 1.0);
        base[static_cast<int>(FieldId::cb1)] *= bwScale;
        base[static_cast<int>(FieldId::cb2)] *= bwScale;
        base[static_cast<int>(FieldId::cb3)] *= bwScale;
      }
    }

    // Build FrameEx by mixing user defaults with per-phoneme values.
    // The mixing formula:
    //   - creakiness, breathiness, jitter, shimmer: additive, clamped to [0,1]
    //   - sharpness: multiplicative (phoneme * user), clamped to reasonable range
    //   - endCf1/2/3, endPf1/2/3: direct values (Hz), NAN if not set
    // Per-phoneme values override only if explicitly set (has* flags).
    // Value-init: every field starts at 0.0. The assignments below cover
    // most fields, but any field they miss (fricationTiltDb was missed for
    // months) otherwise carries stack garbage into EVERY emitted frame —
    // benign where the stack slot happens to hold ~0, catastrophic where it
    // doesn't (Android: 10^(garbage) tilt annihilated fricatives, the
    // issue #100 random-"wiss").
    nvspFrontend_FrameEx frameEx = {};

    // Get per-phoneme values (0 / 1.0 neutral if not set)
    double phonemeCreakiness = (t.def && t.def->hasCreakiness) ? t.def->creakiness : 0.0;
    double phonemeBreathiness = (t.def && t.def->hasBreathiness) ? t.def->breathiness : 0.0;
    double phonemeJitter = (t.def && t.def->hasJitter) ? t.def->jitter : 0.0;
    double phonemeShimmer = (t.def && t.def->hasShimmer) ? t.def->shimmer : 0.0;
    double phonemeSharpness = (t.def && t.def->hasSharpness) ? t.def->sharpness : 1.0;
    
    // Phoneme can only BOOST sharpness, never dull it - this ensures the user's
    // configured sharpness is never reduced by per-phoneme values. A phoneme
    // wanting "less sharp" would actually make it less distinct from neighbors.
    if (phonemeSharpness < 1.0) phonemeSharpness = 1.0;
    
    frameEx.creakiness = clamp01(phonemeCreakiness + frameExDefaults.creakiness);
    double tokenBreathiness = t.hasTokenBreathiness ? t.tokenBreathiness : 0.0;
    frameEx.breathiness = clamp01(phonemeBreathiness + tokenBreathiness + frameExDefaults.breathiness);
    frameEx.jitter = clamp01(phonemeJitter + frameExDefaults.jitter);
    frameEx.shimmer = clamp01(phonemeShimmer + frameExDefaults.shimmer);

    double userSharpness = (frameExDefaults.sharpness > 0.0) ? frameExDefaults.sharpness : 1.0;
    frameEx.sharpness = clampSharpness(phonemeSharpness * userSharpness);    
    // Formant end targets: phoneme-level (deliberate sweep) takes priority,
    // then token-level (from coarticulation), otherwise NAN (no ramping).
    // Phoneme-level endCf encodes within-segment formant movement (e.g.
    // diphthongized GOOSE).  Coarticulation endCf blends toward the next
    // consonant's locus — useful default but must not override intentional
    // phoneme sweeps.
    frameEx.endCf1 = (t.def && t.def->hasEndCf1) ? t.def->endCf1 :
                     t.hasEndCf1 ? t.endCf1 : NAN;
    frameEx.endCf2 = (t.def && t.def->hasEndCf2) ? t.def->endCf2 :
                     t.hasEndCf2 ? t.endCf2 : NAN;
    frameEx.endCf3 = (t.def && t.def->hasEndCf3) ? t.def->endCf3 :
                     t.hasEndCf3 ? t.endCf3 : NAN;
    frameEx.endPf1 = (t.def && t.def->hasEndPf1) ? t.def->endPf1 :
                     t.hasEndPf1 ? t.endPf1 :
                     (t.def && t.def->hasEndCf1) ? t.def->endCf1 :
                     t.hasEndCf1 ? t.endCf1 : NAN;
    frameEx.endPf2 = (t.def && t.def->hasEndPf2) ? t.def->endPf2 :
                     t.hasEndPf2 ? t.endPf2 :
                     (t.def && t.def->hasEndCf2) ? t.def->endCf2 :
                     t.hasEndCf2 ? t.endCf2 : NAN;
    frameEx.endPf3 = (t.def && t.def->hasEndPf3) ? t.def->endPf3 :
                     t.hasEndPf3 ? t.endPf3 :
                     (t.def && t.def->hasEndCf3) ? t.def->endCf3 :
                     t.hasEndCf3 ? t.endCf3 : NAN;

    // Per-parameter transition speed scales (set by boundary_smoothing pass).
    frameEx.transF1Scale = t.transF1Scale;
    frameEx.transF2Scale = t.transF2Scale;
    frameEx.transF3Scale = t.transF3Scale;
    frameEx.transNasalScale = t.transNasalScale;
    frameEx.transSourceHoldRatio = t.transSourceHoldRatio;
    frameEx.transVoicingHoldRatio = t.transVoicingHoldRatio;

    // Detect source transitions for equal-power amplitude crossfade.
    // When voicing source type changes (voiced→voiceless or vice versa),
    // linear crossfade creates an energy dip. Equal-power fixes this.
    // We check the FRAME values (not token flags) because that's what
    // the DSP actually interpolates between.
    {
      double curVA = base[va];   // voiceAmplitude of this frame
      double curFA = base[fa];   // fricationAmplitude of this frame
      bool curVoiced   = (curVA > 0.05);
      bool curFricated = (curFA > 0.05);
      if (trajectoryState->hasPrevFrame) {
        bool prevVoiced   = (trajectoryState->prevVoiceAmp > 0.05);
        bool prevFricated = (trajectoryState->prevFricAmp > 0.05);
        bool sourceChange = (prevVoiced != curVoiced) ||
                            (prevFricated != curFricated);
        frameEx.transAmplitudeMode = sourceChange ? 1.0 : 0.0;
      } else {
        frameEx.transAmplitudeMode = 0.0;
      }
    }

    // Higher cascade formants F7/F8 (DSP v8).
    // Per-phoneme override if set, otherwise Rabiner 1968 defaults.
    frameEx.cf7 = (t.def && t.def->hasCf7) ? t.def->cf7 : 6500.0;
    frameEx.cb7 = (t.def && t.def->hasCb7) ? t.def->cb7 : 720.0;
    frameEx.cf8 = (t.def && t.def->hasCf8) ? t.def->cf8 : 7500.0;
    frameEx.cb8 = (t.def && t.def->hasCb8) ? t.def->cb8 : 1250.0;

    // Fujisaki pitch model parameters (set by applyPitchFujisaki)
    // These pass phrase/accent commands to the DSP for natural prosody contours.
    frameEx.fujisakiEnabled = t.fujisakiEnabled ? 1.0 : 0.0;
    frameEx.fujisakiReset = t.fujisakiReset ? 1.0 : 0.0;
    frameEx.fujisakiPhraseAmp = t.fujisakiPhraseAmp;
    frameEx.fujisakiPhraseLen = lang.fujisakiPhraseLen;  // 0 = DSP default
    frameEx.fujisakiAccentAmp = t.fujisakiAccentAmp;
    // Per-token accent duration: scale by monosyllable shortening factor.
    frameEx.fujisakiAccentDur = lang.fujisakiAccentDur * t.fujisakiAccentDurScale;
    frameEx.fujisakiAccentLen = lang.fujisakiAccentLen;  // 0 = DSP default

    // Save frameEx for voice bar emission (keeps Fujisaki model alive during closures).
    trajectoryState->prevFrameEx = frameEx;
    trajectoryState->hasPrevFrameEx = true;

    // ============================================
    // DIPHTHONG GLIDE MICRO-FRAME EMISSION
    // ============================================
    // Collapsed diphthongs emit N cosine-smoothed micro-frames that sweep
    // formants from onset targets (token's base field[]) to offset targets
    // (endCf1/2/3).  Each micro-frame's endCf/endPf points to the NEXT
    // frame's formant values, so the DSP's per-sample exponential smoothing
    // glides between nearby waypoints.  This avoids:
    //   - phasing (audio crossfade between distant formant configs)
    //   - bandwidth smearing (sweeping across full range kills resonator Q)
    // The last micro-frame gets endCf=NAN (hold at offset target).
    if (t.isDiphthongGlide && t.durationMs > 0.0) {
      const double totalDur = t.durationMs;

      // Number of micro-frames scales with duration.
      // Minimum 5: with N=3, onset hold concentrates waypoints near the
      // onset, leaving only one big jump to the offset — losing the glide.
      // N=5 guarantees enough interior points for a smooth sweep curve.
      // At higher pitch, fewer harmonics per formant bandwidth makes
      // crossfade phasing more audible — tighten the interval.
      double intervalMs = lang.diphthongMicroFrameIntervalMs;
      const double pitch0 = base[vp];
      const double pitchEnd = base[evp];
      // Use the LOWEST pitch across the diphthong for interval floor.
      // Sentence-final declination can drop pitch from 109→76 Hz;
      // onset pitch alone would allow frames too short for the
      // resonator to settle at the offglide's lower pitch.
      double pitchForFloor = pitch0;
      if (pitchEnd > 0.0 && pitchEnd < pitchForFloor)
        pitchForFloor = pitchEnd;
      if (pitchForFloor > 0.0) {
        // Scale interval proportionally to pitch period.
        intervalMs *= (100.0 / pitchForFloor);
        // Floor: at least 2 pitch periods per micro-frame for resonator settling.
        double minInterval = 2000.0 / pitchForFloor;
        if (intervalMs < minInterval) intervalMs = minInterval;
        if (intervalMs < 3.0) intervalMs = 3.0;
      }
      // Start and end formant targets (Hz).
      const double startCf1 = base[static_cast<int>(FieldId::cf1)];
      const double startCf2 = base[static_cast<int>(FieldId::cf2)];
      const double startCf3 = base[static_cast<int>(FieldId::cf3)];
      const double startPf1 = base[static_cast<int>(FieldId::pf1)];
      const double startPf2 = base[static_cast<int>(FieldId::pf2)];
      const double startPf3 = base[static_cast<int>(FieldId::pf3)];

      // Start bandwidths (from onset vowel's base frame).
      const double startCb1 = base[static_cast<int>(FieldId::cb1)];
      const double startCb2 = base[static_cast<int>(FieldId::cb2)];
      const double startCb3 = base[static_cast<int>(FieldId::cb3)];
      const double startPb1 = base[static_cast<int>(FieldId::pb1)];
      const double startPb2 = base[static_cast<int>(FieldId::pb2)];
      const double startPb3 = base[static_cast<int>(FieldId::pb3)];

      const double endCascF1 = nvsp_isnan(frameEx.endCf1) ? startCf1 : frameEx.endCf1;
      const double endCascF2 = nvsp_isnan(frameEx.endCf2) ? startCf2 : frameEx.endCf2;
      const double endCascF3 = nvsp_isnan(frameEx.endCf3) ? startCf3 : frameEx.endCf3;

      const double endParF1 = (t.hasEndPf1 && !nvsp_isnan(frameEx.endPf1))
                              ? frameEx.endPf1 : endCascF1;
      const double endParF2 = (t.hasEndPf2 && !nvsp_isnan(frameEx.endPf2))
                              ? frameEx.endPf2 : endCascF2;
      const double endParF3 = (t.hasEndPf3 && !nvsp_isnan(frameEx.endPf3))
                              ? frameEx.endPf3 : endCascF3;

      // End bandwidths: use offset vowel's values if set, else hold onset.
      const double endCascB1 = t.hasEndCb1 ? t.endCb1 : startCb1;
      const double endCascB2 = t.hasEndCb2 ? t.endCb2 : startCb2;
      const double endCascB3 = t.hasEndCb3 ? t.endCb3 : startCb3;
      const double endParB1 = t.hasEndPb1 ? t.endPb1 : startPb1;
      const double endParB2 = t.hasEndPb2 ? t.endPb2 : startPb2;
      const double endParB3 = t.hasEndPb3 ? t.endPb3 : startPb3;

      // Number of micro-frames scales with duration so each step stays
      // a consistent length regardless of speech rate.  At fast rates
      // N stays at minimum 5 (the staircase sweet spot for shimmer).
      // At slow rates N scales up — more steps with smaller formant
      // jumps.  Matches classic Klatt-style fixed-rate staircasing: slow
      // speech just gets more frames, not bigger jumps.
      static constexpr int kMaxFrames = 64;
      int N = std::clamp(static_cast<int>(std::round(totalDur / intervalMs)), 5, kMaxFrames);

      const double startPitch = base[vp];
      const double endPitch = base[evp];
      const double pitchDelta = endPitch - startPitch;
      const double dipFactor = lang.diphthongAmplitudeDipFactor;
      const double baseSegDur = totalDur / N;

      // Adaptive onset hold: scale exponent based on sweep width and
      // following context.  Wide diphthongs (PRICE) get reduced hold;
      // narrow ones (FACE) keep the full base exponent.
      const Token* nextTok = (&t < &tokens.back()) ? (&t + 1) : nullptr;
      const double onsetHold = adaptiveOnsetHold(
          lang.diphthongOnsetHoldExponent,
          startCf1, endCascF1, startCf2, endCascF2,
          totalDur, nextTok);

      FELOG("DIPH-EX speed=%.2f dur=%.1fms N=%d intv=%.1fms hold=%.2f "
            "cf1=%.0f->%.0f cf2=%.0f->%.0f cf3=%.0f->%.0f "
            "pitch=%.1f->%.1f fade=%.1fms\n",
            speed, totalDur, N, intervalMs, onsetHold,
            startCf1, endCascF1, startCf2, endCascF2,
            startCf3, endCascF3,
            startPitch, endPitch, t.fadeMs);

      // Onset settle: give the first micro-frame extra time so the
      // resonator can settle from the preceding consonant before the
      // formant sweep begins.  Borrowed from remaining segments.
      double seg0Dur = baseSegDur;
      double otherSegDur = baseSegDur;
      const double settleMs = lang.diphthongOnsetSettleMs;
      if (settleMs > 0.0 && N > 1) {
        seg0Dur = std::min(baseSegDur + settleMs, totalDur * 0.5);
        otherSegDur = (totalDur - seg0Dur) / (N - 1);
      }

      // Pre-compute all N waypoints so each frame can reference the next.
      // 6 formants per waypoint: cf1,cf2,cf3,pf1,pf2,pf3
      double wpCf1[kMaxFrames], wpCf2[kMaxFrames], wpCf3[kMaxFrames];
      double wpPf1[kMaxFrames], wpPf2[kMaxFrames], wpPf3[kMaxFrames];
      double wpCb1[kMaxFrames], wpCb2[kMaxFrames], wpCb3[kMaxFrames];
      double wpPb1[kMaxFrames], wpPb2[kMaxFrames], wpPb3[kMaxFrames];

      for (int seg = 0; seg < N; ++seg) {
        double frac = (N > 1) ? (static_cast<double>(seg) / (N - 1)) : 0.0;
        if (onsetHold > 1.0) frac = pow(frac, onsetHold);
        double s = cosineSmooth(frac);

        wpCf1[seg] = calculateFreqAtFadePosition(startCf1, endCascF1, s);
        wpCf2[seg] = calculateFreqAtFadePosition(startCf2, endCascF2, s);
        wpCf3[seg] = calculateFreqAtFadePosition(startCf3, endCascF3, s);
        wpPf1[seg] = calculateFreqAtFadePosition(startPf1, endParF1, s);
        wpPf2[seg] = calculateFreqAtFadePosition(startPf2, endParF2, s);
        wpPf3[seg] = calculateFreqAtFadePosition(startPf3, endParF3, s);
        wpCb1[seg] = calculateFreqAtFadePosition(startCb1, endCascB1, s);
        wpCb2[seg] = calculateFreqAtFadePosition(startCb2, endCascB2, s);
        wpCb3[seg] = calculateFreqAtFadePosition(startCb3, endCascB3, s);
        wpPb1[seg] = calculateFreqAtFadePosition(startPb1, endParB1, s);
        wpPb2[seg] = calculateFreqAtFadePosition(startPb2, endParB2, s);
        wpPb3[seg] = calculateFreqAtFadePosition(startPb3, endParB3, s);

        FELOG("  wp[%d] frac=%.3f s=%.3f cf1=%.0f cf2=%.0f cf3=%.0f\n",
              seg, (N > 1) ? (static_cast<double>(seg) / (N - 1)) : 0.0,
              s, wpCf1[seg], wpCf2[seg], wpCf3[seg]);
      }

      // Emit micro-frames with endCf pointing to the next waypoint.
      for (int seg = 0; seg < N; ++seg) {
        double mf[kFrameFieldCount];
        std::memcpy(mf, base, sizeof(mf));

        mf[static_cast<int>(FieldId::cf1)] = wpCf1[seg];
        mf[static_cast<int>(FieldId::cf2)] = wpCf2[seg];
        mf[static_cast<int>(FieldId::cf3)] = wpCf3[seg];
        mf[static_cast<int>(FieldId::pf1)] = wpPf1[seg];
        mf[static_cast<int>(FieldId::pf2)] = wpPf2[seg];
        mf[static_cast<int>(FieldId::pf3)] = wpPf3[seg];
        mf[static_cast<int>(FieldId::cb1)] = wpCb1[seg];
        mf[static_cast<int>(FieldId::cb2)] = wpCb2[seg];
        mf[static_cast<int>(FieldId::cb3)] = wpCb3[seg];
        mf[static_cast<int>(FieldId::pb1)] = wpPb1[seg];
        mf[static_cast<int>(FieldId::pb2)] = wpPb2[seg];
        mf[static_cast<int>(FieldId::pb3)] = wpPb3[seg];

        // Pitch: linear ramp sliced proportionally.
        double t0 = (N > 1) ? (static_cast<double>(seg) / N) : 0.0;
        double t1 = (N > 1) ? (static_cast<double>(seg + 1) / N) : 1.0;
        mf[vp]  = startPitch + pitchDelta * t0;
        mf[evp] = startPitch + pitchDelta * t1;

        // Slight amplitude dip at midpoint (articulatory gesture)
        double frac = (N > 1) ? (static_cast<double>(seg) / (N - 1)) : 0.0;
        if (dipFactor > 0.0) {
          double ampScale = 1.0 - dipFactor * sin(M_PI * frac);
          mf[va] *= ampScale;
        }

        nvspFrontend_Frame frame;
        std::memcpy(&frame, mf, sizeof(frame));

        // Staircase approach: at normal and fast rates ALL micro-frames
        // hold at their waypoint (endCf=NAN disables per-sample
        // exponential smoothing).  Formants snap during the brief
        // crossfade then stay put, minimizing time spent near
        // harmonic-formant crossings (the shimmer this design killed).
        //
        // Rate-adaptive staircase (#98 half 2): below 1.0x the steps grow
        // long enough to hear individually ("dry", letter "I"), while
        // shimmer loses its bite — more time between pitch cycles.  So at
        // slow rates each interior micro-frame's endCf points partway (or
        // fully, at <=0.5x) toward the NEXT waypoint and the DSP's
        // per-sample glide sweeps formants continuously — smoothing the
        // steps WITHOUT lengthening the audio crossfades (long crossfades
        // overlap two formant spectra and read as an eerie double voice;
        // ear-tested and rejected 2026-08-21).  At >=1.0x this branch is
        // never taken and output stays bit-identical.
        nvspFrontend_FrameEx mfEx = frameEx;
        const double glideBlend = (speed < 1.0)
            ? std::clamp((1.0 - speed) / 0.5, 0.0, 1.0) : 0.0;
        if (glideBlend > 0.0 && seg + 1 < N) {
          mfEx.endCf1 = wpCf1[seg] + (wpCf1[seg + 1] - wpCf1[seg]) * glideBlend;
          mfEx.endCf2 = wpCf2[seg] + (wpCf2[seg + 1] - wpCf2[seg]) * glideBlend;
          mfEx.endCf3 = wpCf3[seg] + (wpCf3[seg + 1] - wpCf3[seg]) * glideBlend;
          mfEx.endPf1 = wpPf1[seg] + (wpPf1[seg + 1] - wpPf1[seg]) * glideBlend;
          mfEx.endPf2 = wpPf2[seg] + (wpPf2[seg + 1] - wpPf2[seg]) * glideBlend;
          mfEx.endPf3 = wpPf3[seg] + (wpPf3[seg + 1] - wpPf3[seg]) * glideBlend;
        } else {
          mfEx.endCf1 = NAN;
          mfEx.endCf2 = NAN;
          mfEx.endCf3 = NAN;
          mfEx.endPf1 = NAN;
          mfEx.endPf2 = NAN;
          mfEx.endPf3 = NAN;
        }

        // Fade: first micro-frame uses token's entry fade.
        // Internal micro-frames use a short snap fade (3ms) so formants
        // jump quickly to the new waypoint then hold — staircase style.
        //
        // After obstruents, onset shimmer is addressed by bandwidth widening
        // below (not by fade capping, which caused clicks).
        //
        const double thisDur = (seg == 0) ? seg0Dur : otherSegDur;
        double fadeIn = (seg == 0) ? t.fadeMs : 3.0;
        if (fadeIn > thisDur) fadeIn = thisDur;

        // After obstruents, apply decaying bandwidth widening across the
        // first few micro-frames, scaled by the diphthong's F2 sweep width.
        // Wide sweeps (MOUTH /aʊ/, 550-750 Hz) need more help settling;
        // narrow sweeps (FACE /eɪ/, ~200 Hz) need little or none.
        // Research-backed bandwidth widening during burst release; we taper
        // across seg0→seg1→seg2 so bandwidths return gradually (no pop).
        const bool afterObstruent = (trajectoryState->hasPrevFrame &&
            trajectoryState->prevVoiceAmp < 0.05);
        if (afterObstruent && seg < 3) {
          const double sweepF2 = fabs(endCascF2 - startCf2);
          // Scale: 0 below 400 Hz, full at 700+ Hz.
          const double sweepScale = std::clamp(
              (sweepF2 - 400.0) / 300.0, 0.0, 1.0);
          if (sweepScale > 0.0) {
            const double decay = 1.0 / (1 << seg);  // 1.0, 0.5, 0.25
            const double s = sweepScale * decay;
            mf[static_cast<int>(FieldId::cb1)] += 200.0 * s;
            mf[static_cast<int>(FieldId::cb2)] += 60.0 * s;
            mf[static_cast<int>(FieldId::cb3)] += 40.0 * s;
            std::memcpy(&frame, mf, sizeof(frame));
          }
        }

        // Don't re-fire Fujisaki commands on internal micro-frames.
        if (seg > 0) {
          mfEx.fujisakiPhraseAmp = 0.0;
          mfEx.fujisakiAccentAmp = 0.0;
          mfEx.fujisakiReset = 0.0;
        }

        FELOG("  emit[%d] dur=%.1fms fade=%.1fms cf1=%.0f cf2=%.0f endCf1=%.0f endCf2=%.0f\n",
              seg, thisDur, fadeIn,
              mf[static_cast<int>(FieldId::cf1)],
              mf[static_cast<int>(FieldId::cf2)],
              nvsp_isnan(mfEx.endCf1) ? -1.0 : mfEx.endCf1,
              nvsp_isnan(mfEx.endCf2) ? -1.0 : mfEx.endCf2);

        emitFn(&frame, &mfEx, thisDur, fadeIn);
        hadPrevFrame = true;
      }

      // Update trajectory state with the final micro-frame's values
      // so the next token's trajectory limiting sees the offset formants.
      trajectoryState->prevCf2 = wpCf2[N - 1];
      trajectoryState->prevCf3 = wpCf3[N - 1];
      trajectoryState->prevPf2 = wpPf2[N - 1];
      trajectoryState->prevPf3 = wpPf3[N - 1];
      trajectoryState->prevVoiceAmp = base[va];
      trajectoryState->prevFricAmp = base[fa];
      trajectoryState->hasPrevFrame = true;
      trajectoryState->prevWasNasal = false;

      prevTokenWasTap = false;
      prevTokenWasStop = false;
      continue;
    }

    // ============================================
    // COARTICULATED VOWEL STEADY-STATE EMISSION (#113)
    // ============================================
    // A coartic-shaped vowel's single frame ramps onset→exit for its WHOLE
    // duration, so the canonical vowel is never rendered (es "dos": /o/
    // swept 1129→1105 Hz, never touching 910 — heard as [ø]).  When the
    // coarticulation pass saved canonical steady targets, emit up to three
    // frames instead: onset transition → steady state → exit transition.
    // Boundary values match at every seam, so the short internal fades
    // cannot phase.  Phoneme-level endCf (deliberate sweeps) and diphthong
    // glides keep their own trajectories and skip this path.
    if (t.hasCoarticSteady && !t.isDiphthongGlide && t.durationMs >= 45.0 &&
        !(trillEnabled && tokenIsTrill(t)) &&
        !(t.def && (t.def->hasEndCf1 || t.def->hasEndCf2 || t.def->hasEndCf3))) {
      const int icf1 = static_cast<int>(FieldId::cf1);
      const int icf2 = static_cast<int>(FieldId::cf2);
      const int icf3 = static_cast<int>(FieldId::cf3);
      const int ipf1 = static_cast<int>(FieldId::pf1);
      const int ipf2 = static_cast<int>(FieldId::pf2);
      const int ipf3 = static_cast<int>(FieldId::pf3);

      const double dur = t.durationMs;
      const double onF1 = base[icf1], onF2 = base[icf2], onF3 = base[icf3];
      const double stF1 = (t.steadyF1 > 0.0) ? t.steadyF1 : onF1;
      const double stF2 = (t.steadyF2 > 0.0) ? t.steadyF2 : onF2;
      const double stF3 = (t.steadyF3 > 0.0) ? t.steadyF3 : onF3;
      const double exF1 = nvsp_isnan(frameEx.endCf1) ? stF1 : frameEx.endCf1;
      const double exF2 = nvsp_isnan(frameEx.endCf2) ? stF2 : frameEx.endCf2;
      const double exF3 = nvsp_isnan(frameEx.endCf3) ? stF3 : frameEx.endCf3;

      // Parallel path follows the cascade deltas: if the coartic pass
      // shifted starts it mirrored cf onto pf, so base pf sits at the
      // onset; carry the same offsets so unset/diverging pf stay coherent.
      const double stPf1 = (base[ipf1] > 0.0) ? base[ipf1] + (stF1 - onF1) : base[ipf1];
      const double stPf2 = (base[ipf2] > 0.0) ? base[ipf2] + (stF2 - onF2) : base[ipf2];
      const double stPf3 = (base[ipf3] > 0.0) ? base[ipf3] + (stF3 - onF3) : base[ipf3];
      const double exPf1 = !nvsp_isnan(frameEx.endPf1) ? frameEx.endPf1
                           : (stPf1 > 0.0 ? stPf1 + (exF1 - stF1) : stPf1);
      const double exPf2 = !nvsp_isnan(frameEx.endPf2) ? frameEx.endPf2
                           : (stPf2 > 0.0 ? stPf2 + (exF2 - stF2) : stPf2);
      const double exPf3 = !nvsp_isnan(frameEx.endPf3) ? frameEx.endPf3
                           : (stPf3 > 0.0 ? stPf3 + (exF3 - stF3) : stPf3);

      const bool needIn = fabs(onF2 - stF2) > 10.0 || fabs(onF1 - stF1) > 8.0 ||
                          fabs(onF3 - stF3) > 12.0;
      const bool needOut = fabs(exF2 - stF2) > 10.0 || fabs(exF1 - stF1) > 8.0 ||
                           fabs(exF3 - stF3) > 12.0;
      if (needIn || needOut) {
        const double tIn = needIn
            ? std::min(lang.coarticulationTransInMs, dur * 0.35) : 0.0;
        const double tOut = needOut
            ? std::min(lang.coarticulationTransOutMs, dur * 0.35) : 0.0;
        const double steadyDur = dur - tIn - tOut;  // >= 0.30*dur by clamps

        const double p0 = base[vp];
        const double pd = base[evp] - p0;

        FELOG("STEADY dur=%.1f tIn=%.1f tOut=%.1f "
              "on=%.0f/%.0f/%.0f st=%.0f/%.0f/%.0f ex=%.0f/%.0f/%.0f\n",
              dur, tIn, tOut, onF1, onF2, onF3, stF1, stF2, stF3,
              exF1, exF2, exF3);

        double pos = 0.0;
        bool firstSeg = true;
        auto emitSeg = [&](double segDur,
                           double a1, double a2, double a3,
                           double ap1, double ap2, double ap3,
                           double e1, double e2, double e3,
                           double ep1, double ep2, double ep3, bool ramp) {
          if (segDur <= 0.5) return;
          double mf[kFrameFieldCount];
          std::memcpy(mf, base, sizeof(mf));
          mf[icf1] = a1; mf[icf2] = a2; mf[icf3] = a3;
          mf[ipf1] = ap1; mf[ipf2] = ap2; mf[ipf3] = ap3;
          mf[vp] = p0 + pd * (pos / dur);
          mf[evp] = p0 + pd * ((pos + segDur) / dur);

          nvspFrontend_Frame frame;
          std::memcpy(&frame, mf, sizeof(frame));

          nvspFrontend_FrameEx segEx = frameEx;
          if (ramp) {
            segEx.endCf1 = e1; segEx.endCf2 = e2; segEx.endCf3 = e3;
            segEx.endPf1 = ep1; segEx.endPf2 = ep2; segEx.endPf3 = ep3;
          } else {
            segEx.endCf1 = NAN; segEx.endCf2 = NAN; segEx.endCf3 = NAN;
            segEx.endPf1 = NAN; segEx.endPf2 = NAN; segEx.endPf3 = NAN;
          }
          if (!firstSeg) {
            segEx.fujisakiPhraseAmp = 0.0;
            segEx.fujisakiAccentAmp = 0.0;
            segEx.fujisakiReset = 0.0;
          }
          double fadeIn = firstSeg ? t.fadeMs : 3.0;
          if (fadeIn > segDur) fadeIn = segDur;

          FELOG("  seg dur=%.1f fade=%.1f cf=%.0f/%.0f end=%.0f/%.0f\n",
                segDur, fadeIn, mf[icf1], mf[icf2],
                nvsp_isnan(segEx.endCf1) ? -1.0 : segEx.endCf1,
                nvsp_isnan(segEx.endCf2) ? -1.0 : segEx.endCf2);

          emitFn(&frame, &segEx, segDur, fadeIn);
          hadPrevFrame = true;
          firstSeg = false;
          pos += segDur;
        };

        if (tIn > 0.0) {
          emitSeg(tIn, onF1, onF2, onF3, base[ipf1], base[ipf2], base[ipf3],
                  stF1, stF2, stF3, stPf1, stPf2, stPf3, true);
        }
        emitSeg(steadyDur, stF1, stF2, stF3, stPf1, stPf2, stPf3,
                0, 0, 0, 0, 0, 0, false);
        if (tOut > 0.0) {
          emitSeg(tOut, stF1, stF2, stF3, stPf1, stPf2, stPf3,
                  exF1, exF2, exF3, exPf1, exPf2, exPf3, true);
        }

        trajectoryState->prevCf2 = needOut ? exF2 : stF2;
        trajectoryState->prevCf3 = needOut ? exF3 : stF3;
        trajectoryState->prevPf2 = needOut ? exPf2 : stPf2;
        trajectoryState->prevPf3 = needOut ? exPf3 : stPf3;
        trajectoryState->prevVoiceAmp = base[va];
        trajectoryState->prevFricAmp = base[fa];
        trajectoryState->hasPrevFrame = true;
        trajectoryState->prevWasNasal = false;

        prevTokenWasTap = false;
        prevTokenWasStop = false;
        continue;
      }
    }

    // Handle trill modulation (simplified version - emits micro-frames)
    if (trillEnabled && tokenIsTrill(t) && t.durationMs > 0.0) {
      double totalDur = t.durationMs;
      constexpr double kFixedTrillCycleMs = 28.0;
      double cycleMs = kFixedTrillCycleMs;
      if (cycleMs > totalDur) cycleMs = totalDur;

      double closeMs = cycleMs * kTrillCloseFrac;
      double openMs = cycleMs - closeMs;

      if (openMs < kMinPhaseMs) {
        openMs = kMinPhaseMs;
        closeMs = std::max(kMinPhaseMs, cycleMs - openMs);
      }
      if (closeMs < kMinPhaseMs) {
        closeMs = kMinPhaseMs;
        openMs = std::max(kMinPhaseMs, cycleMs - closeMs);
      }

      double microFadeMs = 0.25;  // hard edges for percussive trill character

      const bool hasVoiceAmp = ((mask & (1ull << va)) != 0);
      const bool hasFricAmp = ((mask & (1ull << fa)) != 0);
      const double baseVoiceAmp = base[va];
      const double baseFricAmp = base[fa];

      const double startPitch = base[vp];
      const double endPitch = base[evp];
      const double pitchDelta = endPitch - startPitch;

      double remaining = totalDur;
      double pos = 0.0;
      bool highPhase = true;
      bool firstPhase = true;

      while (remaining > 1e-9) {
        double phaseDur = highPhase ? openMs : closeMs;
        if (phaseDur > remaining) phaseDur = remaining;

        double t0 = (totalDur > 0.0) ? (pos / totalDur) : 0.0;
        double t1 = (totalDur > 0.0) ? ((pos + phaseDur) / totalDur) : 1.0;

        double seg[kFrameFieldCount];
        std::memcpy(seg, base, sizeof(seg));

        seg[vp] = startPitch + pitchDelta * t0;
        seg[evp] = startPitch + pitchDelta * t1;

        if (!highPhase) {
          if (hasVoiceAmp) {
            seg[va] = baseVoiceAmp * kTrillCloseFactor;
          }
          if (hasFricAmp && baseFricAmp > 0.0) {
            seg[fa] = std::max(baseFricAmp, kTrillFricFloor);
          }
        }

        nvspFrontend_Frame frame;
        std::memcpy(&frame, seg, sizeof(frame));

        double fadeIn = firstPhase ? t.fadeMs : microFadeMs;
        if (fadeIn <= 0.0) fadeIn = microFadeMs;
        if (fadeIn > phaseDur) fadeIn = phaseDur;

        emitFn(&frame, &frameEx, phaseDur, fadeIn);
        hadPrevFrame = true;

        remaining -= phaseDur;
        pos += phaseDur;
        highPhase = !highPhase;
        firstPhase = false;
      }

      prevTokenWasStop = false;
      continue;
    }

    // ============================================
    // STOP BURST MICRO-FRAME EMISSION
    // ============================================
    {
      const bool isStop = t.def && ((t.def->flags & kIsStop) != 0);
      const bool isAffricate = t.def && ((t.def->flags & kIsAfricate) != 0);

      if ((isStop || isAffricate) && !t.silence && !t.preStopGap &&
          !t.postStopAspiration && !t.voicedClosure && t.durationMs > 1.0) {

        // Word-boundary silence gap before word-initial stops.
        // Stops at high rate can be as short as 4ms — can't steal duration.
        // Instead, INSERT an extra near-silent micro-frame (additive).
        // This gives the ear a boundary cue without shrinking the burst.
        double stopMainDur = t.durationMs;
        double stopMainFade = t.fadeMs;
        if (wbDipMs > 0.0 && t.wordStart && hadPrevFrame) {
          double dip[kFrameFieldCount];
          std::memcpy(dip, base, sizeof(dip));
          dip[va] = 0.0;
          dip[fa] = 0.0;
          dip[static_cast<int>(FieldId::aspirationAmplitude)] = 0.0;
          dip[static_cast<int>(FieldId::preFormantGain)] = 0.0;

          nvspFrontend_Frame dipFrame;
          std::memcpy(&dipFrame, dip, sizeof(dipFrame));
          emitFn(&dipFrame, &frameEx, wbDipMs, 1.0);
          hadPrevFrame = true;
          stopMainFade = 1.0; // short crossfade into burst

          FELOG("WB-GAP-STOP wordStart=%d prevWasStop=%d gapMs=%.1f dur=%.1f\n",
                (int)t.wordStart, (int)prevTokenWasStop, wbDipMs, stopMainDur);
        } else if (isStop || isAffricate) {
          FELOG("NO-WB-GAP-STOP wordStart=%d hadPrev=%d wbDipMs=%.1f dur=%.1f\n",
                (int)t.wordStart, (int)hadPrevFrame, wbDipMs, stopMainDur);
        }

        Place place = getPlace(t.def->key);

        double burstMs = 7.0;
        double decayRate = 0.5;
        double spectralTilt = 0.0;

        switch (place) {
          case Place::Labial:   burstMs = 5.0;  decayRate = 0.6;  spectralTilt = 0.1;   break;
          case Place::Alveolar: burstMs = 7.0;  decayRate = 0.5;  spectralTilt = 0.0;   break;
          case Place::Velar:    burstMs = 11.0; decayRate = 0.4;  spectralTilt = -0.15; break;
          case Place::Palatal:  burstMs = 9.0;  decayRate = 0.45; spectralTilt = -0.1;  break;
          default: break;
        }

        if (t.def->hasBurstDurationMs) burstMs = t.def->burstDurationMs;
        if (t.def->hasBurstDecayRate) decayRate = t.def->burstDecayRate;
        if (t.def->hasBurstSpectralTilt) spectralTilt = t.def->burstSpectralTilt;

        // Coda blend: longer burst, faster decay
        if (t.codaFricStopBlend && !isAffricate) {
          burstMs = std::max(burstMs, stopMainDur * 0.6);
          decayRate = 0.7;
        }

        // Clamp burst to 75% of token duration so it always fires
        double maxBurst = stopMainDur * 0.75;
        if (burstMs > maxBurst) burstMs = maxBurst;

        {
          // Pitch interpolation across 2 micro-frames
          const double startPitch = base[vp];
          const double pitchDelta = base[evp] - startPitch;
          const double totalDur = stopMainDur;
          const double burstFrac = burstMs / totalDur;

          double seg1[kFrameFieldCount];
          std::memcpy(seg1, base, sizeof(seg1));
          seg1[vp] = startPitch;
          seg1[evp] = startPitch + pitchDelta * burstFrac;

          // Locus-theory F2/F3 blending: adjust burst formants toward
          // the following vowel's formants.  Velars shift most (~850 Hz
          // F2 range), alveolars shift moderately, labials shift least.
          // Based on Wintalker/DECTalk locus tables and OpenFormant's
          // 0.6 blend factor.  Without this, every /k/ has the same F2
          // regardless of context, creating a "chopped" transition.
          {
            const int cf2i = static_cast<int>(FieldId::cf2);
            const int cf3i = static_cast<int>(FieldId::cf3);
            const int pf2i = static_cast<int>(FieldId::pf2);
            const int pf3i = static_cast<int>(FieldId::pf3);
            // Look ahead for next vowel
            size_t curIdx = &t - tokens.data();
            double nextCf2 = 0, nextCf3 = 0;
            for (size_t j = curIdx + 1; j < tokens.size(); ++j) {
              const Token& ahead = tokens[j];
              if (ahead.def && (ahead.def->flags & kIsVowel)) {
                nextCf2 = ahead.def->field[cf2i];
                nextCf3 = ahead.def->field[cf3i];
                break;
              }
              if (ahead.silence) break;  // don't blend across silence
            }
            if (nextCf2 > 0 && seg1[cf2i] > 0) {
              double blend = 0.0;
              switch (place) {
                case Place::Velar:    blend = 0.55; break;  // highest context-dependency
                case Place::Palatal:  blend = 0.45; break;
                case Place::Alveolar: blend = 0.30; break;
                case Place::Labial:   blend = 0.20; break;  // least context-dependent
                default: break;
              }
              if (blend > 0.0) {
                seg1[cf2i] += blend * (nextCf2 - seg1[cf2i]);
                seg1[pf2i] += blend * (nextCf2 - seg1[pf2i]);
                if (nextCf3 > 0 && seg1[cf3i] > 0) {
                  seg1[cf3i] += blend * 0.5 * (nextCf3 - seg1[cf3i]);
                  seg1[pf3i] += blend * 0.5 * (nextCf3 - seg1[pf3i]);
                }
              }
            }
          }

          const int pa3i = static_cast<int>(FieldId::pa3);
          const int pa4i = static_cast<int>(FieldId::pa4);
          const int pa5i = static_cast<int>(FieldId::pa5);
          const int pa6i = static_cast<int>(FieldId::pa6);

          if (spectralTilt < 0.0) {
            seg1[pa5i] = std::min(1.0, seg1[pa5i] * (1.0 - spectralTilt));
            seg1[pa6i] = std::min(1.0, seg1[pa6i] * (1.0 - spectralTilt * 0.7));
          } else if (spectralTilt > 0.0) {
            seg1[pa3i] = std::min(1.0, seg1[pa3i] * (1.0 + spectralTilt));
            seg1[pa4i] = std::min(1.0, seg1[pa4i] * (1.0 + spectralTilt * 0.7));
          }

          // b3.1: rate-dependent frication tilt — VELAR STOPS ONLY.
          // At slow/normal rates (speed <= 1.2): flat (tiltDb=0, no effect).
          // At fast rates for velars: progressively darken pa4/pa5/pa6 so
          // velar bursts don't drift into alveolar-tap territory (issue #95
          // Bug 1, 29-Bloo's "Pegue → Pere" report at rate 1.3+).
          //   speed=1.3 → -1 dB, speed=1.7 → -5 dB, speed=2.0 → -8 dB.
          //
          // b3 applied this to all stop places and regressed alveolar/labial
          // crispness at fast rates (Mario Percinic's Mastodon report: TTS
          // "sounds like it lost its front teeth" — /t/, /p/ bursts dulled).
          // Gating on Place::Velar preserves the fix for our validated test
          // cases (pegue, fuego, lago, amigo are all velars) while keeping
          // natural high-freq burst energy on every other place of articulation.
          const double savedFricationTiltDb = frameEx.fricationTiltDb;
          frameEx.fricationTiltDb = (place == Place::Velar && speed > 1.2)
              ? -((speed - 1.2) * 10.0)
              : 0.0;

          nvspFrontend_Frame burstFrame;
          std::memcpy(&burstFrame, seg1, sizeof(burstFrame));
          emitFn(&burstFrame, &frameEx, burstMs, stopMainFade);
          hadPrevFrame = true;

          double seg2[kFrameFieldCount];
          std::memcpy(seg2, base, sizeof(seg2));
          seg2[vp] = startPitch + pitchDelta * burstFrac;
          seg2[evp] = startPitch + pitchDelta;

          const int faIdx = static_cast<int>(FieldId::fricationAmplitude);
          if (!isAffricate) {
            seg2[faIdx] *= (1.0 - decayRate);

            // Spectral evolution: high-frequency parallel amplitudes decay
            // faster than low, matching natural burst decay where the initial
            // broadband explosion narrows and dims.  Without this, the burst
            // and decay frames have identical spectral shape — just different
            // overall amplitude — creating an audible "seam."
            const int pa1i = static_cast<int>(FieldId::pa1);
            const int pa2i = static_cast<int>(FieldId::pa2);
            seg2[pa1i] *= (1.0 - decayRate * 0.3);  // low freq persists
            seg2[pa2i] *= (1.0 - decayRate * 0.4);
            seg2[pa3i] *= (1.0 - decayRate * 0.5);
            seg2[pa4i] *= (1.0 - decayRate * 0.7);
            seg2[pa5i] *= (1.0 - decayRate * 0.9);
            seg2[pa6i] *= (1.0 - decayRate * 1.0);  // high freq fades first
          } else {
            // Affricates: gentle tail decay so frication doesn't hit the
            // crossfade at 100%.  Without this, the affricate→vowel boundary
            // has an amplitude cliff (88%→0%) that creates a "stringy bounce."
            // Reduce to ~65% so the crossfade has less work to do.
            seg2[faIdx] *= 0.75;
          }

          nvspFrontend_Frame decayFrame;
          std::memcpy(&decayFrame, seg2, sizeof(decayFrame));
          double decayDur = stopMainDur - burstMs;
          double decayFade = std::min(burstMs * 0.8, decayDur);
          emitFn(&decayFrame, &frameEx, decayDur, decayFade);

          // Restore fricationTiltDb so next phoneme emits without rate tilt.
          frameEx.fricationTiltDb = savedFricationTiltDb;

          trajectoryState->prevCf2 = burstFrame.cf2;
          trajectoryState->prevCf3 = burstFrame.cf3;
          trajectoryState->prevPf2 = burstFrame.pf2;
          trajectoryState->prevPf3 = burstFrame.pf3;
          trajectoryState->prevVoiceAmp = base[va];
          trajectoryState->prevFricAmp = base[fa];
          trajectoryState->hasPrevFrame = true;
          trajectoryState->prevWasNasal = false;

          prevTokenWasStop = true;
          continue;
        }
      }
    }

    // ============================================
    // FRICATIVE ATTACK/DECAY MICRO-FRAME EMISSION
    // ============================================
    {
      const bool isStop2 = t.def && ((t.def->flags & kIsStop) != 0);
      const bool isAffricate2 = t.def && ((t.def->flags & kIsAfricate) != 0);
      const double fricAmp = base[fa];

      if (!isStop2 && !isAffricate2 && !t.silence && !t.preStopGap &&
          !t.postStopAspiration && !t.voicedClosure && fricAmp > 0.0) {

        double attackMs = t.def->hasFricAttackMs ? t.def->fricAttackMs : 3.0;
        double decayMs = t.def->hasFricDecayMs ? t.def->fricDecayMs : 4.0;

        // An adjacent token with the SAME phoneme def is one continuous
        // frication event, not two: re-triggering the attack/decay envelope
        // at the junction reads as a restarted sample (issue #110,
        // "las semillas" s#s).  Suppress the decay when the next token
        // continues this fricative, and the attack when it continues the
        // previous one.
        const size_t fricIdx = &t - tokens.data();
        auto continuesFric = [&](const Token& o) {
          return o.def == t.def && !o.silence && !o.preStopGap &&
                 !o.postStopAspiration && !o.voicedClosure;
        };
        const bool contPrev = fricIdx > 0 && continuesFric(tokens[fricIdx - 1]);
        const bool contNext = fricIdx + 1 < tokens.size() &&
                              continuesFric(tokens[fricIdx + 1]);

        // Skip attack ramp in post-stop clusters (/ks/, /ts/, etc.)
        if (!prevTokenWasStop && attackMs + decayMs + 2.0 < t.durationMs) {
          const int faIdx = static_cast<int>(FieldId::fricationAmplitude);

          // Pitch interpolation across 3 micro-frames
          const double startPitch = base[vp];
          const double pitchDelta = base[evp] - startPitch;
          const double totalDur = t.durationMs;
          const double effAttackMs = contPrev ? 0.0 : attackMs;
          const double effDecayMs = contNext ? 0.0 : decayMs;
          const double sustainDur = totalDur - effAttackMs - effDecayMs;
          const double attackFrac = effAttackMs / totalDur;
          const double sustainEndFrac = (effAttackMs + sustainDur) / totalDur;

          if (!contPrev) {
            // --- Attack micro-frame: ramp from 10% to full ---
            double seg1[kFrameFieldCount];
            std::memcpy(seg1, base, sizeof(seg1));
            seg1[faIdx] = fricAmp * 0.1;
            seg1[vp] = startPitch;
            seg1[evp] = startPitch + pitchDelta * attackFrac;

            nvspFrontend_Frame attackFrame;
            std::memcpy(&attackFrame, seg1, sizeof(attackFrame));
            emitFn(&attackFrame, &frameEx, attackMs, t.fadeMs);
          }

          // --- Sustain micro-frame: full amplitude ---
          double seg2s[kFrameFieldCount];
          std::memcpy(seg2s, base, sizeof(seg2s));
          seg2s[vp] = startPitch + pitchDelta * attackFrac;
          seg2s[evp] = startPitch + pitchDelta * sustainEndFrac;

          nvspFrontend_Frame sustainFrame;
          std::memcpy(&sustainFrame, seg2s, sizeof(sustainFrame));
          emitFn(&sustainFrame, &frameEx, sustainDur,
                 contPrev ? t.fadeMs : attackMs);
          hadPrevFrame = true;

          if (!contNext) {
            // --- Decay micro-frame: ramp from full to 30% ---
            double seg3[kFrameFieldCount];
            std::memcpy(seg3, base, sizeof(seg3));
            seg3[faIdx] = fricAmp * 0.3;
            seg3[vp] = startPitch + pitchDelta * sustainEndFrac;
            seg3[evp] = startPitch + pitchDelta;

            nvspFrontend_Frame decayFrame;
            std::memcpy(&decayFrame, seg3, sizeof(decayFrame));
            emitFn(&decayFrame, &frameEx, decayMs, decayMs * 0.5);
          }

          trajectoryState->prevCf2 = sustainFrame.cf2;
          trajectoryState->prevCf3 = sustainFrame.cf3;
          trajectoryState->prevPf2 = sustainFrame.pf2;
          trajectoryState->prevPf3 = sustainFrame.pf3;
          trajectoryState->prevVoiceAmp = base[va];
          trajectoryState->prevFricAmp = base[fa];
          trajectoryState->hasPrevFrame = true;
          trajectoryState->prevWasNasal = false;

          prevTokenWasStop = false;
          continue;
        }
      }
    }

    // ============================================
    // RELEASE SPREAD (postStopAspiration tokens)
    // ============================================
    if (t.postStopAspiration && t.def && t.durationMs > 1.0) {
      if (t.codaFricStopBlend) {
        // Coda blend: single decaying aspiration frame, no ramp-in dip.
        const int aspIdx = static_cast<int>(FieldId::aspirationAmplitude);
        double seg[kFrameFieldCount];
        std::memcpy(seg, base, sizeof(seg));
        seg[aspIdx] *= 0.60;

        nvspFrontend_Frame aspFrame;
        std::memcpy(&aspFrame, seg, sizeof(aspFrame));
        emitFn(&aspFrame, &frameEx, t.durationMs, t.durationMs * 0.5);

        trajectoryState->prevCf2 = aspFrame.cf2;
        trajectoryState->prevCf3 = aspFrame.cf3;
        trajectoryState->prevPf2 = aspFrame.pf2;
        trajectoryState->prevPf3 = aspFrame.pf3;
        trajectoryState->prevVoiceAmp = base[va];
        trajectoryState->prevFricAmp = base[fa];
        trajectoryState->hasPrevFrame = true;
        trajectoryState->prevWasNasal = false;

        prevTokenWasStop = false;
        continue;
      }

      double spreadMs = t.def->hasReleaseSpreadMs ? t.def->releaseSpreadMs : 4.0;

      if (spreadMs > 0.0 && spreadMs < t.durationMs) {
        const int faIdx = static_cast<int>(FieldId::fricationAmplitude);
        const int aspIdx = static_cast<int>(FieldId::aspirationAmplitude);

        // Pitch interpolation across 2 micro-frames
        const double startPitch = base[vp];
        const double pitchDelta = base[evp] - startPitch;
        const double totalDur = t.durationMs;
        const double spreadFrac = (totalDur > 0.0) ? (spreadMs / totalDur) : 0.0;

        double seg1[kFrameFieldCount];
        std::memcpy(seg1, base, sizeof(seg1));
        seg1[faIdx] *= 0.15;
        seg1[aspIdx] *= 0.15;
        seg1[vp] = startPitch;
        seg1[evp] = startPitch + pitchDelta * spreadFrac;

        nvspFrontend_Frame rampFrame;
        std::memcpy(&rampFrame, seg1, sizeof(rampFrame));
        emitFn(&rampFrame, &frameEx, spreadMs, t.fadeMs);
        hadPrevFrame = true;

        double seg2[kFrameFieldCount];
        std::memcpy(seg2, base, sizeof(seg2));
        seg2[vp] = startPitch + pitchDelta * spreadFrac;
        seg2[evp] = startPitch + pitchDelta;

        nvspFrontend_Frame fullFrame;
        std::memcpy(&fullFrame, seg2, sizeof(fullFrame));
        double fullDur = t.durationMs - spreadMs;
        emitFn(&fullFrame, &frameEx, fullDur, spreadMs * 0.5);

        trajectoryState->prevCf2 = fullFrame.cf2;
        trajectoryState->prevCf3 = fullFrame.cf3;
        trajectoryState->prevPf2 = fullFrame.pf2;
        trajectoryState->prevPf3 = fullFrame.pf3;
        trajectoryState->prevVoiceAmp = base[va];
        trajectoryState->prevFricAmp = base[fa];
        trajectoryState->hasPrevFrame = true;
        trajectoryState->prevWasNasal = false;

        prevTokenWasStop = true;  // aspiration is stop-related
        continue;
      }
    }

    // ============================================
    // TAP MICRO-EVENT EMISSION (coarticulated amplitude notch)
    // =========================================================
    // A tap is almost entirely coarticulation — the tongue tip briefly flicks
    // the alveolar ridge without the jaw closing.  The primary cue is a brief
    // amplitude dip, but formants must also sweep smoothly from the preceding
    // vowel through the alveolar target and back, never dwelling at the tap's
    // static formant values.  Using static tap formants for all 3 phases
    // created an abrupt formant jump that sounded velar/clenched.
    //
    // New approach: onset starts at 70% previous vowel + 30% tap formants,
    // notch reaches 100% tap target (brief), recovery blends back out.
    // The DSP crossfade to the following token handles the exit naturally.
    const bool isTap = t.def && ((t.def->flags & kIsTap) != 0);
    // Skip micro-event notch for very short taps.
    if (isTap && t.durationMs >= 8.0) {
      const double totalDur = t.durationMs;

      // Phase proportions: onset 30%, notch 40%, recovery 30%.
      // More onset/recovery time than before for smoother coarticulation.
      const double notchFloorMs = 1.5;
      double notchDur = std::max(totalDur * 0.40, notchFloorMs);
      if (notchDur > totalDur * 0.70) notchDur = totalDur * 0.70;
      const double remainDur = totalDur - notchDur;
      const double onsetDur = remainDur * 0.45;
      const double recovDur = remainDur - onsetDur;

      // Amplitude dip: 55% notch — audible flick without sounding like
      // a stop closure.  42% was too deep before weak syllables (schwa/ɚ),
      // making "pattern", "ladder", "untitled" sound stop-like.
      // 60% was too gentle (soft/Russian).  55% splits the difference.
      const int vaIdx = static_cast<int>(FieldId::voiceAmplitude);
      const double origAmp = base[vaIdx];
      const double notchAmp = origAmp * 0.55;

      // Formant indices for coarticulation blending.
      const int cf1 = static_cast<int>(FieldId::cf1);
      const int cf2 = static_cast<int>(FieldId::cf2);
      const int cf3 = static_cast<int>(FieldId::cf3);

      // Pitch interpolation
      const double startPitch = base[vp];
      const double pitchDelta = base[evp] - startPitch;
      const double onsetFrac = (totalDur > 0.0) ? (onsetDur / totalDur) : 0.0;
      const double notchEndFrac = (totalDur > 0.0) ? ((onsetDur + notchDur) / totalDur) : 0.0;

      const double microFade = std::max(0.5, 1.5 / std::max(0.5, speed));

      // Phase 1: onset — formants blend FROM previous vowel TOWARD tap.
      // 50/50 blend — research says tap transitions should be "abrupt but
      // short".  Too much previous vowel (70/30) sounded soft/Russian.
      {
        double seg[kFrameFieldCount];
        std::memcpy(seg, base, sizeof(seg));
        if (trajectoryState->hasPrevBase) {
          const double prevW = 0.50;
          const double tapW  = 0.50;
          seg[cf1] = trajectoryState->prevBase[cf1] * prevW + base[cf1] * tapW;
          seg[cf2] = trajectoryState->prevBase[cf2] * prevW + base[cf2] * tapW;
          seg[cf3] = trajectoryState->prevBase[cf3] * prevW + base[cf3] * tapW;
        }
        seg[vp] = startPitch;
        seg[evp] = startPitch + pitchDelta * onsetFrac;

        nvspFrontend_Frame f;
        std::memcpy(&f, seg, sizeof(f));
        emitFn(&f, &frameEx, onsetDur, microFade);
        hadPrevFrame = true;
      }

      // Phase 2: notch — amplitude dipped, tap formants at full target.
      // This is the brief moment of alveolar contact.
      {
        double seg[kFrameFieldCount];
        std::memcpy(seg, base, sizeof(seg));
        seg[vaIdx] = notchAmp;
        seg[vp] = startPitch + pitchDelta * onsetFrac;
        seg[evp] = startPitch + pitchDelta * notchEndFrac;

        nvspFrontend_Frame f;
        std::memcpy(&f, seg, sizeof(f));
        emitFn(&f, &frameEx, notchDur, microFade);
      }

      // Phase 3: recovery — formants blend AWAY from tap back toward
      // neutral.  Uses 50/50 blend; the DSP crossfade to the next token
      // completes the trajectory to the following vowel.
      //
      // Exception: utterance-final taps (e.g. Spanish "amor") skip the
      // vowel blend — there's no following vowel to transition toward.
      // Instead, keep tap formants and decay amplitude so the frication
      // burst carries the release cue.  Without this, singleWordFinalHold
      // sustains the vowel-blended recovery for ~35ms, sounding lateral.
      {
        const bool isLastToken = (&t == &tokens.back());
        double seg[kFrameFieldCount];
        std::memcpy(seg, base, sizeof(seg));
        if (isLastToken) {
          // Utterance-final: decay voicing, keep tap formants + frication.
          seg[vaIdx] = notchAmp * 0.3;
        } else if (trajectoryState->hasPrevBase) {
          // Blend back toward vowel space — abrupt exit from tap.
          const double prevW = 0.65;
          const double tapW  = 0.35;
          seg[cf1] = trajectoryState->prevBase[cf1] * prevW + base[cf1] * tapW;
          seg[cf2] = trajectoryState->prevBase[cf2] * prevW + base[cf2] * tapW;
          seg[cf3] = trajectoryState->prevBase[cf3] * prevW + base[cf3] * tapW;
        }
        seg[vp] = startPitch + pitchDelta * notchEndFrac;
        seg[evp] = startPitch + pitchDelta;

        nvspFrontend_Frame f;
        std::memcpy(&f, seg, sizeof(f));
        emitFn(&f, &frameEx, recovDur, microFade);
      }

      trajectoryState->prevVoiceAmp = base[va];
      trajectoryState->prevFricAmp = base[fa];
      trajectoryState->hasPrevFrame = true;
      trajectoryState->prevWasNasal = false;

      prevTokenWasTap = true;
      prevTokenWasStop = false;
      continue;
    }

    // ============================================
    // WORD-BOUNDARY AMPLITUDE DIP
    // ============================================
    // Emit a brief reduced-amplitude micro-frame at word starts so the
    // listener can parse word boundaries, especially at high speech rates.
    // Context-aware: deeper dip after stops (e.g. "dialog type") where
    // the stop's own release blends into the next word's onset.
    double mainDur = t.durationMs;
    double mainFade = t.fadeMs;

    if (wbDipMs > 0.0 && t.wordStart && hadPrevFrame && mainDur > wbDipMs + 1.0) {
      double dipDepth = lang.wordBoundaryDipDepth;

      // After a stop, deepen the dip — the stop's release can smear into
      // the next word's onset, especially at high rates.
      if (prevTokenWasStop) {
        dipDepth *= 0.5; // e.g. 0.50 * 0.5 = 0.25 → 75% amplitude cut
      }

      FELOG("WB-DIP-NORM wordStart=%d prevWasStop=%d dipDepth=%.2f dipMs=%.1f dur=%.1f\n",
            (int)t.wordStart, (int)prevTokenWasStop, dipDepth, wbDipMs, mainDur);

      const double dipDur = wbDipMs;

      double dip[kFrameFieldCount];
      std::memcpy(dip, base, sizeof(dip));
      dip[va] *= dipDepth;
      dip[fa] *= dipDepth;
      dip[static_cast<int>(FieldId::aspirationAmplitude)] *= dipDepth;

      nvspFrontend_Frame dipFrame;
      std::memcpy(&dipFrame, dip, sizeof(dipFrame));
      emitFn(&dipFrame, &frameEx, dipDur, mainFade);
      hadPrevFrame = true;

      mainDur -= dipDur;
      mainFade = dipDur; // crossfade from dip back to full amplitude
    }

    // Normal frame emission
    nvspFrontend_Frame frame;
    std::memcpy(&frame, base, sizeof(frame));

    // Trajectory limiting is handled by the trajectory_limit pass (fade-based,
    // Trans-F aware).  We only track state needed by micro-event emission here.
    const bool isNasal = t.def && ((t.def->flags & kIsNasal) != 0);
    trajectoryState->prevVoiceAmp = base[va];
    trajectoryState->prevFricAmp = base[fa];
    trajectoryState->hasPrevFrame = true;
    trajectoryState->prevWasNasal = isNasal;

    // Post-tap: cap fade on the token after a tap so the tap's amplitude
    // notch rings briefly before the next sound smears it away.
    double emitFade = mainFade;
    if (prevTokenWasTap && mainDur > 0.0) {
      emitFade = std::min(emitFade, mainDur * 0.50);
    }

    // Pitch-dependent fade floor: at low F0, each glottal cycle is longer
    // so crossfades span fewer cycles and individual frame boundaries become
    // audible (choppy at low pitch). Enforce a minimum of 2 glottal cycles.
    if (base[vp] > 0.0) {
      double minFadeMs = 2000.0 / base[vp]; // 2 cycles at current pitch
      if (emitFade < minFadeMs && mainDur > minFadeMs) {
        emitFade = minFadeMs;
      }
    }

    emitFn(&frame, &frameEx, mainDur, emitFade);
    hadPrevFrame = true;

    prevTokenWasTap = false;
    prevTokenWasStop = t.def && (
      ((t.def->flags & kIsStop) != 0) ||
      ((t.def->flags & kIsAfricate) != 0) ||
      t.postStopAspiration);
  }
}

void emitFrames(
  const PackSet& pack,
  const std::vector<Token>& tokens,
  int userIndexBase,
  double speed,
  TrajectoryState* trajectoryState,
  nvspFrontend_FrameCallback cb,
  void* userData
) {
  if (!cb) return;
  nvspFrontend_FrameEx dummyEx = {};
  auto emitter = [&](const nvspFrontend_Frame* f, const nvspFrontend_FrameEx*,
                     double dur, double fade) {
    cb(userData, f, dur, fade, userIndexBase);
  };
  generateAcousticEvents(pack, tokens, userIndexBase, speed, dummyEx,
                         trajectoryState, emitter);
}

void emitFramesEx(
  const PackSet& pack,
  const std::vector<Token>& tokens,
  int userIndexBase,
  double speed,
  const nvspFrontend_FrameEx& frameExDefaults,
  TrajectoryState* trajectoryState,
  nvspFrontend_FrameExCallback cb,
  void* userData,
  std::vector<tgsb_data::FrameTraceEntry>* traceSink
) {
  if (!cb) return;
  int traceFrameCount = 0;
  auto emitter = [&](const nvspFrontend_Frame* f, const nvspFrontend_FrameEx* fEx,
                     double dur, double fade) {
    if (traceSink) ++traceFrameCount;
    cb(userData, f, fEx, dur, fade, userIndexBase);
  };
  generateAcousticEvents(pack, tokens, userIndexBase, speed, frameExDefaults,
                         trajectoryState, emitter,
                         traceSink, traceSink ? &traceFrameCount : nullptr);
}

} // namespace nvsp_frontend
