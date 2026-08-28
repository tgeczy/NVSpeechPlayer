/*
TGSpeechBox — Svarabhakti vowel-quality inheritance pass.
Copyright 2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

// =============================================================================
// Svarabhakti pass — the inserted vocoid echoes its neighboring vowel
// =============================================================================
//
// Language packs insert a svarabhakti vocoid (the ᵊ pseudo-phoneme) around
// taps and trills — Spanish: intervocalic ɾ → ᵊɾ, word-final ɾ → ɾ_wf ᵊ ɾ_wf.
// The insertion itself is phonologically right, but ᵊ carried one fixed
// neutral quality (≈500/1400 Hz) at near-full vowel length and amplitude, so
// every /Vɾ/ word ended in the same loud schwa: "far/fer/fir/for/fur" all
// measured as the SAME vowel (~410/1510) while reference formant synthesis
// keeps five distinct ones ("far" read as "farə").
//
// The phonetics literature is specific about what this element really is:
//   - Svarabhakti vowels "replicate the quality of the syllable's nuclear
//     vowel" — same articulatory gesture, not an independent one — with
//     duration ≈ 1/4 of the nuclear vowel (da Silva 2024, J. Speech
//     Sciences, DOI 10.20396/joss.v13i00.19957).
//   - Tap coarticulation is carryover-dominant: the PRECEDING vowel colors
//     through the tap (Recasens 1991, DOI 10.1016/s0095-4470(19)30344-4;
//     Recasens & Pallarès 1999, DOI 10.1006/jpho.1999.0092).
//
// So: each ᵊ inherits cf1-3/pf1-3 from the nearest vowel (left first —
// carryover), keeps a small share of its own neutral color, and scales its
// duration and amplitude down to svarabhakti proportions. Runs BEFORE the
// coarticulation pass so locus logic and steady-state emission see the
// inherited values. Opt-in per language; packs without the flag are
// bit-identical.

#include "svarabhakti.h"
#include "../pack.h"
#include "../ipa_engine.h"
#include <algorithm>

namespace nvsp_frontend::passes {

namespace {

const std::u32string kSchwaInsertKey = U"ᵊ";  // ᵊ (exact; ᵊᵉ untouched)

inline bool isVowelDef(const Token& t) {
  return t.def && ((t.def->flags & kIsVowel) != 0);
}

double getField(const Token& t, FieldId id) {
  const int idx = static_cast<int>(id);
  if ((t.setMask & (1ULL << idx)) != 0) return t.field[idx];
  if (t.def && (t.def->setMask & (1ULL << idx)) != 0) return t.def->field[idx];
  return 0.0;
}

void setField(Token& t, FieldId id, double val) {
  const int idx = static_cast<int>(id);
  t.field[idx] = val;
  t.setMask |= (1ULL << idx);
}

// Nearest vowel: scan left first (carryover direction), then right.
const Token* findDonor(const std::vector<Token>& tokens, std::size_t i) {
  const int kReach = 3;
  for (int d = 1; d <= kReach; ++d) {
    if (i >= static_cast<std::size_t>(d)) {
      const Token& c = tokens[i - d];
      if (isVowelDef(c)) return &c;
      if (c.silence) break;  // don't inherit across a boundary
    }
  }
  for (int d = 1; d <= kReach; ++d) {
    if (i + d < tokens.size()) {
      const Token& c = tokens[i + d];
      if (isVowelDef(c)) return &c;
      if (c.silence) break;
    }
  }
  return nullptr;
}

}  // namespace

bool runSvarabhakti(
    PassContext& ctx,
    std::vector<Token>& tokens,
    std::string& /*outError*/) {
  const LanguagePack& lang = ctx.pack.lang;
  if (!lang.svarabhaktiInheritEnabled) return true;

  const double w = std::clamp(lang.svarabhaktiVowelWeight, 0.0, 1.0);
  const double durScale = std::clamp(lang.svarabhaktiDurationScale, 0.1, 1.0);
  const double ampScale = std::clamp(lang.svarabhaktiAmpScale, 0.1, 1.0);

  static const FieldId kPairs[][2] = {
      {FieldId::cf1, FieldId::pf1},
      {FieldId::cf2, FieldId::pf2},
      {FieldId::cf3, FieldId::pf3},
  };

  for (std::size_t i = 0; i < tokens.size(); ++i) {
    Token& t = tokens[i];
    if (!t.def || t.def->key != kSchwaInsertKey) continue;

    const Token* donor = findDonor(tokens, i);
    if (donor) {
      for (const auto& pair : kPairs) {
        const double own = getField(t, pair[0]);
        double dv = getField(*donor, pair[0]);
        if (dv <= 0.0) dv = getField(*donor, pair[1]);
        if (dv <= 0.0 || own <= 0.0) continue;
        const double blended = dv * w + own * (1.0 - w);
        setField(t, pair[0], blended);
        setField(t, pair[1], blended);
      }
    }
    // Svarabhakti proportions even without a donor: the element is a brief
    // echo, not a syllable of its own.
    t.svarabhaktiShaped = true;
    t.durationMs *= durScale;
    const double va = getField(t, FieldId::voiceAmplitude);
    if (va > 0.0) setField(t, FieldId::voiceAmplitude, va * ampScale);
  }

  // Vowel-to-tap carryover (Recasens 1991): color the tap token itself
  // toward the same donor. The alveolar identity survives in the amplitude
  // dip, the release burst, and the remaining locus share.
  const double tapBlend = std::clamp(lang.svarabhaktiTapBlend, 0.0, 1.0);
  if (tapBlend > 0.0) {
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      Token& t = tokens[i];
      if (!t.def || (t.def->flags & kIsTap) == 0) continue;
      // The trill phoneme is dual-flagged tap+trill (since the original
      // pack import, so tap-matching allophone rules can reach it). Trill
      // wins here: its contact phases carry their own modulation, and
      // vowel-blending them dulled the closures into "a d in the middle
      // of rr" (#115, caught in the field within hours).
      if ((t.def->flags & kIsTrill) != 0) continue;
      const Token* donor = findDonor(tokens, i);
      if (!donor) continue;
      for (const auto& pair : kPairs) {
        const double own = getField(t, pair[0]);
        double dv = getField(*donor, pair[0]);
        if (dv <= 0.0) dv = getField(*donor, pair[1]);
        if (dv <= 0.0 || own <= 0.0) continue;
        const double blended = dv * tapBlend + own * (1.0 - tapBlend);
        setField(t, pair[0], blended);
        setField(t, pair[1], blended);
      }
      // NOTE: the companion amplitude dip (svarabhaktiTapDip) is applied
      // at emission time in frame_emit — the prominence pass assigns
      // voiceAmplitude after this pass and would overwrite a token-level
      // value here.
    }
  }
  return true;
}

}  // namespace nvsp_frontend::passes
