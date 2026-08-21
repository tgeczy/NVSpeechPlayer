/*
TGSpeechBox — Shared types and helpers for frontend passes.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSB_FRONTEND_PASSES_PASS_COMMON_H
#define TGSB_FRONTEND_PASSES_PASS_COMMON_H

#include <string>
#include <unordered_map>
#include <vector>

#include "../data_query.h"
#include "../pack.h"
#include "../ipa_engine.h"

namespace nvsp_frontend {

// When a pass runs in the frontend pipeline.
enum class PassStage {
  // After parse/transforms/default voice defaults, before calculateTimes.
  PreTiming = 0,

  // After calculateTimes, before calculatePitches.
  PostTiming = 1,

  // After calculatePitches (and tone contours if used).
  PostPitch = 2,
};

// Context passed through all passes.
struct PassContext {
  const PackSet& pack;
  double speed = 1.0;
  double basePitch = 100.0;
  double inflection = 0.6;
  char clauseType = '.';

  // Passes can stash intermediate values here for later passes.
  std::unordered_map<std::string, double> scratchpad;

  // Optional per-pass token-state sink. When non-null, pass_pipeline writes
  // one PassSnapshot per non-silence token after each pass runs. Used by
  // the test harness to observe how passes mutate phoneme parameters. Caller
  // is responsible for pre-reserving capacity and clearing between utterances.
  std::vector<tgsb_data::PassSnapshot>* passTraceSink = nullptr;

  PassContext(const PackSet& p, double s, double bp, double inf, char ct)
      : pack(p), speed(s), basePitch(bp), inflection(inf), clauseType(ct) {}
};

// Each pass modifies tokens in place and returns success.
// On failure, outError is set.
using PassFn = bool (*)(
    PassContext& ctx,
    std::vector<Token>& tokens,
    std::string& outError);

struct PassDesc {
  const char* name = "";
  PassStage stage = PassStage::PreTiming;
  PassFn fn = nullptr;
};

// Place of articulation (shared across coarticulation and boundary smoothing).
enum class Place {
  Unknown,
  Labial,
  Alveolar,
  Palatal,
  Velar,
};

inline Place getPlace(const std::u32string& rawKey) {
  // Language-variant allophones carry a suffix after '_' (d_es, s_mx,
  // ɣ_es, l_es, …).  Match on the base symbol: before this, every
  // variant returned Place::Unknown and all place-driven passes
  // (coarticulation locus, cluster blend, boundary smoothing, burst
  // placement) silently skipped them — #108/#109, the Spanish onset
  // place-cue loss.  Suffixed VOWEL keys (a_open, u_fi) are unaffected:
  // their bases aren't in these consonant lists either way.
  const size_t us = rawKey.find(U'_');
  const std::u32string key =
      (us == std::u32string::npos) ? rawKey : rawKey.substr(0, us);
  // Labials
  if (key == U"p" || key == U"b" || key == U"m" ||
      key == U"f" || key == U"v" || key == U"w" ||
      key == U"ʍ" || key == U"ɸ" || key == U"β") {
    return Place::Labial;
  }

  // Alveolars
  if (key == U"t" || key == U"d" || key == U"n" ||
      key == U"s" || key == U"z" || key == U"l" ||
      key == U"r" || key == U"ɹ" || key == U"ɾ" ||
      key == U"θ" || key == U"ð" || key == U"ɬ" ||
      key == U"ɮ" || key == U"ɻ" || key == U"ɖ" ||
      key == U"ʈ" || key == U"ɳ" || key == U"ɽ") {
    return Place::Alveolar;
  }

  // Palatals / Postalveolars (including tie-bar affricate variants)
  if (key == U"ʃ" || key == U"ʒ" || key == U"tʃ" ||
      key == U"dʒ" || key == U"t\u0361ʃ" || key == U"d\u0361ʒ" ||
      key == U"j" || key == U"ɲ" ||
      key == U"ç" || key == U"ʝ" || key == U"c" ||
      key == U"ɟ" || key == U"ʎ") {
    return Place::Palatal;
  }

  // Velars
  if (key == U"k" || key == U"g" || key == U"ɡ" ||  // both ASCII 0x67 and IPA U+0261
      key == U"ŋ" || key == U"x" || key == U"ɣ" || key == U"ɰ") {
    return Place::Velar;
  }

  return Place::Unknown;
}

}  // namespace nvsp_frontend

#endif  // TGSB_FRONTEND_PASSES_PASS_COMMON_H
