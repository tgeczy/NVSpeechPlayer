/*
TGSpeechBox — IPA engine interface and Token definitions.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSB_FRONTEND_IPA_ENGINE_H
#define TGSB_FRONTEND_IPA_ENGINE_H

#include <string>
#include <vector>

#include "data_query.h"
#include "pack.h"
#include "nvspFrontend.h"

namespace nvsp_frontend {

// ============================================================================
// Trajectory limiting state (per-handle, NOT static)
// ============================================================================
// This state tracks previous frame formant values for rate-of-change limiting.
// It MUST be stored per-handle (not as function-static variables) to avoid
// data races when multiple engine instances speak concurrently.
//
// The state is reset at the start of each utterance by calling reset().
struct TrajectoryState {
  double prevCf2 = 0.0;
  double prevCf3 = 0.0;
  double prevPf2 = 0.0;
  double prevPf3 = 0.0;
  bool hasPrevFrame = false;
  bool prevWasNasal = false;  // Skip limiting on frames following nasals

  // For equal-power amplitude crossfade detection
  double prevVoiceAmp = 0.0;
  double prevFricAmp = 0.0;

  // Full previous base frame for voice bar emission.
  // Contains pitch, GOQ, outputGain, vibrato — everything PhonemeDef lacks.
  double prevBase[kFrameFieldCount] = {};
  bool hasPrevBase = false;

  // Previous FrameEx for voice bar emission (keeps Fujisaki model alive).
  // Without this, voice bar sends fujisakiEnabled=0 → DSP resets IIR state.
  nvspFrontend_FrameEx prevFrameEx = {};
  bool hasPrevFrameEx = false;

  void reset() {
    prevCf2 = 0.0;
    prevCf3 = 0.0;
    prevPf2 = 0.0;
    prevPf3 = 0.0;
    hasPrevFrame = false;
    prevWasNasal = false;
    prevVoiceAmp = 0.0;
    prevFricAmp = 0.0;
    hasPrevBase = false;
    hasPrevFrameEx = false;
  }
};

struct Token {
  // If def is null, this token is "silence" (no frame).
  const PhonemeDef* def = nullptr;

  // Per-token field values (only valid where setMask has the bit set).
  std::uint64_t setMask = 0;
  double field[kFrameFieldCount] = {0.0};

  // Meta.
  bool silence = false;
  bool preStopGap = false;
  bool clusterGap = false;
  bool nasalToStopGap = false;  // Dedicated nasal→stop closure gap
  bool postStopAspiration = false;
  bool vowelHiatusGap = false;
  bool voicedClosure = false;  // Voice bar: maintain low-amplitude voicing during closure
  bool codaFricStopBlend = false;  // Coda fricative→stop: maintain noise continuity

  bool wordStart = false;
  bool syllableStart = false;
  int syllableIndex = -1;   // -1 = unassigned. Set by syllable_marking pass.
  int stress = 0; // 0 none, 1 primary, 2 secondary

  bool tiedTo = false;
  bool tiedFrom = false;
  int lengthened = 0;  // count of length marks (ː) - 0=none, 1=one, 2=double, etc.

  // Base char used for some tweaks (like Hungarian short vowel checks).
  char32_t baseChar = 0;

  // Timing (ms) computed later.
  double durationMs = 0.0;
  double fadeMs = 0.0;

  // Tonal marker captured for this syllable start (UTF-32 string), if any.
  std::u32string tone;
  
  // DECTalk-style formant end targets (set by coarticulation pass)
  // These allow within-phoneme formant ramping for CV transitions.
  // If hasEndCf* is true, the token-level value overrides PhonemeDef.
  bool hasEndCf1 = false;
  bool hasEndCf2 = false;
  bool hasEndCf3 = false;
  double endCf1 = 0.0;
  double endCf2 = 0.0;
  double endCf3 = 0.0;

  // Diphthong glide flag (set by diphthong_collapse pass).
  // When true, frame_emit emits cosine-smoothed micro-frames instead of
  // a single steady-state frame.  Start formants are in field[cf1/2/3]
  // and end targets are in endCf1/2/3 (set by the collapse pass).
  bool isDiphthongGlide = false;

  // End targets for parallel formants (mirrors existing endCf pattern).
  // Needed for nasal diphthongs (Portuguese ão/ãe) where the parallel path
  // diverges from cascade.  Falls back to endCf values when unset.
  bool hasEndPf1 = false, hasEndPf2 = false, hasEndPf3 = false;
  double endPf1 = 0.0, endPf2 = 0.0, endPf3 = 0.0;

  // End targets for cascade bandwidths (set by diphthong_collapse pass).
  // Without these, diphthong micro-frames hold onset bandwidths constant
  // while frequencies sweep — producing unnaturally broad resonances at the
  // offset (e.g. /aɪ/: onset B1=116 is fine for F1=677, but way too wide
  // for offset F1=413 where natural B1≈55-70 Hz).
  bool hasEndCb1 = false, hasEndCb2 = false, hasEndCb3 = false;
  double endCb1 = 0.0, endCb2 = 0.0, endCb3 = 0.0;
  bool hasEndPb1 = false, hasEndPb2 = false, hasEndPb3 = false;
  double endPb1 = 0.0, endPb2 = 0.0, endPb3 = 0.0;

  // Canonical steady-state formant targets (set by coarticulation pass when
  // lang.coarticulationSteadyState is on).  With coartic shaping, field[cf*]
  // holds the locus-shifted ONSET and endCf* holds the EXIT target — the
  // canonical vowel would otherwise never be rendered (#113: /o/ in "dos"
  // ramped 1129→1105 Hz, never touching 910).  When hasCoarticSteady is set,
  // frame_emit renders transition → steady(these values) → exit instead of
  // one whole-token ramp.  Zero = formant not applicable (keep base value).
  bool hasCoarticSteady = false;
  double steadyF1 = 0.0, steadyF2 = 0.0, steadyF3 = 0.0;

  // Per-parameter transition speed scales (carried to FrameEx).
  // 0.0 = no override.  See speechPlayer_frameEx_t for semantics.
  double transF1Scale = 0.0;
  double transF2Scale = 0.0;
  double transF3Scale = 0.0;
  double transNasalScale = 0.0;
  double transSourceHoldRatio = 0.0;
  double transVoicingHoldRatio = 0.0;

  // Token-level breathiness override (set by allophone rules).
  // Added to phoneme-level and user-level breathiness in frame_emit.
  bool hasTokenBreathiness = false;
  double tokenBreathiness = 0.0;

  // Fujisaki pitch model markers (set by applyPitchFujisaki)
  // These are passed through to frameEx for DSP-level pitch contour generation.
  bool fujisakiEnabled = false;
  bool fujisakiReset = false;         // Reset model state (at clause start)
  double fujisakiPhraseAmp = 0.0;     // Phrase command amplitude (0 = no command)
  double fujisakiAccentAmp = 0.0;     // Accent command amplitude (0 = no command)
  double fujisakiAccentDurScale = 1.0; // Per-token accent duration multiplier (monosyllable shortening)

  // Prominence score (0.0–1.0), set by the prominence pass.
  // Used by pitch_fujisaki for accent scaling when prominencePitchFromProminence is true.
  double prominence = -1.0;  // -1.0 = not set (prominence pass didn't run)
};

// ============================================================================
// Token helper functions (inline for use across translation units)
// ============================================================================
inline bool tokenIsTrill(const Token& t) {
  return t.def && ((t.def->flags & kIsTrill) != 0);
}

inline bool tokenIsVowel(const Token& t) {
  return t.def && ((t.def->flags & kIsVowel) != 0);
}

inline bool tokenIsSemivowel(const Token& t) {
  return t.def && ((t.def->flags & kIsSemivowel) != 0);
}

inline bool tokenIsLiquid(const Token& t) {
  return t.def && ((t.def->flags & kIsLiquid) != 0);
}

// Convert IPA -> tokens.
// This runs:
//  1) normalization (pack rules)
//  2) phoneme mapping
//  3) gap insertion
//  4) copy-adjacent correction
//  5) transforms
//  6) timing + pitch
//
// On success, tokens are ready to be converted to nvspFrontend_Frame.
bool convertIpaToTokens(
  const PackSet& pack,
  const std::string& ipaUtf8,
  double speed,
  double basePitch,
  double inflection,
  char clauseType,
  std::vector<Token>& outTokens,
  std::string& outError,
  std::vector<tgsb_data::PassSnapshot>* passTraceSink = nullptr
);

// Convert tokens -> callback frames.
// trajectoryState is per-handle state for formant smoothing (must not be null).
void emitFrames(
  const PackSet& pack,
  const std::vector<Token>& tokens,
  int userIndexBase,
  double speed,
  TrajectoryState* trajectoryState,
  nvspFrontend_FrameCallback cb,
  void* userData
);

// Convert tokens -> callback frames with extended parameters (ABI v2+).
// frameExDefaults contains user-level defaults that are mixed with per-phoneme values.
// trajectoryState is per-handle state for formant smoothing (must not be null).
// traceSink (optional): if non-null, one FrameTraceEntry is appended at the
// start of each Token with the phoneme's IPA key and the frame index of its
// first emission. Caller must pre-reserve and clear the vector.
void emitFramesEx(
  const PackSet& pack,
  const std::vector<Token>& tokens,
  int userIndexBase,
  double speed,
  const nvspFrontend_FrameEx& frameExDefaults,
  TrajectoryState* trajectoryState,
  nvspFrontend_FrameExCallback cb,
  void* userData,
  std::vector<tgsb_data::FrameTraceEntry>* traceSink = nullptr
);

} // namespace nvsp_frontend

#endif
