/*
TGSpeechBox — MITalk-style locus coarticulation pass.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

// =============================================================================
// Coarticulation Pass - locus-based START/END formant transitions
// =============================================================================
//
// Summary:
//   • For vowel-like segments, shift START formants toward a consonant-dependent
//     locus target.
//   • Keep END formants at the canonical vowel targets (via endCf1..3).
//   • DSP ramps cf/pf from start → end across the vowel frame.
//
// Locus targets (MITalk-style):
//   locus = src + k * (trg - src)
// where src are consonant formant targets, trg are vowel targets, and k≈0.42.
//
// Notes / design choices:
//   • We primarily modify the vowel (start targets), not the consonant.
//   • We still allow a small consonant-side adjustment for:
//       - velar fronting ("velar pinch") next to front vowels
//   • "Graduated" coarticulation reduces strength when consonants intervene.

#include "coarticulation.h"
#include "../pack.h"
#include "../ipa_engine.h"
#include <algorithm>
#include <cmath>

namespace nvsp_frontend::passes {

// -----------------------------------------------------------------------------
// Helper functions
// -----------------------------------------------------------------------------

static inline bool isVowel(const Token& t) {
  return t.def && ((t.def->flags & kIsVowel) != 0);
}

static inline bool isConsonant(const Token& t) {
  if (!t.def) return false;
  return (t.def->flags & kIsVowel) == 0;
}

static inline bool isStop(const Token& t) {
  return t.def && ((t.def->flags & kIsStop) != 0);
}

static inline bool isAfricate(const Token& t) {
  return t.def && ((t.def->flags & kIsAfricate) != 0);
}

static inline bool isFricative(const Token& t) {
  if (!t.def) return false;
  if (isStop(t) || isAfricate(t)) return false;
  const int idx = static_cast<int>(FieldId::fricationAmplitude);
  double fric = 0.0;
  if ((t.setMask & (1ULL << idx)) != 0) {
    fric = t.field[idx];
  } else if (t.def->setMask & (1ULL << idx)) {
    fric = t.def->field[idx];
  }
  return fric > 0.05;
}

static inline bool isSemivowel(const Token& t) {
  return t.def && ((t.def->flags & kIsSemivowel) != 0);
}

static inline bool isVowelLike(const Token& t) {
  return isVowel(t) || isSemivowel(t);
}

// Does this consonant trigger coarticulation on an adjacent vowel?
static inline bool triggersCoarticulation(const Token& t) {
  return isFricative(t) || isStop(t) || isAfricate(t);
}

// Place of articulation — shared enum/function now in pass_common.h.
// Local using-declaration for convenience.
using nvsp_frontend::Place;
using nvsp_frontend::getPlace;

// -----------------------------------------------------------------------------
// Locus values by place - now uses lang pack settings with fallback defaults
// -----------------------------------------------------------------------------

static double getLocusF2(Place place, const LanguagePack& lang) {
  switch (place) {
    case Place::Labial:   return lang.coarticulationLabialF2Locus > 0 ?
                                 lang.coarticulationLabialF2Locus : 800.0;
    case Place::Alveolar: return lang.coarticulationAlveolarF2Locus > 0 ?
                                 lang.coarticulationAlveolarF2Locus : 1800.0;
    case Place::Palatal:  return 2300.0;  // TODO: add to lang pack if needed
    case Place::Velar:    return lang.coarticulationVelarF2Locus > 0 ?
                                 lang.coarticulationVelarF2Locus : 1200.0;
    default:              return 0.0;
  }
}

// F1 locus is generally constricted (low) for all consonants
static double getLocusF1(Place place) {
  switch (place) {
    case Place::Labial:   return 300.0;
    case Place::Alveolar: return 350.0;
    case Place::Palatal:  return 280.0;
    case Place::Velar:    return 320.0;
    default:              return 0.0;
  }
}

// F3 locus isn't as cleanly "place-locked" as F2, but having a fallback helps
// avoid 0 values when doing MITalk-style interpolation.
static double getLocusF3(Place place, const LanguagePack& lang) {
  (void)lang;
  switch (place) {
    case Place::Labial:   return 2600.0;
    case Place::Alveolar: return 2600.0;
    case Place::Palatal:  return 2700.0;
    case Place::Velar:    return 2500.0;
    default:              return 0.0;
  }
}

// -----------------------------------------------------------------------------
// Field access helpers
// -----------------------------------------------------------------------------

static double getField(const Token& t, FieldId id) {
  const int idx = static_cast<int>(id);
  if ((t.setMask & (1ULL << idx)) != 0) {
    return t.field[idx];
  }
  if (t.def && (t.def->setMask & (1ULL << idx)) != 0) {
    return t.def->field[idx];
  }
  return 0.0;
}

static void setField(Token& t, FieldId id, double val) {
  const int idx = static_cast<int>(id);
  t.field[idx] = val;
  t.setMask |= (1ULL << idx);
}

static inline double clampPos(double v) {
  return (v > 0.0) ? v : 0.0;
}

// Prefer cascade formant if set; fall back to parallel; else 0.
static double getCanonicalFormant(const Token& t, FieldId cf, FieldId pf) {
  double v = getField(t, cf);
  if (v <= 0.0) v = getField(t, pf);
  return v;
}

// For consonant "src" formants: prefer cf/pf, then fall back to place locus.
static double getConsonantSrcFormant(const Token& c, FieldId cf, FieldId pf, double placeFallback) {
  double v = getField(c, cf);
  if (v <= 0.0) v = getField(c, pf);
  if (v <= 0.0) v = placeFallback;
  return v;
}

// Per-place-of-articulation strength multiplier.
static double getPlaceScale(Place place, const LanguagePack& lang) {
  switch (place) {
    case Place::Labial:   return lang.coarticulationLabialScale;
    case Place::Alveolar: return lang.coarticulationAlveolarScale;
    case Place::Palatal:  return lang.coarticulationPalatalScale;
    case Place::Velar:    return lang.coarticulationVelarScale;
    default:              return 1.0;
  }
}

// MITalk-style locus target.
static double mitalkLocus(double src, double trg, double k) {
  if (src <= 0.0 || trg <= 0.0) return 0.0;
  return src + k * (trg - src);
}

// -----------------------------------------------------------------------------
// Velar pinch (modify consonant formants before front vowels)
// -----------------------------------------------------------------------------

static bool isFrontVowel(double f2, const LanguagePack& lang) {
  // Use the velar pinch threshold as the general "front vowel" cutoff, with
  // a safe fallback.
  const double thr = (lang.coarticulationVelarPinchThreshold > 0.0)
                         ? lang.coarticulationVelarPinchThreshold
                         : 1600.0;
  return f2 > thr;
}

static void applyVelarPinch(Token& c, const Token& vowel, 
                            const LanguagePack& lang, double strength) {
  double vowelF2 = getField(vowel, FieldId::cf2);
  if (vowelF2 <= 0.0) vowelF2 = getField(vowel, FieldId::pf2);
  
  if (!isFrontVowel(vowelF2, lang)) {
    return;  // Back vowel - no pinch
  }
  
  // Front vowel - F2 and F3 converge
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

// =============================================================================
// Main coarticulation pass - DECTalk-style START/END transitions
// =============================================================================

bool runCoarticulation(PassContext& ctx, std::vector<Token>& tokens, std::string& outError) {
  (void)outError;

  const auto& lang = ctx.pack.lang;
  if (!lang.coarticulationEnabled) return true;

  const double strength = std::clamp(lang.coarticulationStrength, 0.0, 1.0);
  if (strength <= 0.0) return true;

  // Rate-adaptive attenuation: at high speeds, reduce coarticulation so
  // resonators spend more time at canonical vowel targets.
  double rateScale = 1.0;
  const double hrThreshold = lang.highRateThreshold;
  if (hrThreshold > 0.0 && ctx.speed > hrThreshold) {
    const double ceiling = hrThreshold * 2.5;
    const double t = std::clamp(
        (ctx.speed - hrThreshold) / (ceiling - hrThreshold), 0.0, 1.0);
    rateScale = 1.0 - t * (1.0 - std::clamp(lang.highRateCoarticulationFloor, 0.0, 1.0));
  }
  const double rateAdjustedStrength = strength * rateScale;

  for (size_t i = 0; i < tokens.size(); ++i) {
    Token& t = tokens[i];
    if (t.silence) continue;

    // ----- Consonant-side tweaks (allowed exceptions) -----
    // 1) Velar pinch (modify velar consonant before front vowels)
    if (isConsonant(t) && t.def) {
      Place place = getPlace(t.def->key);
      if (place == Place::Velar && lang.coarticulationVelarPinchEnabled) {
        // Find vowel to the right
        for (size_t j = i + 1; j < tokens.size(); ++j) {
          const Token& next = tokens[j];
          if (next.silence) continue;
          if (isVowelLike(next)) {
            applyVelarPinch(t, next, lang, rateAdjustedStrength);
          }
          break;
        }
      }
    }

    // ----- Vowel coarticulation (locus-based start/end) -----
    if (!isVowelLike(t)) continue;
    // Svarabhakti vocoids already carry their inherited transition quality.
    if (t.svarabhaktiShaped) continue;
    
    // Find the nearest *triggering* consonant to the left (for standard locus).
    const Token* triggerCons = nullptr;
    Place triggerPlace = Place::Unknown;
    int consonantDistance = 0;  // # of intervening consonants between vowel and trigger

    const int maxAdj = std::max(0, (int)std::round(lang.coarticulationAdjacencyMaxConsonants));
    int nonTriggerCount = 0;

    for (size_t j = i; j > 0; --j) {
      const Token& prev = tokens[j - 1];
      if (prev.silence) break;          // Don't coarticulate across explicit silence.
      if (isVowelLike(prev)) break;     // Stop at previous vowel nucleus.
      if (!isConsonant(prev) || !prev.def) continue;

      if (triggersCoarticulation(prev)) {
        triggerCons = &prev;
        triggerPlace = getPlace(prev.def->key);
        consonantDistance = nonTriggerCount;
        break;
      }

      nonTriggerCount++;
      if (maxAdj > 0 && nonTriggerCount > maxAdj) break;
    }

    // Canonical vowel targets.
    const double vF1 = getCanonicalFormant(t, FieldId::cf1, FieldId::pf1);
    const double vF2 = getCanonicalFormant(t, FieldId::cf2, FieldId::pf2);
    const double vF3 = getCanonicalFormant(t, FieldId::cf3, FieldId::pf3);
    if (vF2 <= 0.0) continue;

    const Token* leftCons = triggerCons;
    Place leftPlace = triggerPlace;

    if (!leftCons || leftPlace == Place::Unknown) continue;
    
    // Apply graduated strength falloff if enabled (clusters / non-adjacent triggers).
    double effectiveStrength = rateAdjustedStrength;
    if (lang.coarticulationGraduated && consonantDistance > 0) {
      const int maxCons = std::max(1, (int)std::round(lang.coarticulationAdjacencyMaxConsonants));
      const double df = std::pow(0.6, std::min(consonantDistance, maxCons));
      effectiveStrength *= df;
    }

    // Per-place scaling: labials default weaker because lip rounding is
    // relatively independent of tongue body position.
    effectiveStrength *= getPlaceScale(leftPlace, lang);

    // Cross-syllable coarticulation: weaker pull when consonant and vowel
    // are in different syllables — they're separate articulatory gestures.
    if (lang.coarticulationCrossSyllableScale < 1.0 &&
        leftCons->syllableIndex >= 0 && t.syllableIndex >= 0 &&
        leftCons->syllableIndex != t.syllableIndex) {
      effectiveStrength *= lang.coarticulationCrossSyllableScale;
    }

    effectiveStrength = std::clamp(effectiveStrength, 0.0, 1.0);
    if (effectiveStrength <= 0.0) continue;

    // Consonant "src" formants.
    const double srcF1 = getConsonantSrcFormant(*leftCons, FieldId::cf1, FieldId::pf1, getLocusF1(leftPlace));
    double srcF2 = getConsonantSrcFormant(*leftCons, FieldId::cf2, FieldId::pf2, getLocusF2(leftPlace, lang));
    const double srcF3 = getConsonantSrcFormant(*leftCons, FieldId::cf3, FieldId::pf3, getLocusF3(leftPlace, lang));

    // Velar locus is context-dependent: high before front vowels ("geese"),
    // low before back vowels ("go").  Without this, the phoneme's cf2=1800
    // always wins and velars sound identical to alveolars before back vowels.
    if (leftPlace == Place::Velar) {
      if (vF2 > 1600.0 && lang.coarticulationVelarF2LocusFront > 0.0) {
        srcF2 = lang.coarticulationVelarF2LocusFront;
      } else if (vF2 <= 1600.0 && lang.coarticulationVelarF2LocusBack > 0.0) {
        srcF2 = lang.coarticulationVelarF2LocusBack;
      }
    }

    // MITalk locus targets.
    const double k = std::clamp(lang.coarticulationMitalkK, 0.0, 1.0);
    const double locusF2 = mitalkLocus(srcF2, vF2, k);
    const double locusF1 = mitalkLocus(srcF1, vF1, k);
    const double locusF3 = mitalkLocus(srcF3, vF3, k);
    if (locusF2 <= 0.0) continue;

    // Per-formant scaling: F2 is the main perceptual cue; keep F1/F3 gentler.
    const double f2Scale = std::clamp(lang.coarticulationF2Scale, 0.0, 2.0);
    const double f1Scale = std::clamp(lang.coarticulationF1Scale, 0.0, 2.0);
    const double f3Scale = std::clamp(lang.coarticulationF3Scale, 0.0, 2.0);

    const double startF2 = vF2 + (locusF2 - vF2) * (effectiveStrength * f2Scale);
    double startF1 = vF1;
    if (vF1 > 0.0 && locusF1 > 0.0) {
      startF1 = vF1 + (locusF1 - vF1) * (effectiveStrength * f1Scale);
    }
    double startF3 = vF3;
    if (vF3 > 0.0 && locusF3 > 0.0) {
      startF3 = vF3 + (locusF3 - vF3) * (effectiveStrength * f3Scale);
    }
    
    // Set START formants (the token's main formant values)
    setField(t, FieldId::cf2, startF2);
    setField(t, FieldId::pf2, startF2);
    if (vF1 > 0.0) {
      setField(t, FieldId::cf1, startF1);
      setField(t, FieldId::pf1, startF1);
    }
    if (vF3 > 0.0) {
      setField(t, FieldId::cf3, startF3);
      setField(t, FieldId::pf3, startF3);
    }
    
    // Canonical steady targets for steady-state emission: field[cf*] now
    // holds the locus-shifted onset, so the canonical values must be saved
    // before the VC block (or nothing) overwrites endCf* — otherwise the
    // vowel never renders its own formants (#113).
    if (lang.coarticulationSteadyState) {
      t.hasCoarticSteady = true;
      t.steadyF1 = (vF1 > 0.0) ? vF1 : 0.0;
      t.steadyF2 = vF2;
      t.steadyF3 = (vF3 > 0.0) ? vF3 : 0.0;
    }

    // Set END formants (DSP will ramp from start to end)
    // Only set if there's meaningful movement (avoid overhead for tiny changes)
    if (std::abs(vF2 - startF2) > 10.0) {
      t.hasEndCf2 = true;
      t.endCf2 = vF2;
    }
    if (vF1 > 0.0 && std::abs(vF1 - startF1) > 8.0) {
      t.hasEndCf1 = true;
      t.endCf1 = vF1;
    }
    if (vF3 > 0.0 && std::abs(vF3 - startF3) > 12.0) {
      t.hasEndCf3 = true;
      t.endCf3 = vF3;
    }

    // ----- Aspiration coarticulation -----
    // Post-stop aspiration sits between the stop burst and the vowel.
    // Without shaping, it has generic /h/ formants — a spectral hole in the
    // C→V transition.  In natural speech, aspiration formants ramp from
    // near the stop's place of articulation toward the vowel target.
    //
    // We compute locus-based targets (where aspiration SHOULD be at full
    // strength) and blend from the aspiration's ORIGINAL /h/ formants toward
    // those targets.  This way at low coarticulation strength, aspiration
    // stays near its canonical values instead of getting dragged to the
    // stop's locus (which for labials = F2≈800 = sounds like /w/).
    const double aspBlendStart = std::clamp(lang.coarticulationAspirationBlendStart, 0.0, 1.0);
    const double aspBlendEnd   = std::clamp(lang.coarticulationAspirationBlendEnd,   0.0, 1.0);

    if (effectiveStrength > 0.0) {
      for (size_t j = i; j > 0; --j) {
        Token& prev = tokens[j - 1];
        if (prev.silence) break;
        if (isVowelLike(prev)) break;
        if (!prev.postStopAspiration) continue;

        // Read aspiration's original formants BEFORE overwriting.
        const double origF1 = getCanonicalFormant(prev, FieldId::cf1, FieldId::pf1);
        const double origF2 = getCanonicalFormant(prev, FieldId::cf2, FieldId::pf2);
        const double origF3 = getCanonicalFormant(prev, FieldId::cf3, FieldId::pf3);

        // Locus-based targets at full strength (where aspiration should be).
        // Start = closer to stop, end = closer to vowel.
        const double targetStartF2 = srcF2 + aspBlendStart * (vF2 - srcF2);
        const double targetEndF2   = srcF2 + aspBlendEnd   * (vF2 - srcF2);

        // Blend from original toward targets by effectiveStrength.
        // At strength=0: keep original /h/.  At strength=1: full locus trajectory.
        if (origF2 > 0.0) {
          const double aStartF2 = origF2 + effectiveStrength * (targetStartF2 - origF2);
          const double aEndF2   = origF2 + effectiveStrength * (targetEndF2   - origF2);
          setField(prev, FieldId::cf2, aStartF2);
          setField(prev, FieldId::pf2, aStartF2);
          if (std::abs(aEndF2 - aStartF2) > 10.0) {
            prev.hasEndCf2 = true;
            prev.endCf2 = aEndF2;
          }
        }

        if (srcF1 > 0.0 && vF1 > 0.0 && origF1 > 0.0) {
          const double targetStartF1 = srcF1 + aspBlendStart * (vF1 - srcF1);
          const double targetEndF1   = srcF1 + aspBlendEnd   * (vF1 - srcF1);
          const double aStartF1 = origF1 + effectiveStrength * (targetStartF1 - origF1);
          const double aEndF1   = origF1 + effectiveStrength * (targetEndF1   - origF1);
          setField(prev, FieldId::cf1, aStartF1);
          setField(prev, FieldId::pf1, aStartF1);
          if (std::abs(aEndF1 - aStartF1) > 8.0) {
            prev.hasEndCf1 = true;
            prev.endCf1 = aEndF1;
          }
        }

        if (srcF3 > 0.0 && vF3 > 0.0 && origF3 > 0.0) {
          const double targetStartF3 = srcF3 + aspBlendStart * (vF3 - srcF3);
          const double targetEndF3   = srcF3 + aspBlendEnd   * (vF3 - srcF3);
          const double aStartF3 = origF3 + effectiveStrength * (targetStartF3 - origF3);
          const double aEndF3   = origF3 + effectiveStrength * (targetEndF3   - origF3);
          setField(prev, FieldId::cf3, aStartF3);
          setField(prev, FieldId::pf3, aStartF3);
          if (std::abs(aEndF3 - aStartF3) > 12.0) {
            prev.hasEndCf3 = true;
            prev.endCf3 = aEndF3;
          }
        }

        break;  // Only shape the nearest aspiration token.
      }
    }
  }

  // ----- VC half: vowel END formants toward the FOLLOWING consonant -----
  // (#108, 2026-08-21.)  The loop above covers only the consonant→vowel
  // (CV) cue: a vowel never announces the consonant that follows it, so
  // cluster-internal and intervocalic consonants (Spanish "regla" /eɣl/,
  // "amigo" /iɣo/) lose their strongest place cue — the VC formant ramp.
  // Runs as a second pass so it composes with (and takes precedence over)
  // the canonical end targets the CV block set.  Gated per-language;
  // packs that don't opt in are bit-identical.  Root-caused jointly with
  // a second-model review pass; same locus/scale conventions as CV above.
  if (lang.coarticulationFadeIntoConsonants) {
    const double k = std::clamp(lang.coarticulationMitalkK, 0.0, 1.0);
    const double f1Scale = std::clamp(lang.coarticulationF1Scale, 0.0, 2.0);
    const double f2Scale = std::clamp(lang.coarticulationF2Scale, 0.0, 2.0);
    const double f3Scale = std::clamp(lang.coarticulationF3Scale, 0.0, 2.0);

    double rateScale = 1.0;
    const double hrThreshold = lang.highRateThreshold;
    if (hrThreshold > 0.0 && ctx.speed > hrThreshold) {
      const double ceiling = hrThreshold * 2.5;
      const double tt = std::clamp(
          (ctx.speed - hrThreshold) / (ceiling - hrThreshold), 0.0, 1.0);
      rateScale = 1.0 - tt * (1.0 - std::clamp(lang.highRateCoarticulationFloor, 0.0, 1.0));
    }
    const double rateAdjustedStrength =
        std::clamp(lang.coarticulationStrength, 0.0, 1.0) * rateScale;

    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
      Token& t = tokens[i];
      if (t.silence || !isVowelLike(t)) continue;
      if (t.svarabhaktiShaped) continue;

      // Immediately-adjacent following consonant only — conservative on
      // purpose; distant anticipation can wait for listening evidence.
      const Token* rightCons = nullptr;
      Place rightPlace = Place::Unknown;
      for (size_t j = i + 1; j < tokens.size(); ++j) {
        const Token& next = tokens[j];
        if (next.silence || isVowelLike(next)) break;
        if (!isConsonant(next) || !next.def) continue;
        if (triggersCoarticulation(next)) {
          rightCons = &next;
          rightPlace = getPlace(next.def->key);
        }
        break;
      }
      if (!rightCons || rightPlace == Place::Unknown) continue;

      double rightStrength = rateAdjustedStrength * getPlaceScale(rightPlace, lang);
      if (lang.coarticulationCrossSyllableScale < 1.0 &&
          rightCons->syllableIndex >= 0 && t.syllableIndex >= 0 &&
          rightCons->syllableIndex != t.syllableIndex) {
        rightStrength *= lang.coarticulationCrossSyllableScale;
      }
      rightStrength = std::clamp(rightStrength, 0.0, 1.0);
      if (rightStrength <= 0.0) continue;

      // The vowel's intended END values: whatever the CV block set as the
      // canonical end (if it ran for this vowel), else the current fields.
      const double vF1 = t.hasEndCf1 ? t.endCf1
                                     : getCanonicalFormant(t, FieldId::cf1, FieldId::pf1);
      const double vF2 = t.hasEndCf2 ? t.endCf2
                                     : getCanonicalFormant(t, FieldId::cf2, FieldId::pf2);
      const double vF3 = t.hasEndCf3 ? t.endCf3
                                     : getCanonicalFormant(t, FieldId::cf3, FieldId::pf3);
      if (vF2 <= 0.0) continue;

      const double rSrcF1 = getConsonantSrcFormant(*rightCons, FieldId::cf1, FieldId::pf1, getLocusF1(rightPlace));
      double rSrcF2 = getConsonantSrcFormant(*rightCons, FieldId::cf2, FieldId::pf2, getLocusF2(rightPlace, lang));
      const double rSrcF3 = getConsonantSrcFormant(*rightCons, FieldId::cf3, FieldId::pf3, getLocusF3(rightPlace, lang));

      // Velar locus is context-dependent, same convention as the CV block.
      if (rightPlace == Place::Velar) {
        if (vF2 > 1600.0 && lang.coarticulationVelarF2LocusFront > 0.0) {
          rSrcF2 = lang.coarticulationVelarF2LocusFront;
        } else if (vF2 <= 1600.0 && lang.coarticulationVelarF2LocusBack > 0.0) {
          rSrcF2 = lang.coarticulationVelarF2LocusBack;
        }
      }

      const double locusF2 = mitalkLocus(rSrcF2, vF2, k);
      if (locusF2 <= 0.0) continue;

      // VC-only case (CV block didn't run for this vowel): vF* are still
      // the canonical targets — save them as the steady state before the
      // fade overwrites endCf* below.
      if (lang.coarticulationSteadyState && !t.hasCoarticSteady) {
        t.hasCoarticSteady = true;
        t.steadyF1 = (vF1 > 0.0) ? vF1 : 0.0;
        t.steadyF2 = vF2;
        t.steadyF3 = (vF3 > 0.0) ? vF3 : 0.0;
      }

      const double endF2 = vF2 + (locusF2 - vF2) * (rightStrength * f2Scale);
      if (std::abs(endF2 - vF2) > 10.0) { t.hasEndCf2 = true; t.endCf2 = endF2; }
      if (vF1 > 0.0) {
        const double locusF1 = mitalkLocus(rSrcF1, vF1, k);
        if (locusF1 > 0.0) {
          const double endF1 = vF1 + (locusF1 - vF1) * (rightStrength * f1Scale);
          if (std::abs(endF1 - vF1) > 8.0) { t.hasEndCf1 = true; t.endCf1 = endF1; }
        }
      }
      if (vF3 > 0.0) {
        const double locusF3 = mitalkLocus(rSrcF3, vF3, k);
        if (locusF3 > 0.0) {
          const double endF3 = vF3 + (locusF3 - vF3) * (rightStrength * f3Scale);
          if (std::abs(endF3 - vF3) > 12.0) { t.hasEndCf3 = true; t.endCf3 = endF3; }
        }
      }
    }
  }

  return true;
}

}  // namespace nvsp_frontend::passes
