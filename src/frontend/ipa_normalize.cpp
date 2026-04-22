/*
TGSpeechBox — IPA normalization: raw IPA text -> cleaned u32 string.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include "ipa_normalize.h"

#include "ipa_internal.h"
#include "utf8.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Debug logging for IPA normalization / preReplacement investigation.
// Set to 1 to enable, 0 to disable.
#define IPA_NORM_DEBUG_LOG 0
#if IPA_NORM_DEBUG_LOG
#include <cstdio>
#include <cstdlib>
static FILE* ipaNormLogFile() {
  static FILE* f = nullptr;
  if (!f) {
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = std::getenv("TMP");
    if (!tmp) tmp = "/tmp";
    std::string path = std::string(tmp) + "/tgsb_ipa_norm.log";
    f = std::fopen(path.c_str(), "a");
  }
  return f;
}
static std::string u32toUtf8(const std::u32string& s) {
  std::string out;
  for (char32_t c : s) {
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) { out += (char)(0xC0|(c>>6)); out += (char)(0x80|(c&0x3F)); }
    else if (c < 0x10000) { out += (char)(0xE0|(c>>12)); out += (char)(0x80|((c>>6)&0x3F)); out += (char)(0x80|(c&0x3F)); }
    else { out += (char)(0xF0|(c>>18)); out += (char)(0x80|((c>>12)&0x3F)); out += (char)(0x80|((c>>6)&0x3F)); out += (char)(0x80|(c&0x3F)); }
  }
  return out;
}
#define INLOG(...) do { FILE* _f = ipaNormLogFile(); if (_f) { std::fprintf(_f, __VA_ARGS__); std::fflush(_f); } } while(0)
#else
#define INLOG(...) ((void)0)
#endif

namespace nvsp_frontend {

using ipa_internal::isTieBar;

// IPA vowel codepoint check — used to distinguish diphthong ties (e͡ɪ)
// from affricate ties (t͡s) in the replacement guard.
static bool isIpaVowelChar(char32_t c) {
  switch (c) {
    case U'a': case U'e': case U'i': case U'o': case U'u': case U'y':
    case U'\u0251': case U'\u00E6': case U'\u025B': case U'\u026A':
    case U'\u0254': case U'\u0259': case U'\u028A': case U'\u028C':
    case U'\u0252': case U'\u025C': case U'\u0250': case U'\u0264':
    case U'\u0275': case U'\u0258': case U'\u025E': case U'\u0276':
    case U'\u0268': case U'\u0289': case U'\u026F': case U'\u025D':
    case U'\u025A': case U'\u00F8': case U'\u1D7B': case U'\u1D7F':
    case U'\u00E4':  // ä (used in en-us MOUTH)
      return true;
    default:
      return false;
  }
}

namespace {

// ============================================================================
// Helper functions — internal linkage (anonymous namespace)
// ============================================================================

static inline bool isSpace(char32_t c) {
  return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r';
}

static inline bool isStressMark(char32_t c) {
  return c == U'\u02C8' || c == U'\u02CC';
}

static void collapseWhitespace(std::u32string& s) {
  std::u32string out;
  out.reserve(s.size());
  bool inSpace = true; // trim leading
  for (char32_t c : s) {
    if (isSpace(c)) {
      if (!inSpace) {
        out.push_back(U' ');
        inSpace = true;
      }
    } else {
      out.push_back(c);
      inSpace = false;
    }
  }
  // trim trailing
  while (!out.empty() && out.back() == U' ') out.pop_back();
  s.swap(out);
}

static void removeDelimitedTags(std::u32string& s, char32_t open, char32_t close) {
  std::u32string out;
  out.reserve(s.size());
  bool skipping = false;
  for (char32_t c : s) {
    if (!skipping) {
      if (c == open) {
        skipping = true;
        continue;
      }
      out.push_back(c);
    } else {
      if (c == close) {
        skipping = false;
      }
    }
  }
  s.swap(out);
}

static void replaceAll(std::u32string& s, const std::u32string& from, const std::u32string& to) {
  if (from.empty()) return;
  std::u32string out;
  out.reserve(s.size());

  size_t i = 0;
  while (i < s.size()) {
    if (i + from.size() <= s.size() && s.compare(i, from.size(), from) == 0) {
      out.append(to);
      i += from.size();
    } else {
      out.push_back(s[i]);
      ++i;
    }
  }

  s.swap(out);
}

static bool classContainsNext(const std::unordered_map<std::string, std::vector<std::u32string>>& classes,
                              const std::string& className,
                              const std::u32string& text,
                              size_t nextIndex) {
  if (className.empty()) return true;
  auto it = classes.find(className);
  if (it == classes.end()) return false;
  if (nextIndex >= text.size()) return false;

  // Skip stress marks so rules like "insert schwa before r when beforeClass: VOWELS"
  // still match when eSpeak emits "rˈa" (stress mark between consonant and vowel).
  while (nextIndex < text.size() && isStressMark(text[nextIndex])) {
    ++nextIndex;
  }
  if (nextIndex >= text.size()) return false;

  // Support both single-codepoint and multi-codepoint class members.
  // This allows pack rules like beforeClass: ["t͡ʃ", "d͡ʒ"] if needed.
  for (const auto& member : it->second) {
    if (member.empty()) continue;
    if (nextIndex + member.size() > text.size()) continue;
    if (text.compare(nextIndex, member.size(), member) == 0) return true;
  }
  return false;
}

static bool classContainsPrev(const std::unordered_map<std::string, std::vector<std::u32string>>& classes,
                              const std::string& className,
                              const std::u32string& text,
                              size_t prevIndex) {
  if (className.empty()) return true;
  auto it = classes.find(className);
  if (it == classes.end()) return false;
  if (text.empty()) return false;
  if (prevIndex >= text.size()) return false;

  // Skip stress marks so afterClass rules still match when eSpeak places a stress
  // marker between the previous consonant and the match.
  while (true) {
    if (!isStressMark(text[prevIndex])) break;
    if (prevIndex == 0) return false;
    --prevIndex;
  }

  // Support both single-codepoint and multi-codepoint class members.
  // prevIndex is the index of the character immediately before the match.
  for (const auto& member : it->second) {
    if (member.empty()) continue;
    if (member.size() > prevIndex + 1) continue;
    const size_t start = (prevIndex + 1) - member.size();
    if (text.compare(start, member.size(), member) == 0) return true;
  }
  return false;
}

static bool isWordBoundaryBefore(const std::u32string& text, size_t pos) {
  if (pos == 0) return true;
  return text[pos - 1] == U' ';
}

static bool isWordBoundaryAfter(const std::u32string& text, size_t posAfter) {
  // posAfter is index immediately after the match
  if (posAfter >= text.size()) return true;
  return text[posAfter] == U' ';
}

// Find the first IPA character of the next word (skip space + stress marks).
// Returns the index, or text.size() if no next word.
static size_t nextWordFirstChar(const std::u32string& text, size_t posAfterMatch) {
  size_t i = posAfterMatch;
  // Skip to space
  while (i < text.size() && text[i] != U' ') ++i;
  // Skip space(s)
  while (i < text.size() && text[i] == U' ') ++i;
  // Skip stress marks
  while (i < text.size() && isStressMark(text[i])) ++i;
  return i;
}

// Find the last IPA character of the previous word (skip space + stress marks backwards).
// Returns the index, or SIZE_MAX if no previous word.
static size_t prevWordLastChar(const std::u32string& text, size_t matchStart) {
  if (matchStart == 0) return SIZE_MAX;
  size_t i = matchStart - 1;
  // Skip space(s) backwards
  while (i > 0 && text[i] == U' ') --i;
  if (text[i] == U' ') return SIZE_MAX;  // was at start
  // Skip stress marks backwards
  while (i > 0 && isStressMark(text[i])) --i;
  return i;
}

// Match a pattern at text[pos], treating IPA tie bars as optional on both sides.
// This lets pack rules written as "a͡ɪ" match both "a͡ɪ" and "aɪ" (and similarly for affricates).
// outConsumed is the number of codepoints consumed from *text*.
static bool matchAtLooseTie(const std::u32string& text, size_t pos,
                            const std::u32string& pat,
                            size_t& outConsumed) {
  outConsumed = 0;
  size_t t = pos;
  size_t p = 0;

  while (p < pat.size()) {
    // Skip tie bars in the pattern.
    if (p < pat.size() && isTieBar(pat[p])) {
      ++p;
      continue;
    }

    // Skip tie bars in the text.
    while (t < text.size() && isTieBar(text[t])) {
      ++t;
      ++outConsumed;
    }

    if (t >= text.size()) return false;
    if (text[t] != pat[p]) return false;

    ++t;
    ++p;
    ++outConsumed;
  }

  return true;
}

static std::u32string chooseReplacementTarget(const PackSet& pack, const std::vector<std::u32string>& candidates) {
  for (const auto& c : candidates) {
    if (c.empty()) return c;
    if (hasPhoneme(pack, c)) return c;
  }
  // If none exist, still return the first so the rule is deterministic.
  return candidates.empty() ? std::u32string{} : candidates.front();
}

static void applyRules(std::u32string& text, const PackSet& pack, const std::vector<ReplacementRule>& rules, std::vector<bool>* protResult = nullptr) {
  const bool textHasTie = (text.find(U'\u0361') != std::u32string::npos) || (text.find(U'\u035C') != std::u32string::npos);

  // Track positions produced by earlier replacements so subsequent rules
  // don't match inside them.  Prevents cascade corruption where e.g.
  // "a→a_es" followed by "e→e_es" would mangle the "e" inside "a_es".
  //
  // Key insight: cascade corruption only happens when a replacement is
  // LONGER than the matched text — that's when new character positions
  // appear that weren't in the original.  Same-length (or shorter)
  // replacements just swap characters in-place, so no new matchable
  // substrings are created and intentional chaining is safe
  // (e.g. u→ᵾ then ᵾ→ᵿ, or uː→ᵾː then ᵾ→ᵿ).
  std::vector<bool> prot(text.size(), false);
  std::vector<bool> crossProt;  // only used when protResult != nullptr
  if (protResult) crossProt.assign(text.size(), false);

  int ruleIdx = 0;
  for (const auto& rule : rules) {
    if (rule.from.empty()) { ++ruleIdx; continue; }

    const bool patHasTie = (rule.from.find(U'\u0361') != std::u32string::npos) || (rule.from.find(U'\u035C') != std::u32string::npos);
    const bool useLooseTie = (rule.from.size() > 1) && (textHasTie || patHasTie);

    // Debug: trace rules that contain ɑ
    bool traceThis = (rule.from.find(U'\u0251') != std::u32string::npos)
                  || (rule.from.find(U'\u0263') != std::u32string::npos); // added u0263 trace
    if (traceThis) {
      INLOG("RULE[%d] from='%s' useLooseTie=%d textHasTie=%d text='%s'\n",
            ruleIdx, u32toUtf8(rule.from).c_str(), useLooseTie, textHasTie,
            u32toUtf8(text).c_str());
    }

    // Fast skip: only safe when we can rely on direct substring search.
    // If tie bars are involved, a pattern like "a͡ɪ" should also match "aɪ", so
    // we can't skip purely on text.find(rule.from).
    if (!useLooseTie) {
      if (text.find(rule.from) == std::u32string::npos) {
        if (traceThis) INLOG("  SKIP (not found)\n");
        continue;
      }
    } else if (patHasTie) {
      // If the pattern has a tie bar, also check the no-tie variant.
      std::u32string noTie;
      noTie.reserve(rule.from.size());
      for (char32_t c : rule.from) {
        if (!isTieBar(c)) noTie.push_back(c);
      }
      if (text.find(rule.from) == std::u32string::npos && (!noTie.empty() && text.find(noTie) == std::u32string::npos)) {
        continue;
      }
    } else {
      // Pattern has no tie bar but the text might. We can't cheaply skip in the general case.
      // Still safe to skip for single-codepoint patterns.
      if (rule.from.size() == 1 && text.find(rule.from) == std::u32string::npos) {
        continue;
      }
    }

    const auto to = chooseReplacementTarget(pack, rule.to);

    std::u32string out;
    out.reserve(text.size());
    std::vector<bool> outProt;
    outProt.reserve(text.size());
    // Cross-phase protection: only genuinely NEW growth positions get
    // PUA-A escaped (visible to escapeProtected).  Inherited chars from
    // growth replacements stay visible to the next phase's rules.
    std::vector<bool> outCrossProt;
    if (protResult) outCrossProt.reserve(text.size());

    size_t i = 0;
    while (i < text.size()) {
      // Skip positions that were produced by a previous rule's replacement.
      if (prot[i]) {
        out.push_back(text[i]);
        outProt.push_back(true);
        if (protResult) outCrossProt.push_back(crossProt[i]);
        ++i;
        continue;
      }

      bool matched = false;
      size_t matchLen = 0; // number of codepoints consumed from text

      if (!useLooseTie) {
        if (i + rule.from.size() <= text.size() && text.compare(i, rule.from.size(), rule.from) == 0) {
          matched = true;
          matchLen = rule.from.size();
        }
      } else {
        size_t consumed = 0;
        if (matchAtLooseTie(text, i, rule.from, consumed)) {
          matched = true;
          matchLen = consumed;
        }
      }

      // Reject match if any position in the match range is protected.
      if (matched && matchLen > 1) {
        for (size_t p = i + 1; p < i + matchLen; ++p) {
          if (prot[p]) { matched = false; break; }
        }
      }

      if (matched) {
        const size_t matchStart = i;
        const size_t matchEnd = i + matchLen;

        bool ok = true;
        if (rule.when.atWordStart && !isWordBoundaryBefore(text, matchStart)) ok = false;
        if (rule.when.atWordEnd && !isWordBoundaryAfter(text, matchEnd)) ok = false;
        if (ok && !rule.when.beforeClass.empty()) {
          ok = classContainsNext(pack.lang.classes, rule.when.beforeClass, text, matchEnd);
        }
        if (ok && !rule.when.afterClass.empty()) {
          if (matchStart == 0) {
            ok = false;
          } else {
            ok = classContainsPrev(pack.lang.classes, rule.when.afterClass, text, matchStart - 1);
          }
        }
        // Negative class conditions: fail if char IS in the forbidden class
        if (ok && !rule.when.notBeforeClass.empty()) {
          // Fail if next char exists AND is in the forbidden class
          if (classContainsNext(pack.lang.classes, rule.when.notBeforeClass, text, matchEnd)) {
            ok = false;
          }
        }
        if (ok && !rule.when.notAfterClass.empty()) {
          // Fail if prev char exists AND is in the forbidden class
          if (matchStart > 0 && classContainsPrev(pack.lang.classes, rule.when.notAfterClass, text, matchStart - 1)) {
            ok = false;
          }
        }

        // Cross-word class conditions: look at adjacent words' boundary chars.
        if (ok && !rule.when.nextWordStartsClass.empty()) {
          size_t nwi = nextWordFirstChar(text, matchEnd);
          if (nwi >= text.size()) {
            ok = false;  // no next word = silence; doesn't match
          } else {
            ok = classContainsNext(pack.lang.classes, rule.when.nextWordStartsClass, text, nwi);
          }
        }
        if (ok && !rule.when.nextWordStartsNotClass.empty()) {
          size_t nwi = nextWordFirstChar(text, matchEnd);
          // No next word (silence) passes the NOT condition
          if (nwi < text.size() &&
              classContainsNext(pack.lang.classes, rule.when.nextWordStartsNotClass, text, nwi)) {
            ok = false;
          }
        }
        if (ok && !rule.when.prevWordEndsClass.empty()) {
          size_t pwi = prevWordLastChar(text, matchStart);
          if (pwi == SIZE_MAX) {
            ok = false;  // no prev word
          } else {
            ok = classContainsPrev(pack.lang.classes, rule.when.prevWordEndsClass, text, pwi);
          }
        }
        if (ok && !rule.when.prevWordEndsNotClass.empty()) {
          size_t pwi = prevWordLastChar(text, matchStart);
          if (pwi != SIZE_MAX &&
              classContainsPrev(pack.lang.classes, rule.when.prevWordEndsNotClass, text, pwi)) {
            ok = false;
          }
        }

        // Don't replace inside a tied AFFRICATE sequence.  A tie bar
        // immediately before the match means this phoneme is bound to its
        // predecessor (e.g. "s" inside "t͡s") and must not be split out.
        // But diphthong ties (e͡ɪ, a͡ɪ, o͡ʊ) must allow replacement so
        // the offglide gets its language-specific variant (ɪ→ɪ_es).
        // Distinguish by checking whether the char before the tie bar
        // is a vowel (diphthong) or consonant (affricate).
        if (ok && matchStart > 0 && isTieBar(text[matchStart - 1])) {
          // Find the character before the tie bar.
          bool isDiphthongTie = false;
          if (matchStart >= 2) {
            char32_t preTie = text[matchStart - 2];
            // Skip stress/prosody marks to find the actual phoneme.
            size_t scan = matchStart - 2;
            while (scan > 0 && !isIpaVowelChar(preTie) &&
                   (preTie == U'\u02C8' || preTie == U'\u02CC' ||  // ˈ ˌ
                    preTie == U'\u02D0' || preTie == U'\u02D1')) {  // ː ˑ
              --scan;
              preTie = text[scan];
            }
            isDiphthongTie = isIpaVowelChar(preTie);
          }
          if (!isDiphthongTie) ok = false;
        }

        if (traceThis) {
          INLOG("  MATCH at %zu ok=%d\n", matchStart, ok);
        }
        if (ok) {
          if (traceThis) INLOG("  APPLIED: '%s' -> '%s'\n", u32toUtf8(rule.from).c_str(), u32toUtf8(to).c_str());
          out.append(to);
          // Within-phase: protect ALL positions of growth replacements
          // from cascade corruption (s→s_es then s→s_mx matching the
          // s inside s_es).  Same-length or shorter replacements are
          // safe to chain (e.g. u→ᵾ then ᵾ→ᵿ).
          const bool grew = (to.size() > matchLen);
          for (size_t p = 0; p < to.size(); ++p) {
            outProt.push_back(grew);
            // Cross-phase: only genuinely new growth positions get
            // PUA-A escaped.  Inherited chars (p < matchLen) stay
            // visible so the replacements phase can still match them
            // (e.g. preReplacement fɔːɹ→fɔːᵊɹ leaves ɔ visible for
            // the ɔ→ᴐ allophone rule).
            if (protResult) outCrossProt.push_back(grew && p >= matchLen);
          }
          i = matchEnd;
          continue;
        }
      }

      out.push_back(text[i]);
      outProt.push_back(false);
      if (protResult) outCrossProt.push_back(false);
      ++i;
    }

    text.swap(out);
    prot.swap(outProt);
    if (protResult) crossProt.swap(outCrossProt);
    ++ruleIdx;
  }
  if (protResult) *protResult = std::move(crossProt);
}

// Escape characters that were protected by a prior applyRules() call into
// Unicode Supplementary Private Use Area-A (U+F0000-U+FFFFD).  This makes
// them invisible to subsequent cleanup passes and replacement rules.
// unescapeProtected() reverses the transformation.
static void escapeProtected(std::u32string& text, const std::vector<bool>& prot) {
  for (size_t i = 0; i < text.size() && i < prot.size(); ++i) {
    if (prot[i]) text[i] += 0xF0000;
  }
}

static void unescapeProtected(std::u32string& text) {
  for (auto& c : text) {
    if (c >= 0xF0000 && c <= 0xFFFFF) c -= 0xF0000;
  }
}

static void applyAliases(std::u32string& text, const PackSet& pack) {
  // Apply longest-first so more specific tokens win.
  std::vector<std::pair<std::u32string, std::u32string>> items;
  items.reserve(pack.lang.aliases.size());
  for (const auto& kv : pack.lang.aliases) {
    items.push_back(kv);
  }
  std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
    return a.first.size() > b.first.size();
  });

  for (const auto& kv : items) {
    replaceAll(text, kv.first, kv.second);
  }
}

} // anonymous namespace

namespace internal {

std::u32string normalizeIpaText(const PackSet& pack, const std::string& ipaUtf8) {
  std::u32string t = utf8ToU32(ipaUtf8);

  // Strip any codepoints in Supplementary PUA-A (U+F0000-U+FFFFD) from the
  // input.  We use this range internally to protect characters during allophone
  // cascade processing; any PUA-A arriving from eSpeak or user dictionaries
  // would collide with our escape/unescape logic.
  t.erase(std::remove_if(t.begin(), t.end(),
      [](char32_t c) { return c >= 0xF0000 && c <= 0xFFFFF; }), t.end());

  // Normalize tie bar variants early so pack rules can match reliably.
  replaceAll(t, U"\u035C", U"\u0361");

  // eSpeak pause/separator underscores — strip BEFORE any replacement rules
  // so that both preReplacements and replacements can safely output phoneme
  // keys containing underscores (e.g. ɣ_es, a_es).
  replaceAll(t, U"_:", U" ");
  replaceAll(t, U"_", U" ");

  // 1) Pack pre-replacements (lets you preserve info before we strip chars like '-').
  // Carry protection forward: escape protected characters into Supplementary
  // PUA-A so that cleanup passes and the replacements phase cannot mangle them.
  INLOG("PRE-IN:  %s\n", u32toUtf8(t).c_str());
  std::vector<bool> preProt;
  applyRules(t, pack, pack.lang.preReplacements, &preProt);
  INLOG("PRE-OUT: %s\n", u32toUtf8(t).c_str());
  escapeProtected(t, preProt);

  // 2) Basic cleanup, mirroring ipa_convert.py defaults.
  // Remove ZWJ/ZWNJ.
  replaceAll(t, U"\u200D", U"");
  replaceAll(t, U"\u200C", U"");

  // Strip IPA syllable boundary dots (U+002E).
  // eSpeak inserts these as syllable markers. They're informational, not
  // phonemic, and cause false word splits when hyphens trigger resyllabification
  // (e.g. "M5-series" makes eSpeak split "powerful" across a dot boundary).
  replaceAll(t, U".", U"");

  // Strip tags like (en), [bg], {xx}.
  removeDelimitedTags(t, U'(', U')');
  removeDelimitedTags(t, U'[', U']');
  removeDelimitedTags(t, U'{', U'}');

  // Remove wrapper punctuation.
  for (char32_t c : std::u32string(U"[](){}\\/")) {
    std::u32string from;
    from.push_back(c);
    replaceAll(t, from, U"");
  }

  // eSpeak utility codes.
  replaceAll(t, U"||", U" ");
  for (char32_t c : std::u32string(U"|%=")) {
    std::u32string from;
    from.push_back(c);
    replaceAll(t, from, U"");
  }

  if (pack.lang.stripHyphen) {
    replaceAll(t, U"-", U"");
  }

  // Stress/length markers.
  replaceAll(t, U"'", U"\u02C8");
  replaceAll(t, U",", U"\u02CC");
  replaceAll(t, U":", U"\u02D0");
  // --- IPA normalisation / fallbacks (match legacy ipa_bestversion.py) ---
  // eSpeak's espeak_TextToPhonemes() IPA mode frequently uses tied sequences
  // to represent syllabic /-l/ endings (e.g. "level" -> ...ə͡l, "cancel" -> ...ə͡l).
  // If we treat these as a single phoneme key, the /l/ can disappear entirely.
  // The legacy Python pipeline normalized these into schwa + l.
  replaceAll(t, U"l\u0329", U"\u0259l");
  replaceAll(t, U"\u026B\u0329", U"\u0259l");
  replaceAll(t, U"\u0259\u0361l", U"\u0259l");
  replaceAll(t, U"\u028A\u0361l", U"\u0259l");

  // Strip diphthong tie bars (vowel͡vowel).  eSpeak marks diphthongs
  // with tie bars (e͡ɪ, a͡ɪ, o͡ʊ) which block dialect replacements
  // (ɪ→ɪ_es, ʊ→ʊ_es) because the tie bar guard protects against
  // splitting affricates.  Removing them here is safe: autoTieDiphthongs
  // will re-tie vowel pairs later.  Affricate tie bars (t͡s, t͡ʃ, d͡ʒ)
  // are preserved — both sides are consonants.
  {
    std::u32string cleaned;
    cleaned.reserve(t.size());
    for (size_t i = 0; i < t.size(); ++i) {
      if (isTieBar(t[i]) && i > 0 && i + 1 < t.size() &&
          isIpaVowelChar(t[i - 1]) && isIpaVowelChar(t[i + 1])) {
        continue;  // strip vowel͡vowel tie bar
      }
      cleaned.push_back(t[i]);
    }
    t = std::move(cleaned);
  }

  // Allophone digits (eSpeak often uses '2').
  if (pack.lang.stripAllophoneDigits) {
    // Keep 1-5 for tone digits if tonal.
    for (char32_t d = U'0'; d <= U'9'; ++d) {
      if (pack.lang.tonal && pack.lang.toneDigitsEnabled && (d >= U'1' && d <= U'5')) {
        continue;
      }
      if (d == U'2') {
        std::u32string from;
        from.push_back(d);
        replaceAll(t, from, U"");
      }
    }
  }

  collapseWhitespace(t);

  // 3) Aliases and replacements.
  applyAliases(t, pack);
  applyRules(t, pack, pack.lang.replacements);

  // Restore characters that were protected from the preReplacements phase.
  unescapeProtected(t);

  collapseWhitespace(t);
  return t;
}

} // namespace internal
} // namespace nvsp_frontend
