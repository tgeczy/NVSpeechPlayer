/*
TGSpeechBox — Text parser with CMU Dict stress correction.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

// =============================================================================
// Text Parser — pre-IPA-engine text-level corrections
// =============================================================================
//
// Sits between callers and convertIpaToTokens().  Receives both the original
// text and eSpeak's IPA output, applies word-level plugins, and returns
// corrected IPA.  The IPA engine never knows text was involved.
//
// Current plugin: stress lookup (CMU Dict → stress digit patterns).
// Future plugins (numbers, function-word reduction) slot in at the end
// of runTextParser().

#include "text_parser.h"
#include "text_prepare.h"
#include "dict_lookup.h"
#include "utf8.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

// Temporary debug logging for text parser investigation.
// Set to 1 to enable, 0 to disable.
#define TPARSER_DEBUG_LOG 0
#if TPARSER_DEBUG_LOG
#include <cstdio>
#include <cstdlib>
static FILE* tparserLogFile() {
  static FILE* f = nullptr;
  if (!f) {
#ifdef __ANDROID__
    // App-writable external dir (adb-pullable), same location the
    // instrumentation probes use.
    std::string path =
        "/sdcard/Android/data/com.tgspeechbox.tts/files/tgsb_textparser.log";
#else
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = std::getenv("TMP");
    if (!tmp) tmp = "/tmp";
    std::string path = std::string(tmp) + "/tgsb_textparser.log";
#endif
    f = std::fopen(path.c_str(), "a");
  }
  return f;
}
#define TPLOG(...) do { FILE* _f = tparserLogFile(); if (_f) { std::fprintf(_f, __VA_ARGS__); std::fflush(_f); } } while(0)
#else
#define TPLOG(...) ((void)0)
#endif

namespace nvsp_frontend {

namespace {

// ── IPA vowel codepoint set ────────────────────────────────────────────────
//
// Used for counting vowel nuclei in an IPA chunk.  Consecutive vowels
// (+ length mark ː) count as a single nucleus (handles diphthongs).

static bool isIpaVowel(char32_t c) {
  switch (c) {
    // Basic Latin vowels
    case U'a': case U'e': case U'i': case U'o': case U'u': case U'y':
    // IPA-specific vowels
    case U'\u0251':  // ɑ  open back unrounded
    case U'\u00E6':  // æ  near-open front unrounded
    case U'\u025B':  // ɛ  open-mid front unrounded
    case U'\u026A':  // ɪ  near-close front unrounded
    case U'\u0254':  // ɔ  open-mid back rounded
    case U'\u0259':  // ə  schwa
    case U'\u028A':  // ʊ  near-close back rounded
    case U'\u028C':  // ʌ  open-mid back unrounded
    case U'\u0252':  // ɒ  open back rounded
    case U'\u025C':  // ɜ  open-mid central unrounded
    case U'\u0250':  // ɐ  near-open central
    case U'\u0264':  // ɤ  close-mid back unrounded
    case U'\u0275':  // ɵ  close-mid central rounded
    case U'\u0258':  // ɘ  close-mid central unrounded
    case U'\u025E':  // ɞ  open-mid central rounded
    case U'\u0276':  // ɶ  open front rounded
    case U'\u0268':  // ɨ  close central unrounded
    case U'\u0289':  // ʉ  close central rounded
    case U'\u026F':  // ɯ  close back unrounded
    case U'\u025D':  // ɝ  r-colored schwa
    case U'\u025A':  // ɚ  r-colored schwa (mid central)
    case U'\u00F8':  // ø  close-mid front rounded
    case U'\u1D7B':  // ᵻ  near-close central unrounded (eSpeak reduced vowel)
    case U'\u1D7F':  // ᵿ  near-close central rounded (eSpeak reduced vowel)
      return true;
    default:
      return false;
  }
}

// Reduced vowels that cannot meaningfully carry primary stress.
// Putting ˈ on these is counterproductive — the vowel quality is already
// committed to "reduced," so stress won't sound stressed.
static bool isReducedVowel(char32_t c) {
  switch (c) {
    case U'\u0259':  // ə  schwa
    case U'\u0250':  // ɐ  near-open central
    case U'\u1D7B':  // ᵻ  near-close central unrounded (eSpeak)
    case U'\u1D7F':  // ᵿ  near-close central rounded (eSpeak)
    case U'\u025A':  // ɚ  r-colored schwa
      return true;
    default:
      return false;
  }
}

static bool isLengthMark(char32_t c) {
  return c == U'\u02D0';  // ː
}

static bool isTieBar(char32_t c) {
  return c == U'\u0361';  // ◌͡
}

static bool isSyllabicMark(char32_t c) {
  return c == U'\u0329';  // ◌̩  combining vertical line below
}

static bool isStressMark(char32_t c) {
  return c == U'\u02C8' || c == U'\u02CC';  // ˈ or ˌ
}

// ── Word splitting ─────────────────────────────────────────────────────────

static std::vector<std::string> splitOnWhitespace(const std::string& s) {
  std::vector<std::string> words;
  std::istringstream ss(s);
  std::string w;
  while (ss >> w) {
    words.push_back(std::move(w));
  }
  return words;
}

// Split IPA on spaces.  eSpeak separates word-level IPA with spaces.
static std::vector<std::string> splitIpaWords(const std::string& ipa) {
  std::vector<std::string> chunks;
  size_t start = 0;
  while (start < ipa.size()) {
    size_t sp = ipa.find(' ', start);
    if (sp == std::string::npos) {
      chunks.push_back(ipa.substr(start));
      break;
    }
    if (sp > start) {
      chunks.push_back(ipa.substr(start, sp - start));
    }
    start = sp + 1;
  }
  return chunks;
}

// Split IPA chunks that contain multiple primary stress marks (ˈ).
// A genuine single word never carries two primary stresses; when eSpeak
// merges number sub-words like "fˈɔːɹhˈʌndɹɪd" (four+hundred) into one
// chunk, this splits them back into separate IPA words so the IPA engine
// sees proper word boundaries.
static void splitMultiStressChunks(std::vector<std::string>& chunks) {
  for (size_t i = 0; i < chunks.size(); ++i) {
    // Work in u32 so we can reason about codepoints, not UTF-8 bytes.
    std::u32string u = utf8ToU32(chunks[i]);

    // Find the first primary stress mark (ˈ U+02C8).
    size_t first = std::u32string::npos;
    for (size_t j = 0; j < u.size(); ++j) {
      if (u[j] == U'\u02C8') { first = j; break; }
    }
    if (first == std::u32string::npos) continue;

    // Find the second primary stress mark.
    size_t second = std::u32string::npos;
    for (size_t j = first + 1; j < u.size(); ++j) {
      if (u[j] == U'\u02C8') { second = j; break; }
    }
    if (second == std::u32string::npos) continue;

    // The onset consonant(s) of the stressed syllable sit between the
    // previous syllable's vowel material and the ˈ mark.  Scan backward
    // from the second ˈ past any consonants to find the true word boundary.
    size_t splitPos = second;
    while (splitPos > first + 1) {
      char32_t prev = u[splitPos - 1];
      // Stop if we hit a vowel or length mark (ː) —
      // those belong to the previous word's nucleus.
      if (isIpaVowel(prev) || isLengthMark(prev))
        break;
      // Tie bar (͡) connects the char before it to the char after it
      // (e.g. ɔː͡ɹ). The character at splitPos was already skipped as a
      // "consonant" but it's actually tied to the previous vowel.
      // Restore it to the left chunk.
      if (isTieBar(prev)) {
        ++splitPos;
        break;
      }
      --splitPos;
    }

    std::u32string leftU = u.substr(0, splitPos);
    std::u32string rightU = u.substr(splitPos);

    if (leftU.empty() || rightU.empty()) continue;

    std::string left = u32ToUtf8(leftU);
    std::string right = u32ToUtf8(rightU);

    chunks[i] = std::move(left);
    chunks.insert(chunks.begin() + static_cast<ptrdiff_t>(i) + 1, std::move(right));
    // Re-check the right half in case it has yet another primary stress
    // (e.g. eSpeak merged three words).  Don't advance i.
  }
}

// ── Lowercase (ASCII only — text words are English) ────────────────────────

static std::string asciiLower(const std::string& s) {
  std::string out = s;
  for (auto& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  }
  return out;
}

// Strip punctuation from the edges of a text word (e.g. "hello," → "hello").
static std::string stripPunct(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && !std::isalpha(static_cast<unsigned char>(s[start]))) ++start;
  size_t end = s.size();
  while (end > start && !std::isalpha(static_cast<unsigned char>(s[end - 1]))) --end;
  if (start >= end) return {};
  return s.substr(start, end - start);
}

// ── Vowel nucleus counting ─────────────────────────────────────────────────

struct NucleusInfo {
  size_t start;  // byte offset of the first vowel codepoint in the nucleus
};

// Find all vowel nuclei in a u32 IPA chunk.  Consecutive vowels + ː = 1.
// A tie bar (U+0361) after a vowel binds the next character into the same
// nucleus (e.g. e͡ɪ = one diphthong nucleus, not two).
// A syllabic mark (U+0329) after a consonant makes it a nucleus (n̩, l̩, m̩).
static std::vector<NucleusInfo> findNuclei(const std::u32string& u32) {
  std::vector<NucleusInfo> nuclei;
  bool inVowel = false;
  for (size_t i = 0; i < u32.size(); ++i) {
    if (isTieBar(u32[i]) && inVowel) {
      // Tie bar extends the nucleus — skip it and the next character.
      if (i + 1 < u32.size()) ++i;
      continue;
    }
    // Syllabic consonant: consonant + U+0329 = nucleus.
    // Check if the NEXT character is a syllabic mark.
    if (!isIpaVowel(u32[i]) && !inVowel &&
        i + 1 < u32.size() && isSyllabicMark(u32[i + 1])) {
      nuclei.push_back({i});
      ++i;  // skip the syllabic mark
      inVowel = false;
      continue;
    }
    if (isIpaVowel(u32[i])) {
      if (!inVowel) {
        nuclei.push_back({i});
        inVowel = true;
      }
    } else if (isLengthMark(u32[i]) && inVowel) {
      // Length mark extends the nucleus — stay in vowel state.
    } else {
      inVowel = false;
    }
  }
  return nuclei;
}

// ── Stress remapping ───────────────────────────────────────────────────────

// Remove all ˈ and ˌ from a u32 string.
static std::u32string stripStress(const std::u32string& s) {
  std::u32string out;
  out.reserve(s.size());
  for (char32_t c : s) {
    if (!isStressMark(c)) out.push_back(c);
  }
  return out;
}

// Insert stress marks into an IPA chunk according to a digit pattern.
// Places ˈ/ˌ immediately before each vowel nucleus — matches eSpeak's
// convention and avoids the onset-legality problem entirely.
static std::u32string applyStressPattern(
    const std::u32string& stripped,
    const std::vector<NucleusInfo>& nuclei,
    const std::vector<int>& pattern)
{
  struct Insertion {
    size_t pos;
    char32_t mark;
  };
  std::vector<Insertion> insertions;

  for (size_t n = 0; n < nuclei.size() && n < pattern.size(); ++n) {
    int digit = pattern[n];
    if (digit == 0) continue;  // unstressed — no mark

    char32_t mark = (digit == 1) ? U'\u02C8' : U'\u02CC';  // ˈ or ˌ

    // Insert directly before the vowel nucleus — no onset walk-back.
    // Guard: never insert after a tie bar (would split a ligature).
    size_t pos = nuclei[n].start;
    if (pos > 0 && isTieBar(stripped[pos - 1])) continue;
    insertions.push_back({pos, mark});
  }

  // Apply insertions from back to front to preserve positions.
  std::u32string result = stripped;
  std::sort(insertions.begin(), insertions.end(),
            [](const Insertion& a, const Insertion& b) { return a.pos > b.pos; });
  for (const auto& ins : insertions) {
    result.insert(result.begin() + static_cast<ptrdiff_t>(ins.pos), ins.mark);
  }
  return result;
}

// ── Onset maximization ───────────────────────────────────────────────────
//
// Insert IPA '.' syllable boundaries at linguistically correct positions
// using the Maximal Onset Principle: for a consonant cluster between two
// vowel nuclei, assign the longest suffix that is a legal onset to the
// following syllable.

static std::u32string applySyllableBoundaries(
    const std::u32string& stripped,
    const std::vector<NucleusInfo>& nuclei,
    const std::vector<std::u32string>& legalOnsets)
{
  // Build insertion list (positions where '.' goes).
  std::vector<size_t> dots;

  for (size_t n = 0; n + 1 < nuclei.size(); ++n) {
    // Find end of current nucleus (first non-vowel, non-length-mark after
    // nucleus start).
    size_t codaStart = nuclei[n].start;
    {
      bool inV = false;
      for (size_t j = nuclei[n].start; j < stripped.size(); ++j) {
        if (isIpaVowel(stripped[j])) {
          inV = true;
        } else if (isLengthMark(stripped[j]) && inV) {
          // length mark extends vowel
        } else if (isTieBar(stripped[j]) && inV) {
          if (j + 1 < stripped.size()) ++j;  // skip tied char
        } else if (isSyllabicMark(stripped[j])) {
          // skip
        } else {
          codaStart = j;
          break;
        }
      }
      if (codaStart == nuclei[n].start) continue;  // no consonants between
    }

    size_t onsetEnd = nuclei[n + 1].start;  // just before next nucleus

    if (codaStart >= onsetEnd) continue;  // adjacent nuclei (diphthong)

    // Extract the consonant cluster.
    std::u32string cluster(stripped.begin() + codaStart,
                           stripped.begin() + onsetEnd);

    // Try suffix lengths from longest to 2 (single consonant onset is
    // always legal — that's the default fallback).
    size_t onsetLen = 1;  // default: one consonant goes to next syllable
    for (size_t tryLen = cluster.size(); tryLen >= 2; --tryLen) {
      std::u32string suffix(cluster.end() - tryLen, cluster.end());
      for (const auto& legal : legalOnsets) {
        if (suffix == legal) {
          onsetLen = tryLen;
          goto found;
        }
      }
    }
    found:

    // Insert '.' before the onset.
    size_t dotPos = onsetEnd - onsetLen;
    if (dotPos > codaStart || onsetLen == cluster.size()) {
      // Only insert if there's at least one coda consonant, OR the whole
      // cluster is a legal onset (all consonants go to next syllable).
      dots.push_back(dotPos);
    } else {
      // Fallback: put dot after first consonant.
      dots.push_back(codaStart + 1);
    }
  }

  if (dots.empty()) return stripped;

  // Apply dots from back to front to preserve positions.
  std::u32string result = stripped;
  std::sort(dots.begin(), dots.end(), std::greater<size_t>());
  for (size_t pos : dots) {
    result.insert(result.begin() + static_cast<ptrdiff_t>(pos), U'.');
  }
  return result;
}

// Convert a u32 string back to UTF-8.
static std::string u32ToUtf8(const std::u32string& s) {
  std::string result;
  result.reserve(s.size() * 3);
  for (char32_t c : s) {
    if (c < 0x80) {
      result.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
      result.push_back(static_cast<char>(0xC0 | (c >> 6)));
      result.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
      result.push_back(static_cast<char>(0xE0 | (c >> 12)));
      result.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      result.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
      result.push_back(static_cast<char>(0xF0 | (c >> 18)));
      result.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
      result.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      result.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
  }
  return result;
}

// Apply stress correction to a single IPA word chunk.
// Returns the original chunk unchanged if no correction applies.
static std::string correctStress(
    const std::string& textWord,
    const std::string& ipaChunk,
    const std::unordered_map<std::string, std::vector<int>>& dict,
    const std::unordered_map<std::string, std::vector<std::string>>& compoundMap,
    const std::vector<std::u32string>& legalOnsets)
{
  // Lowercase and strip punctuation from text word.
  const std::string key = asciiLower(stripPunct(textWord));
  if (key.empty()) return ipaChunk;

  // Lookup.
  auto it = dict.find(key);
  if (it == dict.end()) {
    // Compound fallback: default stress for words in compound map.
    // Primary on first nucleus, secondary on last, rest unstressed.
    TPLOG("  correctStress: \"%s\" not in stressDict, checking compoundMap (size=%zu)\n",
          key.c_str(), compoundMap.size());
    auto compIt = compoundMap.find(key);
    if (compIt == compoundMap.end()) { TPLOG("    not in compoundMap\n"); return ipaChunk; }
    TPLOG("    compound hit!\n");

    std::u32string u32 = utf8ToU32(ipaChunk);
    std::u32string stripped = stripStress(u32);
    auto nuclei = findNuclei(stripped);
    TPLOG("    nuclei=%zu\n", nuclei.size());
    if (nuclei.size() < 2) return ipaChunk;

    // Default compound stress: primary on first nucleus, secondary on last.
    // This is a safety net — Phase 2 pre-eSpeak splitting handles the common
    // case with correct vowel quality.  This fallback only fires if a compound
    // word somehow wasn't split before eSpeak.
    std::vector<int> pattern(nuclei.size(), 0);
    pattern[0] = 1;
    pattern[nuclei.size() - 1] = 2;

    // Same safety checks as normal path.
    for (size_t n = 0; n < nuclei.size() && n < pattern.size(); ++n) {
      if (pattern[n] == 1 && isReducedVowel(stripped[nuclei[n].start]))
        return ipaChunk;
    }

    if (!legalOnsets.empty() && nuclei.size() >= 2) {
      std::u32string dotted = applySyllableBoundaries(stripped, nuclei, legalOnsets);
      auto dottedNuclei = findNuclei(dotted);
      std::string result = u32ToUtf8(applyStressPattern(dotted, dottedNuclei, pattern));
      TPLOG("    compound result: \"%s\" (was \"%s\")\n", result.c_str(), ipaChunk.c_str());
      return result;
    }
    std::string result = u32ToUtf8(applyStressPattern(stripped, nuclei, pattern));
    TPLOG("    compound result: \"%s\" (was \"%s\")\n", result.c_str(), ipaChunk.c_str());
    return result;
  }

  const std::vector<int>& pattern = it->second;

  // Monosyllables: never override eSpeak's contextual stress on single-syllable
  // words ("for", "the", "a", "blank", etc.).  Only correct multi-syllable words.
  if (pattern.size() <= 1) return ipaChunk;

  // Convert IPA chunk to u32 for codepoint-level processing.
  std::u32string u32 = utf8ToU32(ipaChunk);

  // Strip existing stress marks before counting nuclei.
  std::u32string stripped = stripStress(u32);

  // Count vowel nuclei.
  auto nuclei = findNuclei(stripped);
  if (nuclei.size() != pattern.size()) {
    // Mismatch — eSpeak segmented differently than CMU Dict expected.
    // Do nothing; eSpeak's stress stands.
    return ipaChunk;
  }

  // Safety: never place primary stress (ˈ) on a reduced vowel nucleus.
  // eSpeak chose ə/ᵻ/ɐ/ɚ because it already decided that syllable is
  // reduced — forcing stress onto it can't fix the vowel quality and
  // sounds wrong.  Skip the entire word if any primary lands on reduced.
  for (size_t n = 0; n < nuclei.size() && n < pattern.size(); ++n) {
    if (pattern[n] == 1 && isReducedVowel(stripped[nuclei[n].start])) {
      return ipaChunk;
    }
  }

  // Apply syllable boundaries (dots) first, then re-find nuclei, then stress.
  // Dots must go on the stripped (stress-free) string so positions are clean.
  if (!legalOnsets.empty() && nuclei.size() >= 2) {
    std::u32string dotted = applySyllableBoundaries(stripped, nuclei, legalOnsets);
    auto dottedNuclei = findNuclei(dotted);
    std::u32string corrected = applyStressPattern(dotted, dottedNuclei, pattern);
    return u32ToUtf8(corrected);
  }

  // No onset table or monosyllable — just stress.
  std::u32string corrected = applyStressPattern(stripped, nuclei, pattern);
  return u32ToUtf8(corrected);
}

}  // namespace

// =============================================================================
// Public API
// =============================================================================

std::string runTextParser(
    const std::string& text,
    const std::string& ipa,
    const std::unordered_map<std::string, std::vector<int>>& stressDict,
    const std::unordered_map<std::string, std::vector<std::string>>& compoundMap,
    const std::vector<std::u32string>& legalOnsets,
    const NumberExpansionRules& numberRules,
    const std::unordered_map<std::string, std::string>& ipaOverrides)
{
  if (text.empty()) return ipa;

  // ── Compound split boundary tracking ──────────────────────────────────
  // prepareTextForEspeak() uses \x1F (Unit Separator) between compound
  // halves instead of space.  Scan for these markers and record which
  // adjacent word pairs came from our compound splitting (vs. the user
  // writing separate words).  Replace \x1F with space for normal parsing.
  std::unordered_set<std::string> compoundSplitPairs;
  std::string cleanText;
  {
    cleanText.reserve(text.size());
    std::string prevWord, curWord;
    bool prevSepWasUS = false;

    for (size_t p = 0; p <= text.size(); ++p) {
      char c = (p < text.size()) ? text[p] : ' ';
      bool isSep = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\x1F');
      if (isSep) {
        if (!curWord.empty()) {
          if (prevSepWasUS && !prevWord.empty()) {
            compoundSplitPairs.insert(
                asciiLower(stripPunct(prevWord)) + "\x1F" +
                asciiLower(stripPunct(curWord)));
          }
          prevWord = curWord;
          curWord.clear();
        }
        prevSepWasUS = (c == '\x1F');
        cleanText.push_back(prevSepWasUS ? ' ' : c);
      } else {
        curWord.push_back(c);
        cleanText.push_back(c);
      }
    }
    TPLOG("  compoundSplitPairs: %zu\n", compoundSplitPairs.size());
  }

  auto textWords = splitOnWhitespace(cleanText);
  splitMixedTokens(textWords);

  // ── Number expansion ──────────────────────────────────────────────────
  // Expand numeric text words using YAML rules so "24" becomes
  // ["twenty", "four"], matching eSpeak's 2 IPA words.  Languages
  // without numberExpansion rules fall through to existing heuristics.
  if (numberRules.enabled) {
    std::vector<std::string> expanded;
    expanded.reserve(textWords.size());
    for (const auto& tw : textWords) {
      bool isNum = !tw.empty();
      for (unsigned char ch : tw) {
        if (!std::isdigit(ch) && ch != ',') { isNum = false; break; }
      }
      if (isNum) {
        auto words = expandNumber(tw, numberRules);
        if (!words.empty()) {
          for (auto& w : words) expanded.push_back(std::move(w));
          continue;
        }
      }
      expanded.push_back(tw);
    }
    textWords = std::move(expanded);
  }

  auto ipaChunks = splitIpaWords(ipa);

  // eSpeak sometimes merges number sub-words into a single IPA chunk
  // (e.g. "wˈʌnhˈʌndɹɪd" = one+hundred).  Always split multi-stress
  // chunks when number expansion is active — the old size() guard missed
  // cases where other numbers expanded to MORE IPA chunks, masking the
  // merge (e.g. "100 ... 1.95" → 13 text words but 15 IPA chunks).
  if (numberRules.enabled) {
    splitMultiStressChunks(ipaChunks);
  }

  // ── Compound IPA merge ──────────────────────────────────────────────
  // Phase 2 compound splitting fed "pop corn" to eSpeak for correct vowel
  // quality, but now the IPA has a word boundary between the halves.
  // Merge them back: if adjacent text words concatenate to a compound map
  // key, join their IPA into one word so downstream passes see a single
  // connected word (no word-boundary gap, no word-final allophone rules).
  if (!compoundMap.empty() && textWords.size() == ipaChunks.size()) {
    std::vector<std::string> newText;
    std::vector<std::string> newIpa;
    newText.reserve(textWords.size());
    newIpa.reserve(ipaChunks.size());

    size_t i = 0;
    while (i < textWords.size()) {
      bool merged = false;
      // Try longest span first (3→2) to catch multi-part compounds.
      size_t maxSpan = std::min(static_cast<size_t>(4), textWords.size() - i);
      for (size_t span = maxSpan; span >= 2; --span) {
        std::string combined;
        for (size_t j = 0; j < span; ++j) {
          combined += asciiLower(stripPunct(textWords[i + j]));
        }
        if (compoundMap.find(combined) != compoundMap.end()) {
          // Only merge if WE split this compound (\x1F boundaries).
          // User-written separate words (regular spaces) keep their boundary.
          bool allSplit = true;
          if (!compoundSplitPairs.empty()) {
            for (size_t j = 0; j < span - 1; ++j) {
              std::string pairKey = asciiLower(stripPunct(textWords[i + j])) + "\x1F"
                                  + asciiLower(stripPunct(textWords[i + j + 1]));
              if (compoundSplitPairs.find(pairKey) == compoundSplitPairs.end()) {
                allSplit = false;
                break;
              }
            }
          } else {
            allSplit = false;  // no \x1F markers at all → nothing to merge
          }
          if (!allSplit) {
            TPLOG("  compoundMerge: [%s] skipped (user-written separate words)\n",
                  combined.c_str());
            continue;
          }
          // Merge text and IPA — no space between IPA chunks.
          newText.push_back(combined);
          std::string ipaJoined;
          for (size_t j = 0; j < span; ++j) {
            ipaJoined += ipaChunks[i + j];
          }
          newIpa.push_back(ipaJoined);
          TPLOG("  compoundMerge: [%s] = %zu chunks -> \"%s\"\n",
                combined.c_str(), span, ipaJoined.c_str());
          i += span;
          merged = true;
          break;
        }
      }
      if (!merged) {
        newText.push_back(textWords[i]);
        newIpa.push_back(ipaChunks[i]);
        ++i;
      }
    }
    textWords = std::move(newText);
    ipaChunks = std::move(newIpa);
  }

  if (textWords.empty() || ipaChunks.empty()) return ipa;

  // ── IPA override splicing ────────────────────────────────────────────
  // Words whose dict entry had toIpa set were left untouched in the text
  // so eSpeak could phonemize the surrounding context naturally.  Now
  // replace eSpeak's IPA for those words with the dict-provided IPA.
  //
  // Walk text words and IPA chunks in parallel.  Text may have more
  // words than IPA chunks because NVDA expands punctuation to spoken
  // words (e.g. "quote", ".") that eSpeak doesn't phonemize.  Skip
  // text words with no alpha content when advancing the IPA index.
  bool hadIpaSplice = false;
  std::unordered_set<size_t> splicedChunks;  // IPA-overridden chunks — skip stress correction
  if (!ipaOverrides.empty()) {
    size_t ipaIdx = 0;
    for (size_t i = 0; i < textWords.size(); ++i) {
      std::string key = asciiLower(stripPunct(textWords[i]));
      if (key.empty()) continue;  // punctuation-only word, no IPA chunk
      if (ipaIdx >= ipaChunks.size()) break;
      auto it = ipaOverrides.find(key);
      if (it != ipaOverrides.end()) {
        TPLOG("  ipaSplice: [%s] eSpeak=\"%s\" -> override=\"%s\"\n",
              key.c_str(), ipaChunks[ipaIdx].c_str(), it->second.c_str());
        // Convert spaces to U+001F (phoneme boundary) so the IPA
        // tokenizer treats them as key separators within a single word,
        // not word boundaries.  This enables space-delimited phoneme
        // keys like "k n iː v o_es l" in toIpa fields.
        //
        // Also protect underscores within phoneme keys: ipa_normalize
        // strips all '_' → ' ' (eSpeak pause markers).  We encode '_'
        // as U+F0001 (PUA-A) which survives normalization and gets
        // decoded back to '_' in the tokenizer's \x1F handler.
        std::string spliced;
        spliced.reserve(it->second.size() * 2);
        for (size_t si = 0; si < it->second.size(); ) {
          unsigned char ch = static_cast<unsigned char>(it->second[si]);
          if (ch == ' ') {
            spliced.push_back('\x1F');
            ++si;
          } else if (ch == '_') {
            // U+E001 in UTF-8: EE 80 81 (BMP PUA, survives normalization)
            spliced.push_back(static_cast<char>(0xEE));
            spliced.push_back(static_cast<char>(0x80));
            spliced.push_back(static_cast<char>(0x81));
            ++si;
          } else {
            spliced.push_back(static_cast<char>(ch));
            ++si;
          }
        }
        ipaChunks[ipaIdx] = std::move(spliced);
        splicedChunks.insert(ipaIdx);
        hadIpaSplice = true;
      }
      ++ipaIdx;
    }
  }

  // If there's no stress dict (e.g. en-gb), compound merge was still useful
  // but stress correction can't run.  Reassemble and return.
  if (stressDict.empty()) {
    std::string result;
    for (const auto& c : ipaChunks) {
      if (c.empty()) continue;
      if (!result.empty()) result.push_back(' ');
      result += c;
    }
    TPLOG("  -> no stressDict, returning after compound merge: \"%s\"\n", result.c_str());
    return result;
  }

  TPLOG("--- runTextParser ---\n");
  TPLOG("  text: \"%s\"\n", text.c_str());
  TPLOG("  ipa:  \"%s\"\n", ipa.c_str());
  TPLOG("  textWords(%zu):", textWords.size());
  for (size_t _i = 0; _i < textWords.size(); ++_i)
    TPLOG(" [%s]", textWords[_i].c_str());
  TPLOG("\n");
  TPLOG("  ipaChunks(%zu):", ipaChunks.size());
  for (size_t _i = 0; _i < ipaChunks.size(); ++_i)
    TPLOG(" [%s]", ipaChunks[_i].c_str());
  TPLOG("\n");

  // When word counts don't match, try greedy IPA chunk merging first:
  // eSpeak often splits compound words into separate IPA "words"
  // (e.g. "lockbox" → "lɒk bɒks"), causing a text↔IPA count mismatch.
  // Walk text words and greedily consume IPA chunks until the joined
  // IPA's nucleus count matches the stress dictionary pattern.  If a
  // text word isn't in the dict or no merge works, leave it alone.
  //
  // Fall back to syllable-boundary-only mode for any remaining chunks.
  if (textWords.size() != ipaChunks.size()) {
    bool anyChange = false;

    // Phase 1: Greedy merge — try to align text words to IPA chunks.
    // Build a merged IPA chunk list that parallels textWords.
    if (textWords.size() < ipaChunks.size()) {
      size_t ipaIdx = 0;
      for (size_t tw = 0; tw < textWords.size() && ipaIdx < ipaChunks.size(); ++tw) {
        const std::string key = asciiLower(stripPunct(textWords[tw]));
        auto dictIt = key.empty() ? stressDict.end() : stressDict.find(key);

        TPLOG("  tw=%zu key=\"%s\" ipaIdx=%zu inDict=%s\n",
              tw, key.c_str(), ipaIdx,
              (dictIt != stressDict.end()) ? "yes" : "no");

        if (dictIt != stressDict.end() && dictIt->second.size() >= 2) {
          // We have a stress pattern — try consuming 1..N IPA chunks.
          const size_t expectedNuclei = dictIt->second.size();
          TPLOG("    dict-matched: expectedNuclei=%zu\n", expectedNuclei);
          std::string joined;
          size_t bestEnd = 0;

          for (size_t tryEnd = ipaIdx; tryEnd < ipaChunks.size(); ++tryEnd) {
            if (!joined.empty()) joined += ' ';  // preserve space for correctStress
            joined += ipaChunks[tryEnd];

            // Count nuclei in the joined string.
            std::u32string u32j = utf8ToU32(joined);
            std::u32string strippedj = stripStress(u32j);
            auto nucleij = findNuclei(strippedj);

            if (nucleij.size() == expectedNuclei) {
              bestEnd = tryEnd + 1;
              break;
            }
            if (nucleij.size() > expectedNuclei) break;  // overshot
          }

          TPLOG("    bestEnd=%zu (consumed %zu chunks)\n", bestEnd, bestEnd > ipaIdx ? bestEnd - ipaIdx : 0);

          if (bestEnd > ipaIdx) {
            // Rebuild the joined chunk without spaces (single IPA word).
            std::string merged;
            for (size_t k = ipaIdx; k < bestEnd; ++k) {
              merged += ipaChunks[k];
            }
            TPLOG("    merged=\"%s\"\n", merged.c_str());

            std::string corrected = correctStress(textWords[tw], merged, stressDict, compoundMap, legalOnsets);
            // Always merge the chunks into one, even if stress didn't change.
            // The merge itself is the fix — it reunites split compound IPA.
            ipaChunks[ipaIdx] = (corrected != merged) ? std::move(corrected) : std::move(merged);
            for (size_t k = ipaIdx + 1; k < bestEnd; ++k) {
              ipaChunks[k].clear();
            }
            anyChange = true;
            ipaIdx = bestEnd;
            continue;
          }
        }

        // No dict match or merge failed — figure out how many IPA chunks
        // this text word maps to.  Numbers and abbreviations often expand
        // to multiple IPA words (e.g. "68" → "sˈɪksti ˈeɪt" = 2 chunks),
        // so consuming only 1 chunk misaligns all subsequent corrections.
        //
        // Look-ahead: find the next text word whose multi-syllable dict
        // entry can anchor the alignment, then pick the skip count that
        // places that word's IPA chunk at the right position.
        {
          size_t skip = 1;
          const size_t excess = ipaChunks.size() - textWords.size();

          // Empty-key text words (punctuation like "(", ")") — eSpeak
          // produces no IPA for them. Don't consume any IPA chunks.
          const std::string twKey = asciiLower(stripPunct(textWords[tw]));
          if (twKey.empty()) {
            skip = 0;
            TPLOG("    no-dict skip=0 (empty-key punctuation) -> ipaIdx=%zu\n", ipaIdx);
            ipaIdx += skip;
            continue;
          }

          // Is this text word purely numeric?  Numbers expand to multiple
          // IPA words ("100" → "one hundred" = 2 chunks), so we prefer
          // consuming MORE chunks to avoid misaligning subsequent words.
          bool isNumeric = !textWords[tw].empty();
          for (unsigned char ch : textWords[tw]) {
            if (!std::isdigit(ch) && ch != ',' && ch != '.') {
              isNumeric = false;
              break;
            }
          }

          if (excess > 0) {
            for (size_t probe = tw + 1; probe < textWords.size(); ++probe) {
              const std::string probeKey = asciiLower(stripPunct(textWords[probe]));
              auto probeIt = probeKey.empty() ? stressDict.end()
                                              : stressDict.find(probeKey);
              if (probeIt == stressDict.end() || probeIt->second.size() < 2)
                continue;

              const size_t probeNuclei = probeIt->second.size();
              // Text words between current (tw) and the anchor each consume
              // at least 1 IPA chunk — but skip empty-key words (punctuation
              // like "(" / ")") since eSpeak produces no IPA for them.
              size_t textGap = 0;
              for (size_t g = tw + 1; g < probe; ++g) {
                const std::string gKey = asciiLower(stripPunct(textWords[g]));
                if (!gKey.empty()) ++textGap;
              }

              for (size_t s = 1; s <= excess + 1; ++s) {
                const size_t candidateIdx = ipaIdx + s + textGap;
                if (candidateIdx >= ipaChunks.size()) break;

                std::u32string u32 = utf8ToU32(ipaChunks[candidateIdx]);
                std::u32string stripped = stripStress(u32);
                auto nuclei = findNuclei(stripped);

                if (nuclei.size() == probeNuclei) {
                  skip = s;
                  // Numeric text words: prefer last match (consume more
                  // chunks) since numbers expand to many IPA words and
                  // an early match is likely a number sub-word whose
                  // nucleus count accidentally equals the probe word's.
                  if (!isNumeric) break;
                }
              }
              break;  // only use the first anchoring word
            }
          }
          // Numeric words (e.g. "24") often expand to multiple IPA words
          // ("twenty four") but the look-ahead anchor may fail when the
          // next text word is monosyllabic and doesn't qualify as an anchor.
          // Use remaining word/chunk counts to estimate the correct skip.
          if (isNumeric && skip == 1) {
            size_t remainingText = textWords.size() - tw - 1;
            size_t remainingIpa = 0;
            for (size_t k = ipaIdx; k < ipaChunks.size(); ++k) {
              if (!ipaChunks[k].empty()) ++remainingIpa;
            }
            if (remainingIpa > remainingText + 1) {
              skip = remainingIpa - remainingText;
            }
          }
          TPLOG("    no-dict skip=%zu (numeric=%d) -> ipaIdx=%zu\n", skip, (int)isNumeric, ipaIdx + skip);
          ipaIdx += skip;
        }
      }
    }

    // Phase 2: Apply syllable boundaries to any chunks that weren't
    // stress-corrected above (including single-chunk words and leftovers).
    if (!legalOnsets.empty()) {
      for (size_t i = 0; i < ipaChunks.size(); ++i) {
        if (ipaChunks[i].empty()) continue;

        std::u32string u32 = utf8ToU32(ipaChunks[i]);
        std::u32string stripped = stripStress(u32);
        auto nuclei = findNuclei(stripped);
        if (nuclei.size() < 2) continue;

        std::u32string dotted = applySyllableBoundaries(stripped, nuclei, legalOnsets);
        if (dotted == stripped) continue;

        // Re-insert eSpeak's original stress marks on the dotted string.
        auto dottedNuclei = findNuclei(dotted);
        std::vector<int> pattern(dottedNuclei.size(), 0);
        {
          size_t nIdx = 0;
          bool pendingStress = false;
          int pendingLevel = 0;
          bool inVowel = false;
          for (size_t j = 0; j < u32.size(); ++j) {
            char32_t c = u32[j];
            if (isStressMark(c)) {
              pendingStress = true;
              pendingLevel = (c == U'\u02C8') ? 1 : 2;
              inVowel = false;
            } else if (isIpaVowel(c)) {
              if (!inVowel && nIdx < pattern.size()) {
                if (pendingStress) {
                  pattern[nIdx] = pendingLevel;
                  pendingStress = false;
                }
                inVowel = true;
              }
            } else if (isLengthMark(c) && inVowel) {
              // Extends current nucleus.
            } else if (isTieBar(c) && inVowel) {
              if (j + 1 < u32.size()) ++j;
            } else {
              if (inVowel) {
                nIdx++;
                inVowel = false;
              }
            }
          }
        }

        std::u32string result = applyStressPattern(dotted, dottedNuclei, pattern);
        std::string utf8Result = u32ToUtf8(result);
        if (utf8Result != ipaChunks[i]) {
          ipaChunks[i] = std::move(utf8Result);
          anyChange = true;
        }
      }
    }

    if (!anyChange) {
      TPLOG("  -> no changes, returning original\n");
      return ipa;
    }

    // Reassemble, skipping blanked-out chunks from the merge phase.
    // First collect non-empty chunks, then split multi-stress words
    // for IPA engine word boundaries.
    std::vector<std::string> finalChunks;
    for (size_t i = 0; i < ipaChunks.size(); ++i) {
      if (!ipaChunks[i].empty()) finalChunks.push_back(std::move(ipaChunks[i]));
    }
    splitMultiStressChunks(finalChunks);
    std::string result;
    for (const auto& c : finalChunks) {
      if (c.empty()) continue;
      if (!result.empty()) result.push_back(' ');
      result += c;
    }
    TPLOG("  -> result: \"%s\"\n", result.c_str());
    return result;
  }

  bool anyChange = false;

  for (size_t i = 0; i < textWords.size(); ++i) {
    // IPA-overridden chunks already have correct stress from the dict
    // entry — don't let the stress dict re-pattern them (it can corrupt
    // the \x1F-delimited phoneme sequence).
    if (splicedChunks.count(i)) continue;
    std::string corrected = correctStress(textWords[i], ipaChunks[i], stressDict, compoundMap, legalOnsets);
    if (corrected != ipaChunks[i]) {
      ipaChunks[i] = std::move(corrected);
      anyChange = true;
    }
  }

  if (!anyChange && !hadIpaSplice) return ipa;

  // Reassemble.  Split multi-stress chunks for IPA engine word boundaries.
  splitMultiStressChunks(ipaChunks);
  std::string result;
  for (size_t i = 0; i < ipaChunks.size(); ++i) {
    if (ipaChunks[i].empty()) continue;
    if (!result.empty()) result.push_back(' ');
    result += ipaChunks[i];
  }
  return result;
}

// ── Public API ──

std::string prepareTextForEspeak(
    const std::string& text,
    const std::unordered_map<std::string, std::vector<std::string>>& compoundMap,
    const PronDict& pronDict,
    const std::unordered_set<std::string>& disabledDictTypes,
    const std::string& langTag,
    bool yearSplitting,
    bool thousandsSeparatorCommaToSpace,
    const std::string& ohDigit,
    const std::vector<std::string>& dictSuffixes,
    std::unordered_map<std::string, std::string>* ipaOverrides)
{
  if (text.empty()) return text;

  TPLOG("prepareTextForEspeak IN: \"%s\" lang=%s yearSplit=%d\n",
        text.c_str(), langTag.c_str(), yearSplitting ? 1 : 0);

  std::string result = text;

  // -1. Normalize bracket punctuation: ensure spaces around ( ) [ ].
  // Screen readers expand these to spoken words ("left paren", "right bracket")
  // but may leave the original character glued to adjacent text (e.g. NVDA sends
  // "left paren(Knievel" for "(Knievel)"), breaking dict lookup and word-IPA
  // alignment.  Splitting here lets the tokenizer see each word separately.
  {
    std::string norm;
    norm.reserve(result.size() + 16);
    for (size_t i = 0; i < result.size(); ++i) {
      char c = result[i];
      if (c == '(' || c == ')' || c == '[' || c == ']') {
        if (!norm.empty() && norm.back() != ' ') norm += ' ';
        norm += c;
        if (i + 1 < result.size() && result[i + 1] != ' ') norm += ' ';
      } else {
        norm += c;
      }
    }
    result = std::move(norm);
  }

  // -0.5. Split hyphens between alphanumeric+letter tokens.
  // "M5-series" → "M5 series", "F-16" → "F 16", "COVID-19" → "COVID 19".
  // eSpeak treats hyphenated compounds as single prosodic units, which can
  // cause resyllabification of unrelated words in the same utterance.
  {
    std::string dehyph;
    dehyph.reserve(result.size());
    for (size_t i = 0; i < result.size(); ++i) {
      if (result[i] == '-' && i > 0 && i + 1 < result.size()) {
        bool leftAlnum = std::isalnum(static_cast<unsigned char>(result[i - 1]));
        bool rightAlpha = std::isalpha(static_cast<unsigned char>(result[i + 1]))
                       || std::isdigit(static_cast<unsigned char>(result[i + 1]));
        if (leftAlnum && rightAlpha) {
          dehyph += ' ';
          continue;
        }
      }
      dehyph += result[i];
    }
    result = std::move(dehyph);
  }

  // -0.25. Thousands-separator commas: "44,100" → "44 100" or "44100".
  // Pattern: digit + comma + exactly 3 digits + (non-digit or end).
  // Keeps decimal commas like "3,14" (only 2 digits after comma).
  // For European locales (thousandsSeparatorCommaToSpace): → spaces,
  // because eSpeak recognizes space-separated digit groups as thousands.
  // For all other locales: strip commas entirely so eSpeak sees "44100"
  // and handles it natively (e.g. "forty-four thousand one hundred").
  {
    std::string cleaned;
    cleaned.reserve(result.size());
    for (size_t ci = 0; ci < result.size(); ++ci) {
      if (result[ci] == ',' && ci > 0 &&
          std::isdigit(static_cast<unsigned char>(result[ci - 1])) &&
          ci + 3 < result.size() &&
          std::isdigit(static_cast<unsigned char>(result[ci + 1])) &&
          std::isdigit(static_cast<unsigned char>(result[ci + 2])) &&
          std::isdigit(static_cast<unsigned char>(result[ci + 3])) &&
          (ci + 4 >= result.size() ||
           !std::isdigit(static_cast<unsigned char>(result[ci + 4])))) {
        if (thousandsSeparatorCommaToSpace) cleaned += ' ';
        // else: strip comma entirely
        continue;
      }
      cleaned += result[ci];
    }
    result = std::move(cleaned);
  }

  // 0. Pronunciation dictionary replacement (highest priority).
  // Words with toIpa set are left in place; their overrides are collected
  // in ipaOverrides for downstream splicing in runTextParser.
  if (!pronDict.entries.empty() && disabledDictTypes.count("pronounce") == 0) {
    result = dictReplaceInText(result, pronDict, dictSuffixes, ipaOverrides);
  }

  // 1. Compound splitting.
  if (!compoundMap.empty() && disabledDictTypes.count("compound") == 0) {
    result = splitCompoundsInText(result, compoundMap);
  }

  // 2. English date ordinals ("June 6" → "June 6th").
  if (langTag.size() >= 2 && (langTag[0] == 'e' || langTag[0] == 'E') &&
      (langTag[1] == 'n' || langTag[1] == 'N') &&
      (langTag.size() == 2 || langTag[2] == '-' || langTag[2] == '_')) {
    result = insertDateOrdinals(result);
  }

  // 3. Expand uppercase "AM" → "A. M." so eSpeak spells it as letter names
  //    ("ay em") instead of the word "am".  Dots trigger eSpeak's abbreviation
  //    mode.  Applies to all-caps only ("AM", not "am"/"Am").
  //    "PM" already comes out as "P M" from eSpeak, so no fix needed there.
  if (langTag.size() >= 2 && (langTag[0] == 'e' || langTag[0] == 'E') &&
      (langTag[1] == 'n' || langTag[1] == 'N') &&
      (langTag.size() == 2 || langTag[2] == '-' || langTag[2] == '_')) {
    std::string expanded;
    expanded.reserve(result.size() + 8);
    for (size_t i = 0; i < result.size(); ) {
      if (result[i] == 'A' && i + 1 < result.size() && result[i + 1] == 'M') {
        // Check it's a whole word: not preceded/followed by a letter.
        bool prevOk = (i == 0 || !std::isalpha(static_cast<unsigned char>(result[i - 1])));
        bool nextOk = (i + 2 >= result.size() || !std::isalpha(static_cast<unsigned char>(result[i + 2])));
        if (prevOk && nextOk) {
          expanded += "A. M.";
          i += 2;
          continue;
        }
      }
      expanded += result[i];
      ++i;
    }
    result = std::move(expanded);
  }

  // 4. Time expansion ("6:03" → "6 oh 3", "12:45" → "12 45").
  if (langTag.size() >= 2 && (langTag[0] == 'e' || langTag[0] == 'E') &&
      (langTag[1] == 'n' || langTag[1] == 'N') &&
      (langTag.size() == 2 || langTag[2] == '-' || langTag[2] == '_')) {
    result = expandTimes(result, ohDigit);
  }

  // 5. Separate hyphenated numbers so year splitting can process both.
  // No connector word inserted — platform handles dash announcement.
  // Safe for all languages (no English-specific text injected).
  result = separateHyphenatedNumbers(result);

  // 6. Year splitting ("1995" → "19 95").
  if (yearSplitting) {
    result = splitYears(result, ohDigit);
  }

  TPLOG("prepareTextForEspeak OUT: \"%s\"\n", result.c_str());
  return result;
}

}  // namespace nvsp_frontend
