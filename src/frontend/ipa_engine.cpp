/*
TGSpeechBox — IPA-to-frame conversion engine.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include "ipa_engine.h"

#include "ipa_internal.h"
#include "ipa_normalize.h"
#include "ipa_parser.h"

#include "passes/pass_pipeline.h"
#include "passes/pitch_common.h"
#include "passes/pitch_espeak.h"
#include "passes/pitch_fujisaki.h"
#include "passes/pitch_impulse.h"
#include "passes/pitch_klatt.h"
#include "passes/pitch_legacy.h"
#include "voice_profile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <sstream>

namespace nvsp_frontend {

using ipa_internal::isTieBar;
using ipa_internal::findPhoneme;
using ipa_internal::tokenIsVoiced;
using ipa_internal::tokenIsStop;
using ipa_internal::tokenIsAfricate;
using ipa_internal::tokenIsTap;
using ipa_internal::tokenIsNasal;
using ipa_internal::getFieldOrZero;
using ipa_internal::tokenIsFricativeLike;

static inline bool hasFlag(const PhonemeDef* def, std::uint32_t bit) {
  return def && ((def->flags & bit) != 0);
}

static void correctCopyAdjacent(std::vector<Token>& tokens) {
  const int n = static_cast<int>(tokens.size());
  for (int i = 0; i < n; ++i) {
    Token& cur = tokens[i];
    if (!cur.def) continue;
    if ((cur.def->flags & kCopyAdjacent) == 0) continue;

    // Find adjacent real phoneme that has actual formant values.
    // Skip other _copyAdjacent phonemes since they won't have formants yet!
    const Token* adjacent = nullptr;
    for (int j = i + 1; j < n; ++j) {
      if (tokens[j].def && !tokens[j].silence) {
        // Skip phonemes that also use _copyAdjacent — they don't have real formants
        if ((tokens[j].def->flags & kCopyAdjacent) != 0) continue;
        adjacent = &tokens[j];
        break;
      }
    }
    if (!adjacent) {
      for (int j = i - 1; j >= 0; --j) {
        if (tokens[j].def && !tokens[j].silence) {
          // Skip phonemes that also use _copyAdjacent
          if ((tokens[j].def->flags & kCopyAdjacent) != 0) continue;
          adjacent = &tokens[j];
          break;
        }
      }
    }
    if (!adjacent) continue;

    for (int f = 0; f < kFrameFieldCount; ++f) {
      const std::uint64_t bit = (1ull << f);
      if ((cur.setMask & bit) == 0 && (adjacent->setMask & bit) != 0) {
        cur.field[f] = adjacent->field[f];
        cur.setMask |= bit;
      }
    }
  }
}

static void applyTransforms(const LanguagePack& lang, std::vector<Token>& tokens) {
  for (Token& t : tokens) {
    if (!t.def || t.silence) continue;

    const bool isVowel = tokenIsVowel(t);
    const bool isVoiced = tokenIsVoiced(t);
    const bool isStop = tokenIsStop(t);
    const bool isAfricate = tokenIsAfricate(t);
    const bool isNasal = tokenIsNasal(t);
    const bool isLiquid = tokenIsLiquid(t);
    const bool isSemivowel = tokenIsSemivowel(t);
    const bool isTap = tokenIsTap(t);
    const bool isTrill = tokenIsTrill(t);
    const bool isFricLike = tokenIsFricativeLike(t);

    for (const TransformRule& tr : lang.transforms) {
      auto matchTri = [](int want, bool have) {
        return (want < 0) || (want == (have ? 1 : 0));
      };
      if (!matchTri(tr.isVowel, isVowel)) continue;
      if (!matchTri(tr.isVoiced, isVoiced)) continue;
      if (!matchTri(tr.isStop, isStop)) continue;
      if (!matchTri(tr.isAfricate, isAfricate)) continue;
      if (!matchTri(tr.isNasal, isNasal)) continue;
      if (!matchTri(tr.isLiquid, isLiquid)) continue;
      if (!matchTri(tr.isSemivowel, isSemivowel)) continue;
      if (!matchTri(tr.isTap, isTap)) continue;
      if (!matchTri(tr.isTrill, isTrill)) continue;
      if (!matchTri(tr.isFricativeLike, isFricLike)) continue;

      // set
      for (const auto& kv : tr.set) {
        int idx = static_cast<int>(kv.first);
        t.field[idx] = kv.second;
        t.setMask |= (1ull << idx);
      }

      // scale
      for (const auto& kv : tr.scale) {
        int idx = static_cast<int>(kv.first);
        std::uint64_t bit = (1ull << idx);
        if ((t.setMask & bit) == 0) continue;
        t.field[idx] *= kv.second;
      }

      // add
      for (const auto& kv : tr.add) {
        int idx = static_cast<int>(kv.first);
        std::uint64_t bit = (1ull << idx);
        if ((t.setMask & bit) == 0) continue;
        t.field[idx] += kv.second;
      }
    }
  }
}

static void calculateTimes(std::vector<Token>& tokens, const PackSet& pack, double baseSpeed) {
  const LanguagePack& lang = pack.lang;
  Token* last = nullptr;
  int syllableStress = 0;
  double curSpeed = baseSpeed;

  for (size_t i = 0; i < tokens.size(); ++i) {
    Token& t = tokens[i];
    Token* next = (i + 1 < tokens.size()) ? &tokens[i + 1] : nullptr;

    if (t.syllableStart) {
      syllableStress = t.stress;
      if (lang.prominenceEnabled) {
        // When the prominence pass handles stress realization,
        // skip the old stressDiv logic to avoid double-lengthening.
        curSpeed = baseSpeed;
      } else if (syllableStress == 1) {
        curSpeed = baseSpeed / lang.primaryStressDiv;
      } else if (syllableStress == 2) {
        curSpeed = baseSpeed / lang.secondaryStressDiv;
      } else {
        curSpeed = baseSpeed;
      }
    }

    // Use configurable defaults instead of hardcoded magic numbers
    double dur = lang.defaultVowelDurationMs / curSpeed;
    double fade = lang.defaultFadeMs / curSpeed;

    if (t.vowelHiatusGap) {
      dur = lang.stressedVowelHiatusGapMs / baseSpeed;
      fade = lang.stressedVowelHiatusFadeMs / baseSpeed;
    } else if (t.preStopGap) {
      double closureScale = (t.def && t.def->hasDurationScale) ? t.def->durationScale : 1.0;

      if (t.clusterGap) {
        double baseDur = lang.stopClosureClusterGapMs * closureScale;
        double baseFade = lang.stopClosureClusterFadeMs;

        // Dedicated nasal→stop gap: use shorter timing to avoid clicks
        // while still protecting the stop burst from being swallowed.
        if (t.nasalToStopGap && lang.stopClosureNasalToStopGapMs > 0.0) {
          baseDur = lang.stopClosureNasalToStopGapMs * closureScale;
          baseFade = lang.stopClosureNasalToStopFadeMs;
        } else if (t.wordStart && lang.stopClosureWordBoundaryClusterGapMs > 0.0) {
          baseDur = lang.stopClosureWordBoundaryClusterGapMs * closureScale;
        }
        if (!t.nasalToStopGap && t.wordStart && lang.stopClosureWordBoundaryClusterFadeMs > 0.0) {
          baseFade = lang.stopClosureWordBoundaryClusterFadeMs;
        }

        dur = baseDur / curSpeed;
        fade = baseFade / curSpeed;
      } else {
        dur = lang.stopClosureVowelGapMs * closureScale / curSpeed;
        fade = lang.stopClosureVowelFadeMs / curSpeed;
      }
    } else if (t.postStopAspiration) {
      dur = lang.postStopAspirationDurationMs / curSpeed;
    } else if (tokenIsTap(t) || tokenIsTrill(t)) {
      if (tokenIsTrill(t)) {
        // Use trillModulationMs as the base duration for trills (at speed=1.0).
        // If unset/invalid, fall back to configurable default.
        double baseDur = (lang.trillModulationMs > 0.0) ? lang.trillModulationMs : lang.trillFallbackDurationMs;
        dur = baseDur / curSpeed;
      } else {
        dur = std::min(lang.tapDurationMs / curSpeed, lang.tapDurationMs);
      }
      fade = 0.001;
    } else if (tokenIsStop(t)) {
      double scale = (t.def && t.def->hasDurationScale) ? t.def->durationScale : 1.0;
      dur = std::min(lang.stopDurationMs * scale / curSpeed,
                     lang.stopDurationMs * scale);
      fade = 0.001;
    } else if (tokenIsAfricate(t)) {
      double scale = (t.def && t.def->hasDurationScale) ? t.def->durationScale : 1.0;
      dur = lang.affricateDurationMs * scale / curSpeed;
      fade = 0.001;
    } else if (!tokenIsVoiced(t)) {
      dur = lang.voicelessFricativeDurationMs / curSpeed;
    } else {
      if (tokenIsVowel(t)) {
        if (last && (tokenIsLiquid(*last) || tokenIsSemivowel(*last))) {
          fade = lang.fadeAfterLiquidMs / curSpeed;
        }

        if (t.tiedTo) {
          dur = lang.tiedVowelDurationMs / curSpeed;
        } else if (t.tiedFrom) {
          dur = lang.tiedFromVowelDurationMs / curSpeed;
          fade = lang.tiedFromVowelFadeMs / curSpeed;
        } else if (!syllableStress && !t.syllableStart && next && !next->wordStart && (tokenIsLiquid(*next) || tokenIsNasal(*next))) {
          if (tokenIsLiquid(*next)) {
            dur = lang.vowelBeforeLiquidDurationMs / curSpeed;
          } else {
            dur = lang.vowelBeforeNasalDurationMs / curSpeed;
          }
        }
      } else {
        dur = lang.voicedConsonantDurationMs / curSpeed;
        if (tokenIsLiquid(t) || tokenIsSemivowel(t)) {
          fade = lang.liquidFadeMs / curSpeed;
        }
        // Universal nasal floor: place perception depends on formant
        // transitions that need real time (/ɲ/ vs /n/ vs /ŋ/).
        // Configurable per-pack; rate_compensation adds a second layer.
        if (tokenIsNasal(t) && dur < lang.nasalMinDurationMs) {
          dur = lang.nasalMinDurationMs;
        }
      }
    }

    // Optional: semivowel offglide shortening.
    //
    // Some packs render diphthongs as vowel+semivowel sequences (e.g. eɪ -> ej).
    // When that semivowel is followed by another segment within the same word,
    // giving it a full consonant duration can sound like an unintended micro-break
    // or over-emphasized glide (e.g. "player", "later", "application", "vacation").
    if (lang.semivowelOffglideScale != 1.0 && tokenIsSemivowel(t)) {
      double s = lang.semivowelOffglideScale;
      if (s <= 0.0) s = 1.0;
      // Keep this bounded to avoid pathological configs.
      if (s < 0.05) s = 0.05;
      if (s > 3.0) s = 3.0;

      const bool prevIsVowel = (last && !last->silence && tokenIsVowel(*last));
      // Shorten semivowel offglides before any non-silence segment within the word.
      // Previously this only applied before vowels/liquids, which left -ation words
      // with over-long glides before fricatives like ʃ.
      const bool nextInWord = (next && !next->silence && !next->wordStart);

      if (prevIsVowel && nextInWord) {
        dur *= s;
        fade *= s;
        // Avoid zero/negative durations.
        dur = std::max(dur, 1.0 / curSpeed);
        fade = std::max(fade, 0.001);
        if (fade > dur) fade = dur;
      }
    }

    // Hungarian short vowel tweak (defaults to enabled, safe to disable).
    if (lang.huShortAVowelEnabled && tokenIsVowel(t) && t.lengthened == 0 && t.baseChar != 0) {
      if (t.baseChar == (lang.huShortAVowelKey.empty() ? U'\0' : lang.huShortAVowelKey[0])) {
        dur *= lang.huShortAVowelScale;
      }
    }

    // English word-final long /uː/ shortening.
    if (lang.englishLongUShortenEnabled && tokenIsVowel(t) && t.lengthened > 0 && t.baseChar != 0) {
      if (t.baseChar == (lang.englishLongUKey.empty() ? U'\0' : lang.englishLongUKey[0])) {
        if (!next || next->wordStart) {
          dur *= lang.englishLongUWordFinalScale;
          fade = std::min(fade, 14.0 / curSpeed);
        }
      }
    }

    // Lengthened scaling.
    // Multiple length marks (ːː) now stack multiplicatively.
    if (t.lengthened > 0) {
      if (!lang.applyLengthenedScaleToVowelsOnly || tokenIsVowel(t)) {
        const bool isHu = (lang.langTag.rfind("hu", 0) == 0);
        const double scale = isHu ? lang.lengthenedScaleHu : lang.lengthenedScale;
        dur *= std::pow(scale, t.lengthened);
      }
    }

    // Optional: additional shortening for lengthened vowels (ː) in a final
    // closed syllable (vowel + word-final consonant(s)).
    //
    // This is intentionally conservative: we only apply it when there are no
    // later vowels before the next word boundary, which avoids false positives
    // in words where a consonant cluster is actually the onset of the next
    // syllable (e.g. "apricot" /ˈeɪprɪ.../).
    if (lang.lengthenedVowelFinalCodaScale != 1.0 && t.lengthened > 0 && tokenIsVowel(t)) {
      // Find the next non-silence token.
      size_t j = i + 1;
      while (j < tokens.size() && tokens[j].silence) ++j;

      if (j < tokens.size() && !tokens[j].wordStart) {
        const Token& after = tokens[j];
        const bool afterVowelLike = tokenIsVowel(after) || tokenIsSemivowel(after);

        // Only consider cases where the vowel is followed by a consonant.
        if (!afterVowelLike) {
          // If there are any later vowels in this word, avoid shortening.
          bool laterVowel = false;
          for (size_t k = j; k < tokens.size(); ++k) {
            const Token& t2 = tokens[k];
            if (t2.wordStart) break;
            if (t2.silence) continue;
            if (tokenIsVowel(t2)) {
              laterVowel = true;
              break;
            }
          }

          if (!laterVowel) {
            dur *= lang.lengthenedVowelFinalCodaScale;
            // Keep fades from dominating very short vowels.
            fade = std::min(fade, 14.0 / curSpeed);
          }
        }
      }
    }

    t.durationMs = dur;

    // NOTE: Boundary smoothing is now handled by the boundary_smoothing pass
    // which runs after timing calculation. This allows for more sophisticated
    // transition handling without cluttering this function.

    t.fadeMs = fade;
    last = &t;
  }
}


static void calculatePitches(std::vector<Token>& tokens, const PackSet& pack,
                              double speed, double basePitch, double inflection, char clauseType) {
  const auto& mode = pack.lang.legacyPitchMode;
  if (mode == "legacy")
    applyPitchLegacy(tokens, pack, speed, basePitch, inflection, clauseType);
  else if (mode == "fujisaki_style")
    applyPitchFujisaki(tokens, pack, speed, basePitch, inflection, clauseType);
  else if (mode == "impulse_style")
    applyPitchImpulse(tokens, pack, speed, basePitch, inflection, clauseType);
  else if (mode == "klatt_style")
    applyPitchKlatt(tokens, pack, speed, basePitch, inflection, clauseType);
  else  // Default: espeak_style
    applyPitchEspeak(tokens, pack, speed, basePitch, inflection, clauseType);
}


static void applyToneContours(std::vector<Token>& tokens, const PackSet& pack, double basePitch, double inflection) {
  const LanguagePack& lang = pack.lang;
  if (!lang.tonal) return;
  if (lang.toneContours.empty()) return;

  // Build syllable start indices.
  std::vector<int> syllStarts;
  for (int i = 0; i < static_cast<int>(tokens.size()); ++i) {
    if (tokens[i].syllableStart) syllStarts.push_back(i);
  }
  if (syllStarts.empty()) return;

  auto clampPct = [](double p) {
    if (p < 0.0) return 0.0;
    if (p > 100.0) return 100.0;
    return p;
  };

  for (size_t si = 0; si < syllStarts.size(); ++si) {
    int start = syllStarts[si];
    int end = (si + 1 < syllStarts.size()) ? syllStarts[si + 1] : static_cast<int>(tokens.size());

    const std::u32string& toneKey = tokens[start].tone;
    if (toneKey.empty()) continue;

    auto it = lang.toneContours.find(toneKey);
    if (it == lang.toneContours.end()) continue;
    const std::vector<int>& contour = it->second;
    if (contour.size() < 2) continue;

    // Establish baseline percent from the existing phrase-level pitch at syllable start.
    double baselinePitch = getFieldOrZero(tokens[start], FieldId::voicePitch);
    if (baselinePitch <= 0.0) baselinePitch = basePitch;
    const double baselinePct = percentFromPitch(basePitch, inflection, baselinePitch);

    // Convert contour points to target percents.
    std::vector<double> targetPct;
    targetPct.reserve(contour.size());

    const bool absolute = lang.toneContoursAbsolute;
    for (int p : contour) {
      double v = static_cast<double>(p);
      if (!absolute) {
        v = baselinePct + v; // relative offset
      }
      targetPct.push_back(clampPct(v));
    }

    // Piecewise-linear over voiced duration.
    double voicedDuration = 0.0;
    for (int i = start; i < end; ++i) {
      if (tokenIsVoiced(tokens[i])) voicedDuration += tokens[i].durationMs;
    }
    if (voicedDuration <= 0.0) continue;

    // Precompute segment boundaries.
    const int segCount = static_cast<int>(targetPct.size()) - 1;
    double curVoiced = 0.0;

    for (int i = start; i < end; ++i) {
      Token& t = tokens[i];
      double startPitch = getFieldOrZero(t, FieldId::voicePitch);
      double endPitch = getFieldOrZero(t, FieldId::endVoicePitch);

      if (tokenIsVoiced(t)) {
        double tStart = curVoiced / voicedDuration; // 0..1
        curVoiced += t.durationMs;
        double tEnd = curVoiced / voicedDuration;

        auto pctAt = [&](double u) {
          // u 0..1 -> segment
          double pos = u * segCount;
          int seg = static_cast<int>(std::floor(pos));
          if (seg < 0) seg = 0;
          if (seg >= segCount) seg = segCount - 1;
          double local = pos - seg;
          double a = targetPct[seg];
          double b = targetPct[seg + 1];
          return a + ((b - a) * local);
        };

        double p0 = pctAt(tStart);
        double p1 = pctAt(tEnd);
        startPitch = pitchFromPercent(basePitch, inflection, p0);
        endPitch = pitchFromPercent(basePitch, inflection, p1);
      }

      setPitchFields(t, startPitch, endPitch);
    }
  }
}

static void setDefaultVoiceFields(const LanguagePack& lang, Token& t) {
  auto setIfUnset = [&](FieldId id, double v) {
    int idx = static_cast<int>(id);
    std::uint64_t bit = (1ull << idx);
    if ((t.setMask & bit) == 0) {
      t.field[idx] = v;
      t.setMask |= bit;
    }
  };

  setIfUnset(FieldId::vibratoPitchOffset, lang.defaultVibratoPitchOffset);
  setIfUnset(FieldId::vibratoSpeed, lang.defaultVibratoSpeed);
  setIfUnset(FieldId::voiceTurbulenceAmplitude, lang.defaultVoiceTurbulenceAmplitude);
  setIfUnset(FieldId::glottalOpenQuotient, lang.defaultGlottalOpenQuotient);
  setIfUnset(FieldId::preFormantGain, lang.defaultPreFormantGain);
  setIfUnset(FieldId::outputGain, lang.defaultOutputGain);
}

bool convertIpaToTokens(
  const PackSet& pack,
  const std::string& ipaUtf8,
  double speed,
  double basePitch,
  double inflection,
  char clauseType,
  std::vector<Token>& outTokens,
  std::string& outError,
  std::vector<tgsb_data::PassSnapshot>* passTraceSink
) {
  outTokens.clear();

  if (speed <= 0.0) speed = 1.0;

  // The legacy pitch math was historically paired with a lower default inflection
  // setting (e.g. 35) than many modern configs (often 60).
  // To keep legacyPitchMode usable without forcing users to retune sliders,
  // we apply an optional scale here.
  double inflScale = pack.lang.legacyPitchInflectionScale;
  if (inflScale <= 0.0) inflScale = 1.0;
  // Keep this bounded to avoid pathological values from bad configs.
  if (inflScale > 2.0) inflScale = 2.0;
  const double infl = inflection * inflScale;
  if (clauseType == 0) clauseType = '.';

  const std::u32string normalized = internal::normalizeIpaText(pack, ipaUtf8);
  if (normalized.empty()) {
    return true;
  }

  if (!internal::parseToTokens(pack, normalized, outTokens, outError)) {
    return false;
  }

  if (outTokens.empty()) {
    return true;
  }

  // Word count for optional single-word tuning.
  int wordCount = 0;
  for (const Token& t : outTokens) {
    if (!t.def || t.silence) continue;
    if (t.wordStart) ++wordCount;
  }
  const bool isSingleWordUtterance = (wordCount == 1);

  // Optional: override intonation clause type for single-word utterances.
  // This is useful when callers pass clauseType=',' for "continuation"; for
  // isolated words/letters this often sounds like an odd rising comma.
  char effectiveClauseType = clauseType;
  if (isSingleWordUtterance && pack.lang.singleWordClauseTypeOverride != 0) {
    if (!pack.lang.singleWordClauseTypeOverrideCommaOnly || clauseType == ',') {
      effectiveClauseType = pack.lang.singleWordClauseTypeOverride;
    }
  }

  // Optional: auto-tie diphthongs when IPA does not include an explicit tie-bar.
  internal::autoTieDiphthongs(pack, outTokens);

  // Optional: spelling diphthong handling (e.g. acronym letter names).
  internal::applySpellingDiphthongMode(pack, outTokens);

  // Copy-adjacent correction (h, inserted aspirations, etc.).
  correctCopyAdjacent(outTokens);

  // Transforms (language-specific tuning for aspiration, fricatives, etc.).
  applyTransforms(pack.lang, outTokens);

  // Ensure voice defaults (vibrato, GOQ, gains) exist.
  for (Token& t : outTokens) {
    if (!t.def || t.silence) continue;
    setDefaultVoiceFields(pack.lang, t);
  }

  // Frontend passes: modular token-level rules (coarticulation, prosody, etc.).
  PassContext passCtx(pack, speed, basePitch, inflection, effectiveClauseType);
  passCtx.passTraceSink = passTraceSink;
  if (!runPasses(passCtx, PassStage::PreTiming, outTokens, outError)) {
    return false;
  }

  // Timing.
  calculateTimes(outTokens, pack, speed);

  if (!runPasses(passCtx, PassStage::PostTiming, outTokens, outError)) {
    return false;
  }

  // Pitch.
  calculatePitches(outTokens, pack, speed, basePitch, inflection, effectiveClauseType);

  // Tone overlay (optional).
  applyToneContours(outTokens, pack, basePitch, inflection);

  // Voice profile application (optional).
  // Apply formant scaling, pitch scaling, and per-phoneme overrides based on the active voice profile.
  // This runs AFTER calculatePitches so pitch multipliers (voicePitch_mul, endVoicePitch_mul) work.
  if (pack.voiceProfiles && !pack.lang.voiceProfileName.empty()) {
    for (Token& t : outTokens) {
      if (!t.def || t.silence) continue;
      applyVoiceProfileToFields(t.field, t.setMask, t.def, pack.voiceProfiles.get(), pack.lang.voiceProfileName);
    }
  }

  if (!runPasses(passCtx, PassStage::PostPitch, outTokens, outError)) {
    return false;
  }

  // Inflection hard gate: at 0% inflection, force true monotone by flattening
  // all pitch to basePitch and zeroing vibrato.  Without this gate, microprosody
  // (voiceless F0 raise, voiced F0 lower, intrinsic F0) and vibrato still
  // modulate pitch even though the pitch modes themselves scale to zero.
  if (inflection <= 0.0) {
    const int vp  = static_cast<int>(FieldId::voicePitch);
    const int evp = static_cast<int>(FieldId::endVoicePitch);
    const int vib = static_cast<int>(FieldId::vibratoPitchOffset);
    for (Token& t : outTokens) {
      t.field[vp]  = basePitch;
      t.field[evp] = basePitch;
      t.field[vib] = 0.0;
      t.setMask |= (1ull << vp) | (1ull << evp) | (1ull << vib);
    }
  }

  // Optional single-word tail tuning.
  //
  // The frame engine cuts to silence abruptly when the queue becomes empty.
  // For isolated words/letters this can make the final voiced phoneme sound
  // clipped. We fix this by adding a tiny extra hold to the final voiced
  // vowel/liquid/nasal and (optionally) appending a NULL frame to fade out.
  if (isSingleWordUtterance && pack.lang.singleWordTuningEnabled) {
    int lastReal = -1;
    for (int i = static_cast<int>(outTokens.size()) - 1; i >= 0; --i) {
      if (outTokens[i].def && !outTokens[i].silence) { lastReal = i; break; }
    }

    if (lastReal >= 0) {
      const Token& lt = outTokens[static_cast<size_t>(lastReal)];
      const bool voiced = tokenIsVoiced(lt);
      const bool isLiquid = tokenIsLiquid(lt);
      const bool tailSensitive = tokenIsVowel(lt) || tokenIsSemivowel(lt) || isLiquid || tokenIsTap(lt) || tokenIsTrill(lt) || tokenIsNasal(lt);

      const double sp = (speed > 0.0) ? speed : 1.0;

      // Hold: voiced tail-sensitive only (vowels, liquids, nasals, etc.)
      if (voiced && tailSensitive) {
        if (pack.lang.singleWordFinalHoldMs > 0.0) {
          double holdMs = pack.lang.singleWordFinalHoldMs;
          // Reduce hold for liquids (like R, L) which can sound unnatural when held.
          if (isLiquid && pack.lang.singleWordFinalLiquidHoldScale != 1.0) {
            holdMs *= pack.lang.singleWordFinalLiquidHoldScale;
          }
          outTokens[static_cast<size_t>(lastReal)].durationMs += (holdMs / sp);
        }
      }

      // Fade: ANY final phoneme — stops need the crossfade so the DSP
      // runs through voiceGenerator during the fade (aspiration release).
      if (pack.lang.singleWordFinalFadeMs > 0.0) {
        Token s;
        s.def = nullptr;
        s.silence = true;
        s.durationMs = 0.0;
        s.fadeMs = pack.lang.singleWordFinalFadeMs / sp;
        // Avoid a zero-sample fade at extreme speeds.
        if (s.fadeMs < 0.1) s.fadeMs = 0.1;
        outTokens.push_back(s);
      }
    }
  }

  // Clause-final hold: extend the last voiced sonorant in ALL utterances
  // (multi-word included) so it doesn't sound clipped/swallowed.
  // For single-word utterances, singleWordFinalHoldMs already handled this.
  if (!isSingleWordUtterance && pack.lang.clauseFinalHoldMs > 0.0) {
    int lastReal = -1;
    for (int i = static_cast<int>(outTokens.size()) - 1; i >= 0; --i) {
      if (outTokens[i].def && !outTokens[i].silence) { lastReal = i; break; }
    }
    if (lastReal >= 0) {
      const Token& lt = outTokens[static_cast<size_t>(lastReal)];
      const bool voiced = tokenIsVoiced(lt);
      const bool tailSensitive = tokenIsVowel(lt) || tokenIsSemivowel(lt) ||
                                 tokenIsLiquid(lt) || tokenIsTap(lt) ||
                                 tokenIsTrill(lt) || tokenIsNasal(lt);
      if (voiced && tailSensitive) {
        const double sp = (speed > 0.0) ? speed : 1.0;
        outTokens[static_cast<size_t>(lastReal)].durationMs +=
            (pack.lang.clauseFinalHoldMs / sp);
      }
    }
  }

  // Clause-final fade: append a silence fade token at the end of ALL
  // utterances (multi-word included) so clause-final stops get a proper
  // crossfade through voiceGenerator instead of the crude stopFade path.
  if (pack.lang.clauseFinalFadeMs > 0.0) {
    bool alreadyHasFade = false;
    if (!outTokens.empty()) {
      const Token& last = outTokens.back();
      if (last.silence && !last.def && last.durationMs == 0.0) {
        alreadyHasFade = true;  // singleWord block already appended one
      }
    }
    if (!alreadyHasFade) {
      const double sp = (speed > 0.0) ? speed : 1.0;
      Token s;
      s.def = nullptr;
      s.silence = true;
      s.durationMs = 0.0;
      s.fadeMs = pack.lang.clauseFinalFadeMs / sp;
      if (s.fadeMs < 0.1) s.fadeMs = 0.1;
      outTokens.push_back(s);
    }
  }

  return true;
}


} // namespace nvsp_frontend
