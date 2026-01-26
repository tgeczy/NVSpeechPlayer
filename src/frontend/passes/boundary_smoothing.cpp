#include "boundary_smoothing.h"

#include <algorithm>

namespace nvsp_frontend::passes {

namespace {

static inline bool tokIsSilenceOrMissing(const Token& t) {
  return t.silence || !t.def;
}

static inline bool tokIsVowel(const Token& t) {
  return t.def && ((t.def->flags & kIsVowel) != 0);
}

static inline bool tokIsSemivowel(const Token& t) {
  return t.def && ((t.def->flags & kIsSemivowel) != 0);
}

static inline bool tokIsVowelLike(const Token& t) {
  return tokIsVowel(t) || tokIsSemivowel(t);
}

static inline bool tokIsStopLike(const Token& t) {
  if (!t.def || t.silence) return false;
  // Treat post-stop aspiration as part of the stop release for boundary rules.
  if (t.postStopAspiration) return true;
  const uint32_t f = t.def->flags;
  return ((f & kIsStop) != 0) || ((f & kIsAfricate) != 0);
}

static inline bool tokIsFricativeLike(const Token& t) {
  // In this engine, fricatives are typically represented by non-zero
  // fricationAmplitude.
  if (!t.def || t.silence) return false;
  const int fa = static_cast<int>(FieldId::fricationAmplitude);
  const uint64_t bit = 1ULL << fa;
  double v = 0.0;
  if (t.setMask & bit) v = t.field[fa];
  else if (t.def->setMask & bit) v = t.def->field[fa];
  return v > 0.0;
}

static inline void clampFadeToDuration(Token& t) {
  if (t.durationMs < 0.0) t.durationMs = 0.0;
  if (t.fadeMs < 0.0) t.fadeMs = 0.0;
  if (t.fadeMs > t.durationMs) t.fadeMs = t.durationMs;
}

static int findPrevReal(
    const std::vector<Token>& tokens,
    int idxBefore,
    double maxSkipSilenceMs) {
  for (int j = idxBefore; j >= 0; --j) {
    const Token& t = tokens[static_cast<size_t>(j)];

    if (!tokIsSilenceOrMissing(t)) return j;

    // Do not reach across long pauses. We only skip short silences or
    // silences that were inserted as micro-gaps.
    if (t.silence) {
      const bool isMicroGap = t.preStopGap || t.clusterGap || t.vowelHiatusGap;
      if (!isMicroGap && t.durationMs > maxSkipSilenceMs) {
        break;
      }
    }
  }
  return -1;
}

}  // namespace

bool runBoundarySmoothing(PassContext& ctx, std::vector<Token>& tokens, std::string& outError) {
  (void)outError;

  const auto& lang = ctx.pack.lang;
  if (!lang.boundarySmoothingEnabled) return true;
  if (tokens.size() < 2) return true;

  // Values are specified as ms at speed=1.0, consistent with other timing knobs.
  const double sp = (ctx.speed > 0.0) ? ctx.speed : 1.0;
  const double v2s = std::max(0.0, lang.boundarySmoothingVowelToStopFadeMs) / sp;
  const double s2v = std::max(0.0, lang.boundarySmoothingStopToVowelFadeMs) / sp;
  const double v2f = std::max(0.0, lang.boundarySmoothingVowelToFricFadeMs) / sp;

  // If there's a real pause, don't treat earlier phonemes as adjacent.
  const double maxSkipSilenceMs = 60.0;

  // Fade belongs to the *incoming* token. We therefore adjust `tokens[i].fadeMs`
  // based on the nearest preceding real phoneme (skipping inserted silence gaps).
  for (int i = 0; i < static_cast<int>(tokens.size()); ++i) {
    Token& cur = tokens[static_cast<size_t>(i)];
    if (tokIsSilenceOrMissing(cur)) continue;

    const int prevIdx = findPrevReal(tokens, i - 1, maxSkipSilenceMs);
    if (prevIdx < 0) continue;
    const Token& prev = tokens[static_cast<size_t>(prevIdx)];

    // Vowel -> Stop
    if (v2s > 0.0 && tokIsVowelLike(prev) && tokIsStopLike(cur)) {
      cur.fadeMs = std::max(cur.fadeMs, v2s);

      clampFadeToDuration(cur);
      continue;
    }

    // Stop -> Vowel
    if (s2v > 0.0 && tokIsStopLike(prev) && tokIsVowelLike(cur)) {
      cur.fadeMs = std::max(cur.fadeMs, s2v);
      clampFadeToDuration(cur);
      continue;
    }

    // Vowel -> Fricative
    if (v2f > 0.0 && tokIsVowelLike(prev) && tokIsFricativeLike(cur)) {
      cur.fadeMs = std::max(cur.fadeMs, v2f);
      clampFadeToDuration(cur);
      continue;
    }
  }

  return true;
}

}  // namespace nvsp_frontend::passes
