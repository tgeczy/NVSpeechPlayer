#include "coarticulation.h"

#include <algorithm>
#include <cmath>

namespace nvsp_frontend::passes {

namespace {

// -----------------------------------------------------------------------------
// Field helpers
// -----------------------------------------------------------------------------

static inline bool hasField(const Token& t, FieldId id) {
  return (t.setMask & (1ULL << static_cast<int>(id))) != 0;
}

static inline double getField(const Token& t, FieldId id) {
  // Returns the token's field value if set, otherwise the def's value.
  const int idx = static_cast<int>(id);
  if (hasField(t, id)) {
    return t.field[idx];
  }
  // Fall back to phoneme def if it has the field.
  if (t.def && (t.def->setMask & (1ULL << idx))) {
    return t.def->field[idx];
  }
  return 0.0;
}

static inline void setField(Token& t, FieldId id, double value) {
  // Set the field value AND mark it in setMask so emitFrames will use it.
  const int idx = static_cast<int>(id);
  t.field[idx] = value;
  t.setMask |= (1ULL << idx);
}

// -----------------------------------------------------------------------------
// Phoneme classification
// -----------------------------------------------------------------------------

static inline bool isVowel(const Token& t) {
  return t.def && ((t.def->flags & kIsVowel) != 0);
}

static inline bool isSilenceOrMissing(const Token& t) {
  return t.silence || !t.def;
}

static inline bool isConsonant(const Token& t) {
  if (!t.def) return false;
  // Anything that's not a vowel and not silence.
  return (t.def->flags & kIsVowel) == 0;
}

static inline bool isStopLike(const Token& t) {
  if (!t.def) return false;
  const uint32_t f = t.def->flags;
  return ((f & kIsStop) != 0) || ((f & kIsAfricate) != 0);
}

static inline bool isNasal(const Token& t) {
  return t.def && ((t.def->flags & kIsNasal) != 0);
}

static inline bool isLiquid(const Token& t) {
  return t.def && ((t.def->flags & kIsLiquid) != 0);
}

static inline bool isSemivowel(const Token& t) {
  return t.def && ((t.def->flags & kIsSemivowel) != 0);
}

static inline bool isVowelLike(const Token& t) {
  return isVowel(t) || isSemivowel(t);
}

// -----------------------------------------------------------------------------
// Place of articulation detection
// -----------------------------------------------------------------------------

enum class PlaceOfArticulation {
  Unknown,
  Labial,
  Alveolar,
  Velar,
  // Could add: Palatal, Glottal, etc.
};

static PlaceOfArticulation getPlaceOfArticulation(const std::u32string& key) {
  // Labials
  if (key == U"p" || key == U"b" || key == U"m" ||
      key == U"f" || key == U"v" || key == U"w" ||
      key == U"ʍ") {
    return PlaceOfArticulation::Labial;
  }
  
  // Alveolars
  if (key == U"t" || key == U"d" || key == U"n" ||
      key == U"s" || key == U"z" || key == U"l" ||
      key == U"r" || key == U"ɾ" || key == U"ɹ" ||
      key == U"ɬ" || key == U"ɮ") {
    return PlaceOfArticulation::Alveolar;
  }
  
  // Velars
  if (key == U"k" || key == U"g" || key == U"ŋ" ||
      key == U"x" || key == U"ɣ") {
    return PlaceOfArticulation::Velar;
  }
  
  return PlaceOfArticulation::Unknown;
}

// -----------------------------------------------------------------------------
// Vowel lookup helpers
// -----------------------------------------------------------------------------

struct VowelHit {
  const Token* tok = nullptr;
  int consonantsAway = 0;  // 0 = immediately adjacent (ignoring silences)
};

static VowelHit findNearestVowelLeft(
    const std::vector<Token>& tokens,
    size_t i,
    bool crossWord,
    int maxConsonants) {
  VowelHit out;
  int cons = 0;

  for (size_t j = i; j > 0; --j) {
    const Token& prev = tokens[j - 1];

    // Silence breaks coarticulation context.
    if (isSilenceOrMissing(prev)) break;

    if (isVowelLike(prev)) {
      out.tok = &prev;
      out.consonantsAway = cons;
      return out;
    }

    // Consonant.
    cons += 1;
    if (cons > maxConsonants) break;

    // If we've reached the start of the current word, don't cross further.
    if (!crossWord && prev.wordStart) break;
  }

  return out;
}

static VowelHit findNearestVowelRight(
    const std::vector<Token>& tokens,
    size_t i,
    bool crossWord,
    int maxConsonants) {
  VowelHit out;
  int cons = 0;

  for (size_t j = i + 1; j < tokens.size(); ++j) {
    const Token& next = tokens[j];

    // Silence breaks coarticulation context.
    if (isSilenceOrMissing(next)) break;

    // Stop at word boundary unless explicitly crossing.
    if (!crossWord && next.wordStart) break;

    if (isVowelLike(next)) {
      out.tok = &next;
      out.consonantsAway = cons;
      return out;
    }

    // Consonant.
    cons += 1;
    if (cons > maxConsonants) break;
  }

  return out;
}

static inline double hitWeight(const VowelHit& h) {
  if (!h.tok) return 0.0;
  return 1.0 / (static_cast<double>(h.consonantsAway) + 1.0);
}

// -----------------------------------------------------------------------------
// Core coarticulation logic
// -----------------------------------------------------------------------------

static void applyLocusShift(
    Token& c,
    FieldId formantId,
    double locus,
    double strength,
    const Token* adjacentVowel) {
  
  // Get the current formant value (from token or def).
  double current = getField(c, formantId);
  
  // If the consonant has no formant value at all, use the locus as a starting point.
  // This handles stops that only have parallel (burst) formants defined.
  if (current <= 0.0) {
    // Try to get a reasonable starting value from the adjacent vowel.
    if (adjacentVowel) {
      current = getField(*adjacentVowel, formantId);
    }
    // If still zero, use the locus itself as the base.
    if (current <= 0.0) {
      current = locus;
    }
  }
  
  // Interpolate toward the locus.
  double shifted = current + (locus - current) * strength;
  
  // Write back with setMask so emitFrames will use it.
  setField(c, formantId, shifted);
}

static void applyVelarPinch(
    Token& c,
    const Token& nextVowel,
    const LanguagePack& lang,
    double strength) {

  strength = std::clamp(strength, 0.0, 1.0);
  if (strength <= 0.0) return;
  
  // Velar pinch: before front vowels, F2 and F3 converge.
  // This makes /ki/ sound different from /ku/.
  double vowelF2 = getField(nextVowel, FieldId::cf2);
  if (vowelF2 <= 0.0) {
    vowelF2 = getField(nextVowel, FieldId::pf2);
  }
  
  if (vowelF2 < lang.coarticulationVelarPinchThreshold) {
    // Back vowel - no pinch needed.
    return;
  }
  
  // Front vowel - apply pinch.
  const double pinchF2 = vowelF2 * lang.coarticulationVelarPinchF2Scale;
  const double pinchF3 = lang.coarticulationVelarPinchF3;

  auto blendToward = [&](FieldId id, double target) {
    double cur = getField(c, id);
    if (cur <= 0.0) cur = target;
    setField(c, id, cur + (target - cur) * strength);
  };

  blendToward(FieldId::cf2, pinchF2);
  blendToward(FieldId::pf2, pinchF2);

  if (pinchF3 > 0.0) {
    blendToward(FieldId::cf3, pinchF3);
    blendToward(FieldId::pf3, pinchF3);
  }
}

}  // namespace

bool runCoarticulation(PassContext& ctx, std::vector<Token>& tokens, std::string& outError) {
  (void)outError;

  const auto& lang = ctx.pack.lang;
  if (!lang.coarticulationEnabled) return true;

  const double strength = std::clamp(lang.coarticulationStrength, 0.0, 1.0);
  if (strength <= 0.0) return true;
  
  const double extent = std::clamp(lang.coarticulationTransitionExtent, 0.0, 1.0);

  for (size_t i = 0; i < tokens.size(); ++i) {
    Token& c = tokens[i];
    if (isSilenceOrMissing(c)) continue;
    if (!isConsonant(c)) continue;  // Only coarticulate consonants.

    const std::u32string& key = c.def->key;
    PlaceOfArticulation place = getPlaceOfArticulation(key);
    
    if (place == PlaceOfArticulation::Unknown) {
      // No locus data for this consonant - skip.
      continue;
    }

    // Determine F2 locus based on place of articulation.
    double locusF2 = 0.0;
    switch (place) {
      case PlaceOfArticulation::Labial:
        locusF2 = lang.coarticulationLabialF2Locus;
        break;
      case PlaceOfArticulation::Alveolar:
        locusF2 = lang.coarticulationAlveolarF2Locus;
        break;
      case PlaceOfArticulation::Velar:
        locusF2 = lang.coarticulationVelarF2Locus;
        break;
      default:
        continue;
    }

    // Find nearby vowel-like segments for context.
    //
    // When coarticulationGraduated is enabled, we scale the strength based on
    // how "close" the nearest vowel is. This avoids a hard on/off feeling in
    // clusters.
    const int maxCons = std::clamp(
        static_cast<int>(std::round(lang.coarticulationAdjacencyMaxConsonants)),
        0,
        6);
    const VowelHit left = findNearestVowelLeft(tokens, i, /*crossWord=*/false, maxCons);
    const VowelHit right = findNearestVowelRight(tokens, i, /*crossWord=*/false, maxCons);

    double w = 1.0;
    if (lang.coarticulationGraduated) {
      w = std::max(hitWeight(left), hitWeight(right));
      if (w <= 0.0) {
        // No nearby vowel => don't apply locus shaping.
        continue;
      }
    }

    const double effStrength = strength * std::clamp(w, 0.0, 1.0);

    // Prefer the nearest vowel, biasing to the right (anticipatory) on ties.
    const Token* adjacentVowel = nullptr;
    if (right.tok && (!left.tok || right.consonantsAway <= left.consonantsAway)) {
      adjacentVowel = right.tok;
    } else {
      adjacentVowel = left.tok;
    }

    // Special case: velar pinch before *immediately-adjacent* front vowel-like.
    if (place == PlaceOfArticulation::Velar &&
        lang.coarticulationVelarPinchEnabled &&
        right.tok &&
        right.consonantsAway == 0) {
      applyVelarPinch(c, *right.tok, lang, effStrength);
    } else {
      // Normal locus-based coarticulation.
      // Apply to both cascade and parallel F2 (whichever is active).
      applyLocusShift(c, FieldId::cf2, locusF2, effStrength, adjacentVowel);
      applyLocusShift(c, FieldId::pf2, locusF2, effStrength, adjacentVowel);
    }

    // Optional: longer fade INTO consonants for smoother transitions.
    if (lang.coarticulationFadeIntoConsonants && extent > 0.0 && c.durationMs > 0.0) {
      double minFade = c.durationMs * extent;

      // If we're using graduated coarticulation, don't force a large fade when
      // there's no close vowel context.
      if (lang.coarticulationGraduated) {
        minFade *= std::clamp(w, 0.0, 1.0);
      }
      
      // Keep word-initial consonants crisper.
      if (c.wordStart) {
        minFade *= lang.coarticulationWordInitialFadeScale;
      }
      
      c.fadeMs = std::max(c.fadeMs, minFade);
      
      // Don't let fade exceed duration.
      if (c.fadeMs > c.durationMs) {
        c.fadeMs = c.durationMs;
      }
    }
  }

  return true;
}

}  // namespace nvsp_frontend::passes
