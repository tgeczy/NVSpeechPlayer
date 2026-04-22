/*
TGSpeechBox — IPA tokenizer / parser implementation.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include "ipa_parser.h"
#include "ipa_internal.h"

#include <algorithm>
#include <cstring>

// Debug logging for token-level stop-closure decisions.
// Set to 1 to enable, 0 to disable.
#define PARSER_DEBUG_LOG 0
#if PARSER_DEBUG_LOG
#include <cstdio>
#include <cstdlib>
#include <string>
#include "utf8.h"
static FILE* parserLogFile() {
  static FILE* f = nullptr;
  if (!f) {
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = std::getenv("TMP");
    if (!tmp) tmp = "/tmp";
    std::string path = std::string(tmp) + "/tgsb_parser.log";
    f = std::fopen(path.c_str(), "a");
  }
  return f;
}
#define PLOG(...) do { FILE* _f = parserLogFile(); if (_f) { std::fprintf(_f, __VA_ARGS__); std::fflush(_f); } } while(0)
#else
#define PLOG(...) ((void)0)
#endif

using nvsp_frontend::ipa_internal::isTieBar;
using nvsp_frontend::ipa_internal::findPhoneme;
using nvsp_frontend::ipa_internal::tokenIsVoiced;
using nvsp_frontend::ipa_internal::tokenIsStop;
using nvsp_frontend::ipa_internal::tokenIsAfricate;
using nvsp_frontend::ipa_internal::tokenIsTap;
using nvsp_frontend::ipa_internal::tokenIsNasal;
using nvsp_frontend::ipa_internal::tokenIsFricativeLike;

namespace nvsp_frontend {
namespace {

// ---------------------------------------------------------------------------
// Helpers (anonymous namespace — internal linkage)
// ---------------------------------------------------------------------------

// Greedy phoneme lookup: try to match the longest phoneme key starting at position `pos`.
// Returns the PhonemeDef* and sets `outConsumed` to the number of codepoints matched.
// Also sets `outBaseChar` to the first non-tie-bar character in the match.
// If no match is found, returns nullptr and outConsumed is 0.
static const PhonemeDef* greedyMatchPhoneme(
    const PackSet& pack,
    const std::u32string& text,
    size_t pos,
    size_t& outConsumed,
    char32_t& outBaseChar) {
  outConsumed = 0;
  outBaseChar = (pos < text.size()) ? text[pos] : 0;

  // Use the pre-sorted keys from PackSet (sorted by length descending).
  const auto& sortedKeys = pack.sortedPhonemeKeys;

  for (const auto& key : sortedKeys) {
    if (key.empty()) continue;
    if (pos + key.size() > text.size()) continue;

    // Direct match (exact)
    if (text.compare(pos, key.size(), key) == 0) {
      const PhonemeDef* def = findPhoneme(pack, key);
      if (def) {
        outConsumed = key.size();
        // Find the first non-tie-bar character for baseChar
        for (size_t j = pos; j < pos + key.size(); ++j) {
          if (!isTieBar(text[j])) {
            outBaseChar = text[j];
            break;
          }
        }
        return def;
      }
    }

    // Loose tie-bar match: skip tie bars in both text and key.
    // This allows "nʲ" in the key to match "n͡ʲ" in text.
    //
    // IMPORTANT: We only allow this matching when the TEXT contains tie bars
    // that the KEY lacks. We do NOT allow matching when the KEY contains tie bars
    // that the TEXT lacks. This prevents "əl" in text from matching "ə͡l" in the key,
    // which would cause syllabic L to lose its lateral quality.
    //
    // Rationale: The normalization pass intentionally removes tie bars from
    // sequences like "ə͡l" -> "əl" so they tokenize as separate phonemes.
    // The greedy tokenizer should not undo this normalization.

    // First, check if the key contains tie bars. If so, and exact match failed,
    // then we should NOT try loose matching (the text doesn't have the tie bar).
    bool keyHasTieBar = false;
    for (char32_t kc : key) {
      if (isTieBar(kc)) {
        keyHasTieBar = true;
        break;
      }
    }

    // Only do loose tie-bar matching if the key does NOT have tie bars.
    // This allows text "n͡ʲ" to match key "nʲ", but NOT text "əl" to match key "ə͡l".
    if (keyHasTieBar) {
      continue;
    }

    size_t t = pos;
    size_t k = 0;
    size_t consumed = 0;
    bool matched = true;
    char32_t firstNonTie = 0;

    while (k < key.size() && matched) {
      // Skip tie bars in key (though there shouldn't be any if keyHasTieBar is false)
      while (k < key.size() && isTieBar(key[k])) ++k;
      if (k >= key.size()) break;

      // Skip tie bars in text
      while (t < text.size() && isTieBar(text[t])) {
        ++t;
        ++consumed;
      }
      if (t >= text.size()) {
        matched = false;
        break;
      }

      // Compare characters
      if (text[t] != key[k]) {
        matched = false;
        break;
      }

      // Track first non-tie-bar character
      if (firstNonTie == 0) {
        firstNonTie = text[t];
      }

      ++t;
      ++k;
      ++consumed;
    }

    // Make sure we consumed the entire key
    while (k < key.size() && isTieBar(key[k])) ++k;

    if (matched && k >= key.size() && consumed > 0) {
      const PhonemeDef* def = findPhoneme(pack, key);
      if (def) {
        outConsumed = consumed;
        if (firstNonTie != 0) {
          outBaseChar = firstNonTie;
        }
        return def;
      }
    }
  }

  return nullptr;
}

static inline bool isToneLetter(char32_t c) {
  // Chao tone letters: ˥ ˦ ˧ ˨ ˩ (U+02E5..U+02E9)
  return c >= 0x02E5 && c <= 0x02E9;
}

static inline bool isAutoDiphthongOffglideCandidate(char32_t c) {
  // Heuristic for "diphthong offglide" vowels.
  //
  // Many IPA sources (including eSpeak) represent diphthongs as two vowels.
  // Some languages (or some eSpeak outputs) omit an explicit tie-bar/non-syllabic
  // mark. When enabled via packs, we can treat certain vowel+vowel sequences as a
  // diphthong by marking them as tied (as if U+0361 were present).
  //
  // We keep this intentionally conservative: only "high" vowels which commonly
  // act as offglides are considered.
  switch (c) {
    case U'i':
    case U'\u026A': // ɪ
    case U'u':
    case U'\u028A': // ʊ
    case U'y':
    case U'\u028F': // ʏ
    case U'\u026F': // ɯ
    case U'\u0268': // ɨ
      return true;
    default:
      return false;
  }
}

static void setTokenFromDef(Token& t, const PhonemeDef* def) {
  if (!def) return;
  t.def = def;
  t.setMask = def->setMask;
  std::memcpy(t.field, def->field, sizeof(double) * kFrameFieldCount);
  if (!def->key.empty()) {
    t.baseChar = def->key[0];
  }
}

static const PhonemeDef* mapOffglideToSemivowel(const PackSet& pack, char32_t vowel) {
  // Conservative mapping used by autoDiphthongOffglideToSemivowel.
  // If your language needs rounded-front glides (ɥ, etc.), map those in packs
  // by introducing a dedicated phoneme key.
  char32_t target = 0;
  switch (vowel) {
    case U'i':
    case U'\u026A': // ɪ
    case U'\u0268': // ɨ
      target = U'j';
      break;
    case U'u':
    case U'\u028A': // ʊ
      target = U'w';
      break;
    default:
      return nullptr;
  }

  std::u32string k;
  k.push_back(target);
  return findPhoneme(pack, k);
}

static bool wordLooksLikeSpelling(const std::vector<Token>& tokens, size_t start, size_t end) {
  int syllables = 0;
  int stressed = 0;

  for (size_t i = start; i < end; ++i) {
    const Token& t = tokens[i];
    if (!t.def || t.silence) continue;
    if (t.syllableStart) {
      ++syllables;
      if (t.stress != 0) ++stressed;
    }
  }

  // Heuristic: spelled-out acronyms/initialisms tend to have stress on every
  // letter-name syllable, and they are almost always multi-syllable.
  if (syllables < 2) return false;
  if (stressed < syllables) return false;
  return true;
}

} // anonymous namespace

namespace internal {

bool parseToTokens(const PackSet& pack, const std::u32string& text, std::vector<Token>& outTokens, std::string& outError) {
  const LanguagePack& lang = pack.lang;

  bool newWord = true;
  int pendingStress = 0;
  bool pendingSyllableBoundary = false;
  bool wordHasExplicitBoundaries = false;

  // IMPORTANT:
  // We must NOT keep raw pointers into outTokens across push_back(), because
  // std::vector can reallocate and invalidate them. Doing so breaks stress /
  // syllable tracking and leads to "flat" or inconsistent intonation.
  //
  // Use indices instead.
  int lastIndex = -1;           // index of last (non-gap) token
  int syllableStartIndex = -1;  // index of current syllable start token

  auto attachToneToSyllable = [&](char32_t toneChar) {
    if (!lang.tonal) return;
    if (syllableStartIndex < 0) return;
    if (syllableStartIndex >= static_cast<int>(outTokens.size())) return;
    outTokens[syllableStartIndex].tone.push_back(toneChar);
  };

  auto attachToneStringToSyllable = [&](const std::u32string& toneStr) {
    if (!lang.tonal) return;
    if (syllableStartIndex < 0) return;
    if (syllableStartIndex >= static_cast<int>(outTokens.size())) return;
    outTokens[syllableStartIndex].tone.append(toneStr);
  };

  const size_t n = text.size();
  // Reserve a bit extra because we sometimes insert gaps/aspiration.
  outTokens.reserve(n * 2);

  for (size_t i = 0; i < n; ) {
    const char32_t c = text[i];

    if (c == U' ') {
      newWord = true;
      wordHasExplicitBoundaries = false;
      ++i;
      continue;
    }

    // Phoneme boundary marker (U+001F Unit Separator) — used in
    // space-delimited toIpa fields to separate phoneme keys within a
    // single word.  Extract the full token up to the next boundary and
    // try it as a complete phoneme key (e.g. "a_es", "ʊ_tr").  If it
    // matches a known phoneme, emit it directly.  If not, let the
    // characters fall through to normal greedy matching.
    if (c == U'\x1F') {
      ++i;
      // Extract token to next \x1F, space, or end.
      size_t tokStart = i;
      while (i < n && text[i] != U'\x1F' && text[i] != U' ') ++i;
      if (i > tokStart) {
        std::u32string tok(text, tokStart, i - tokStart);
        // Decode U+E001 (BMP PUA underscore escape) back to '_' for key lookup.
        for (auto& ch : tok) {
          if (ch == U'\xE001') ch = U'_';
        }
        const PhonemeDef* def = findPhoneme(pack, tok);
        if (def) {
          // Matched a phoneme key — emit it as a token.
          Token t;
          t.def = def;
          t.setMask = def->setMask;
          for (int f = 0; f < kFrameFieldCount; ++f)
            t.field[f] = def->field[f];
          t.baseChar = tok[0];
          t.stress = pendingStress;
          pendingStress = 0;
          t.wordStart = newWord;
          t.syllableStart = newWord || pendingSyllableBoundary;
          pendingSyllableBoundary = false;
          newWord = false;
          outTokens.push_back(t);
          lastIndex = static_cast<int>(outTokens.size()) - 1;
          syllableStartIndex = (outTokens.back().syllableStart)
              ? lastIndex : syllableStartIndex;
          continue;
        }
        // Not a phoneme key — rewind so characters get greedy-matched.
        i = tokStart;
      }
      continue;
    }

    // Primary/secondary stress.
    if (c == U'\u02C8') { // ˈ
      pendingStress = 1;
      ++i;
      continue;
    }
    if (c == U'\u02CC') { // ˌ
      pendingStress = 2;
      ++i;
      continue;
    }

    // Explicit syllable boundary marker (inserted by onset maximization).
    if (c == U'.') {
      pendingSyllableBoundary = true;
      wordHasExplicitBoundaries = true;
      ++i;
      continue;
    }

    // Tone markers (only when tonal is enabled).
    if (lang.tonal) {
      if (isToneLetter(c)) {
        // Collect run of tone letters.
        std::u32string run;
        run.push_back(c);
        ++i;
        while (i < n && isToneLetter(text[i])) {
          run.push_back(text[i++]);
        }
        attachToneStringToSyllable(run);
        continue;
      }
      if (lang.toneDigitsEnabled && (c >= U'1' && c <= U'5')) {
        attachToneToSyllable(c);
        ++i;
        continue;
      }
    }

    // Skip standalone tie bars (they should have been consumed by a phoneme match).
    if (isTieBar(c)) {
      ++i;
      continue;
    }

    // === GREEDY MULTI-CHARACTER PHONEME MATCHING ===
    // Try to match the longest phoneme key starting at position i.
    // This handles multi-character phonemes like "ɲʲ", "t͡ʃ", "aː", etc.

    const PhonemeDef* def = nullptr;
    size_t consumed = 0;
    bool tiedTo = false;
    int lengthened = 0;  // count of length marks
    char32_t baseChar = c;

    // Use greedy longest-match tokenization.
    def = greedyMatchPhoneme(pack, text, i, consumed, baseChar);

    if (def && consumed > 0) {
      // Check if this match includes or is followed by a tie bar (for tiedTo flag).
      // Also count length marks within the match.
      for (size_t j = i; j < i + consumed; ++j) {
        if (isTieBar(text[j])) tiedTo = true;
        if (text[j] == U'\u02D0') ++lengthened; // ː
      }
      // Check if next char after match is a tie bar.
      if (i + consumed < n && isTieBar(text[i + consumed])) {
        tiedTo = true;
      }
      // Consume ALL consecutive length marks after the match.
      while (i + consumed < n && text[i + consumed] == U'\u02D0') {
        // Try to find a lengthened version of this phoneme (only on first extra mark).
        if (lengthened == 0) {
          std::u32string lenKey = def->key;
          lenKey.push_back(U'\u02D0');
          const PhonemeDef* lenDef = findPhoneme(pack, lenKey);
          if (lenDef) {
            def = lenDef;
          }
        }
        ++consumed;
        ++lengthened;
      }
    } else {
      // No greedy match found - try single character as fallback.
      std::u32string k;
      k.push_back(c);
      def = findPhoneme(pack, k);
      if (def) {
        consumed = 1;
        // Consume ALL consecutive length marks after the character.
        while (i + consumed < n && text[i + consumed] == U'\u02D0') {
          // Try to find a lengthened version of this phoneme (only on first mark).
          if (lengthened == 0) {
            std::u32string lenKey = k;
            lenKey.push_back(U'\u02D0');
            const PhonemeDef* lenDef = findPhoneme(pack, lenKey);
            if (lenDef) {
              def = lenDef;
            }
          }
          ++consumed;
          ++lengthened;
        }
        // Check for tie bar.
        if (i + consumed < n && isTieBar(text[i + consumed])) {
          tiedTo = true;
        }
      } else {
        // Unknown char: drop it (safe default).
        ++i;
        continue;
      }
    }

    // Check if this token was preceded by a tie bar (for tiedFrom flag).
    const bool isTiedFrom = (i > 0 && isTieBar(text[i - 1]));

    // Advance the position by the number of codepoints consumed.
    i += consumed;

    Token t;
    t.def = def;
    t.setMask = def->setMask;
    for (int f = 0; f < kFrameFieldCount; ++f) {
      t.field[f] = def->field[f];
    }

    t.baseChar = baseChar;
    t.tiedFrom = isTiedFrom;
    t.tiedTo = tiedTo;
    t.lengthened = lengthened;

    const int stress = pendingStress;
    pendingStress = 0;

    // Helper to access last token safely by index.
    auto haveLast = [&]() -> bool {
      return lastIndex >= 0 && lastIndex < static_cast<int>(outTokens.size());
    };

    // Explicit syllable boundary from '.' marker.
    if (pendingSyllableBoundary) {
      t.syllableStart = true;
      pendingSyllableBoundary = false;
    }

    // Heuristic syllable start detection — only when the word has no
    // explicit '.' boundaries (onset maximization already placed them).
    if (!wordHasExplicitBoundaries && haveLast()) {
      Token& last = outTokens[lastIndex];
      if (!tokenIsVowel(last) && tokenIsVowel(t)) {
        last.syllableStart = true;
        syllableStartIndex = lastIndex;
      } else if (stress == 1 && tokenIsVowel(last)) {
        t.syllableStart = true;
        // syllableStartIndex will be set after we push t.
      }
    }

    // Post-stop aspiration insertion.
    // Avoid keeping references into outTokens across push_back() (brittle if this code grows).
    if (lang.postStopAspirationEnabled && haveLast()) {
      const bool lastIsStop = tokenIsStop(outTokens[lastIndex]);
      const bool lastIsVoiced = tokenIsVoiced(outTokens[lastIndex]);
      const bool curIsVoiced = tokenIsVoiced(t);
      const bool curIsStop = tokenIsStop(t);
      const bool curIsAfricate = tokenIsAfricate(t);

      if (lastIsStop && !lastIsVoiced && curIsVoiced && !curIsStop && !curIsAfricate) {
        const PhonemeDef* asp = findPhoneme(pack, lang.postStopAspirationPhoneme);
        if (asp) {
          Token a;
          a.def = asp;
          a.setMask = asp->setMask;
          for (int f = 0; f < kFrameFieldCount; ++f) a.field[f] = asp->field[f];
          a.postStopAspiration = true;
          a.baseChar = U'\0';
          outTokens.push_back(a);
          // Match the Python behavior: the inserted aspiration becomes "last".
          lastIndex = static_cast<int>(outTokens.size()) - 1;
        }
      }
    }

    if (newWord) {
      newWord = false;
      t.wordStart = true;
      t.syllableStart = true;
      // Syllable start will be the token we append for this word.
      syllableStartIndex = -1;
    }

    // Optional: intra-word hiatus break between adjacent vowels when the
    // second vowel is explicitly stressed (useful for spelled-out acronyms).
    if (lang.stressedVowelHiatusGapMs > 0.0 && stress != 0 && haveLast()) {
      const Token& prev = outTokens[lastIndex];
      if (!prev.silence && !t.wordStart && tokenIsVowel(prev) && tokenIsVowel(t)) {
        // Do not insert if IPA already tied these vowels.
        if (!prev.tiedTo && !prev.tiedFrom && !t.tiedTo && !t.tiedFrom) {
          Token gap;
          gap.silence = true;
          gap.vowelHiatusGap = true;
          outTokens.push_back(gap);
          // IMPORTANT: do NOT update lastIndex here. We want the previous real
          // phoneme to remain "last" for stress and other logic, matching the
          // stop-closure gap behavior.
        }
      }
    }

    // Stop closure insertion.
#if PARSER_DEBUG_LOG
    {
      std::string keyUtf = t.def ? nvsp_frontend::u32ToUtf8(t.def->key) : std::string("<null>");
      PLOG("TOKEN key='%s' stress=%d isStop=%d isAfricate=%d flags=0x%x wordStart=%d\n",
           keyUtf.c_str(), stress, tokenIsStop(t) ? 1 : 0,
           tokenIsAfricate(t) ? 1 : 0,
           (unsigned)(t.def ? t.def->flags : 0u),
           t.wordStart ? 1 : 0);
    }
#endif
    if (stress == 0 && (tokenIsStop(t) || tokenIsAfricate(t))) {
      bool needGap = false;
      bool clusterGap = false;
      bool nasalToStopGap = false;

      if (lang.stopClosureMode == "always") {
        needGap = true;
      } else if (lang.stopClosureMode == "after-vowel") {
        if (haveLast() && tokenIsVowel(outTokens[lastIndex])) needGap = true;
      } else if (lang.stopClosureMode == "vowel-and-cluster") {
        if (haveLast() && tokenIsVowel(outTokens[lastIndex])) {
          needGap = true;
        } else if (lang.stopClosureClusterGapsEnabled && haveLast() && !outTokens[lastIndex].silence) {
          const Token& prev = outTokens[lastIndex];
          const bool prevIsNasal = tokenIsNasal(prev);
          const bool prevIsStopLike = tokenIsStop(prev) || tokenIsAfricate(prev);
          const bool prevIsLiquidLike = tokenIsLiquid(prev) || tokenIsSemivowel(prev);
          const bool prevIsFric = tokenIsFricativeLike(prev);
          const bool allowAfterNasals = lang.stopClosureAfterNasalsEnabled;
          if ((!prevIsNasal || allowAfterNasals) &&
              (prevIsFric || prevIsStopLike || prevIsLiquidLike || (allowAfterNasals && prevIsNasal))) {
            needGap = true;
            clusterGap = true;
          }
          // Dedicated nasal→stop gap: when the general cluster gap skips nasals
          // but the language has stopClosureNasalToStopGapMs > 0, insert a
          // shorter dedicated gap to protect the stop burst.
          if (!needGap && prevIsNasal && lang.stopClosureNasalToStopGapMs > 0.0) {
            needGap = true;
            clusterGap = true;
            nasalToStopGap = true;
          }
        }
      } else {
        // none
      }

#if PARSER_DEBUG_LOG
      {
        std::string keyUtf = t.def ? nvsp_frontend::u32ToUtf8(t.def->key) : std::string("<null>");
        std::string prevKeyUtf = haveLast() && outTokens[lastIndex].def
            ? nvsp_frontend::u32ToUtf8(outTokens[lastIndex].def->key)
            : std::string("<none>");
        PLOG("  STOP-DECISION key='%s' prev='%s' mode='%s' needGap=%d clusterGap=%d nasal2stop=%d\n",
             keyUtf.c_str(), prevKeyUtf.c_str(), lang.stopClosureMode.c_str(),
             needGap ? 1 : 0, clusterGap ? 1 : 0, nasalToStopGap ? 1 : 0);
      }
#endif
      if (needGap) {
        Token gap;
        gap.silence = true;
        gap.preStopGap = true;
        gap.clusterGap = clusterGap;
        gap.nasalToStopGap = nasalToStopGap;
        // Preserve word boundary information for timing tweaks.
        // The gap is inserted *before* the stop/affricate token `t`.
        gap.wordStart = t.wordStart;

        // For voiced stops/affricates, mark as voiced closure so the frame emitter
        // can output a voice bar (low-frequency murmur) instead of true silence.
        // This maintains continuous voicing through the closure while letting
        // formants interpolate naturally (no abrupt formant discontinuity).
        // Thread parent stop's PhonemeDef so voice bar can read voiceBarAmplitude/F1.
        gap.def = t.def;  // Always thread parent def for place-aware closure timing
        if (tokenIsVoiced(t)) {
          gap.voicedClosure = true;
        }

        // Coda noise taper: when a fricative precedes a voiceless coda stop,
        // mark the gap so frame_emit can emit a taper frame instead of silence.
        // The !t.wordStart guard prevents onset clusters (/st/ in "style") from
        // getting the taper — only coda clusters (/st/ in "list") qualify.
        if (clusterGap && haveLast() && tokenIsFricativeLike(outTokens[lastIndex])
            && !tokenIsVoiced(t) && !t.wordStart
            && lang.codaNoiseTaperEnabled && !tokenIsAfricate(t)) {
          gap.codaFricStopBlend = true;
          t.codaFricStopBlend = true;
        }

        outTokens.push_back(gap);
        // IMPORTANT: do NOT update lastIndex here; Python keeps lastPhoneme as the
        // previous *real* phoneme, not the inserted gap.
      }
    }

    // Word-boundary gap after affricates: when a word-final affricate
    // is followed by a word-initial consonant (that isn't a stop/affricate
    // — those already get preStopGap), insert a micro-silence so the
    // affricate's frication doesn't collide with the next consonant's
    // noise onset (e.g. "image for": postalveolar /dʒ/ → labiodental /f/).
    if (t.wordStart && !tokenIsVowel(t) && !t.silence &&
        !tokenIsStop(t) && !tokenIsAfricate(t) &&
        haveLast() && tokenIsAfricate(outTokens[lastIndex])) {
      Token gap;
      gap.silence = true;
      gap.preStopGap = true;
      gap.clusterGap = true;
      gap.wordStart = true;
      outTokens.push_back(gap);
    }

    // Append the real phoneme.
    outTokens.push_back(t);
    const int curIndex = static_cast<int>(outTokens.size()) - 1;

    // Finish syllableStart handling after insertion.
    if (outTokens[curIndex].syllableStart) {
      syllableStartIndex = curIndex;
    } else if (outTokens[curIndex].wordStart) {
      syllableStartIndex = curIndex;
    }

    // Apply stress to syllable start.
    if (stress != 0 && syllableStartIndex >= 0 && syllableStartIndex < static_cast<int>(outTokens.size())) {
      outTokens[syllableStartIndex].stress = stress;
    }

    lastIndex = curIndex;
  }

  (void)outError;
  return true;
}

void autoTieDiphthongs(const PackSet& pack, std::vector<Token>& tokens) {
  if (!pack.lang.autoTieDiphthongs) return;

  int prevReal = -1;
  const int n = static_cast<int>(tokens.size());
  for (int i = 0; i < n; ++i) {
    Token& cur = tokens[i];
    if (!cur.def || cur.silence) continue;

    if (prevReal >= 0) {
      Token& prev = tokens[prevReal];

      const bool prevVowelLike = tokenIsVowel(prev);  // onset must be a vowel, not a semivowel
      const bool curVowelLike = tokenIsVowel(cur) || tokenIsSemivowel(cur);

      // R-colored vowels (ɚ, ɝ) should never be diphthong onset nuclei.
      // ɚ+i is not a real diphthong in any language — when the text parser
      // fails to insert syllable dots, ɚ can end up adjacent to i and get
      // falsely tied, causing diphthong_collapse to erase the second vowel.
      const bool prevIsRColored = (prev.baseChar == U'\u025A' ||  // ɚ
                                   prev.baseChar == U'\u025D');   // ɝ

      // Syllabic nasals (n̩, m̩) are flagged _isVowel for duration purposes,
      // but a nasal can never be a diphthong onset nucleus.  Without this
      // guard, "tightening" /tˈaɪʔn̩ɪŋ/ falsely ties n̩+ɪ and collapse
      // erases the /ɪ/, swallowing the entire "-ing".
      const bool prevIsNasal = tokenIsNasal(prev);

      // Only consider within-syllable vowel-like sequences.
      // If the current token starts a new syllable (explicit stress, word start,
      // etc.), treat it as hiatus instead.
      if (prevVowelLike && !prevIsRColored && !prevIsNasal && curVowelLike && !cur.wordStart && !cur.syllableStart) {
        // Skip if the IPA already encoded tying, or either vowel is explicitly long.
        // A lengthened onset (e.g. oː from GOAT monophthongization) is a monophthong,
        // not a diphthong candidate — tying it with the next vowel creates a false
        // glide (e.g. "going" /ɡoːɪŋ/ → /ɡo͡ɪŋ/ sounds like "boing").
        if (!prev.tiedTo && !prev.tiedFrom && !cur.tiedTo && !cur.tiedFrom &&
            prev.lengthened == 0 && cur.lengthened == 0) {
          // Only auto-tie when the second vowel is a common offglide candidate.
          if (isAutoDiphthongOffglideCandidate(cur.baseChar)) {
            prev.tiedTo = true;
            cur.tiedFrom = true;
            if (pack.lang.autoDiphthongOffglideToSemivowel) {
              if (const PhonemeDef* glide = mapOffglideToSemivowel(pack, cur.baseChar)) {
                setTokenFromDef(cur, glide);
              }
            }
          }
        }
      }
    }

    prevReal = i;
  }
}

void applySpellingDiphthongMode(const PackSet& pack, std::vector<Token>& tokens) {
  const LanguagePack& lang = pack.lang;
  if (lang.spellingDiphthongMode != "monophthong") return;

  // Walk words (real phoneme tokens only; ignore inserted silence tokens).
  size_t i = 0;
  while (i < tokens.size()) {
    // Find the next word start (non-silence token with wordStart).
    while (i < tokens.size() && (tokens[i].silence || !tokens[i].def || !tokens[i].wordStart)) {
      ++i;
    }
    if (i >= tokens.size()) break;

    const size_t wordStart = i;
    size_t wordEnd = wordStart + 1;
    while (wordEnd < tokens.size()) {
      if (!tokens[wordEnd].silence && tokens[wordEnd].def && tokens[wordEnd].wordStart) {
        break;
      }
      ++wordEnd;
    }

    if (wordLooksLikeSpelling(tokens, wordStart, wordEnd)) {
      // Convert letter-name diphthongs to monophthongs.
      // Currently this is intentionally narrow: only handle English letter 'A'
      // (/eɪ/ or pack-normalized /ej/) when it follows a vowel-like sound.
      int prevReal = -1;
      size_t pos = wordStart;
      while (pos < wordEnd) {
        Token& t = tokens[pos];
        if (!t.def || t.silence) {
          ++pos;
          continue;
        }

        // Candidate for "A": stressed syllable that starts on a vowel 'e'.
        const bool isStressedSyllableStart = t.syllableStart && (t.stress != 0);
        const bool isE = tokenIsVowel(t) && (t.baseChar == U'e');

        bool prevVowelLike = false;
        if (prevReal >= 0 && static_cast<size_t>(prevReal) >= wordStart && static_cast<size_t>(prevReal) < wordEnd) {
          const Token& prev = tokens[static_cast<size_t>(prevReal)];
          if (prev.def && !prev.silence) {
            prevVowelLike = tokenIsVowel(prev) || tokenIsSemivowel(prev);
          }
        }

        if (isStressedSyllableStart && isE && prevVowelLike) {
          // Find the next real token (skip silence).
          size_t j = pos + 1;
          while (j < wordEnd && (tokens[j].silence || !tokens[j].def)) ++j;

          if (j < wordEnd) {
            const Token& off = tokens[j];
            const bool isJ = tokenIsSemivowel(off) && (off.baseChar == U'j');
            const bool isIshVowel = tokenIsVowel(off) && (off.baseChar == U'\u026A' || off.baseChar == U'i');
            if (isJ || isIshVowel) {
              // Only treat this as standalone /eɪ/ if the offglide is followed
              // by the next syllable (next letter) or the end of the word.
              size_t k = j + 1;
              while (k < wordEnd && (tokens[k].silence || !tokens[k].def)) ++k;

              if (k >= wordEnd || tokens[k].syllableStart) {
                // Monophthongize: keep the /e/ nucleus, drop the offglide.
                // Mark the nucleus as lengthened to preserve a letter-name feel.
                t.lengthened = 1;
                t.tiedTo = false;
                t.tiedFrom = false;

                // Erase the offglide token.
                tokens.erase(tokens.begin() + static_cast<std::vector<Token>::difference_type>(j));
                --wordEnd;

                // Do not advance pos; re-evaluate with the new neighbor.
                continue;
              }
            }
          }
        }

        prevReal = static_cast<int>(pos);
        ++pos;
      }
    }

    i = wordEnd;
  }
}

} // namespace internal
} // namespace nvsp_frontend
