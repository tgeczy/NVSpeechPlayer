/*
TGSpeechBox — Pre-eSpeak text transforms (numbers, dates, compounds, years).
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include "text_prepare.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

// Debug logging (set to 1 to enable, 0 to disable).
#define TPREP_DEBUG_LOG 0
#if TPREP_DEBUG_LOG
#include <cstdio>
static FILE* tprepLogFile() {
  static FILE* f = nullptr;
  if (!f) {
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = std::getenv("TMP");
    if (!tmp) tmp = "/tmp";
    std::string path = std::string(tmp) + "/tgsb_textprepare.log";
    f = std::fopen(path.c_str(), "a");
  }
  return f;
}
#define TPREP(...) do { FILE* _f = tprepLogFile(); if (_f) { std::fprintf(_f, __VA_ARGS__); std::fflush(_f); } } while(0)
#else
#define TPREP(...) ((void)0)
#endif

namespace nvsp_frontend {

namespace {

// Characters that eSpeak expands to spoken words (e.g. % -> "percent").
static bool isExpandedSymbol(char c) {
  switch (c) {
    case '%': case '$': case '#': case '+': case '&': case '@':
      return true;
    default:
      return false;
  }
}

// ── English month names for date ordinal insertion ──

static const char* const kEnglishMonths[] = {
  "january", "february", "march", "april", "may", "june",
  "july", "august", "september", "october", "november", "december",
  "jan", "feb", "mar", "apr", "jun", "jul", "aug", "sep", "sept", "oct", "nov", "dec"
};
static const int kNumMonthNames = sizeof(kEnglishMonths) / sizeof(kEnglishMonths[0]);

static bool isEnglishMonth(const std::string& word) {
  // Require first letter uppercase — "March 23" is a date, "march 23" may be a verb.
  if (word.empty() || !std::isupper(static_cast<unsigned char>(word[0]))) return false;
  std::string lower = word;
  for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (int i = 0; i < kNumMonthNames; ++i) {
    if (lower == kEnglishMonths[i]) return true;
  }
  return false;
}

static const char* ordinalSuffix(int n) {
  if (n >= 11 && n <= 13) return "th";
  switch (n % 10) {
    case 1: return "st";
    case 2: return "nd";
    case 3: return "rd";
    default: return "th";
  }
}

// ── Time expansion core ──

static std::string expandTimeCore(const std::string& hourStr, int min,
                                  char minTens, char minOnes,
                                  const std::string& ohDigit) {
  if (min == 0) return hourStr + " o'clock";
  if (minTens == '0') {
    std::string oh = ohDigit.empty() ? "oh" : ohDigit;
    return hourStr + " " + oh + " " + minOnes;
  }
  return hourStr + " " + minTens + minOnes;
}

}  // namespace

// ── Number expansion ──────────────────────────────────────────────────────
//
// Expand a numeric text word into its spoken-word components using
// language-pack YAML rules.  Returns empty vector if rules are disabled,
// the string isn't a pure integer, or the word lists are too short.
// Commas are stripped ("1,000" -> 1000).
// Leading zeros trigger digit-by-digit reading ("07" -> "zero seven").

std::vector<std::string> expandNumber(
    const std::string& numStr,
    const NumberExpansionRules& rules)
{
  if (!rules.enabled) return {};
  if (rules.digits.size() < 10 || rules.teens.size() < 10 || rules.tens.size() < 10)
    return {};

  // Strip commas.
  std::string cleaned;
  cleaned.reserve(numStr.size());
  for (char c : numStr) {
    if (c != ',') cleaned.push_back(c);
  }

  // Reject non-pure-digit strings (decimals, negatives -> Phase 2).
  if (cleaned.empty()) return {};
  for (unsigned char c : cleaned) {
    if (!std::isdigit(c)) return {};
  }

  // Leading zeros -> digit-by-digit (eSpeak behavior: "07" -> "zero seven").
  if (cleaned.size() > 1 && cleaned[0] == '0') {
    std::vector<std::string> result;
    for (char c : cleaned) {
      result.push_back(rules.digits[c - '0']);
    }
    return result;
  }

  // Parse as uint64.
  unsigned long long val = 0;
  try {
    val = std::stoull(cleaned);
  } catch (...) {
    return {};  // overflow or parse failure
  }

  if (val == 0) return { rules.digits[0] };  // "zero"

  // Two-digit expansion (1-99).
  auto expandTwoDigit = [&](unsigned int n, std::vector<std::string>& out) {
    if (n < 10) {
      out.push_back(rules.digits[n]);
    } else if (n < 20) {
      out.push_back(rules.teens[n - 10]);
    } else {
      out.push_back(rules.tens[n / 10]);
      if (n % 10 != 0) {
        out.push_back(rules.digits[n % 10]);
      }
    }
  };

  // Group expansion (1-999).
  auto expandGroup = [&](unsigned int n, std::vector<std::string>& out) {
    if (n >= 100) {
      out.push_back(rules.digits[n / 100]);
      out.push_back(rules.hundred);
      unsigned int rem = n % 100;
      if (rem > 0) {
        if (!rules.conjunction.empty())
          out.push_back(rules.conjunction);
        expandTwoDigit(rem, out);
      }
    } else {
      expandTwoDigit(n, out);
    }
  };

  // Beyond billions (trillions+), we don't have YAML words — return empty
  // so the caller falls back to eSpeak's alignment heuristics.
  // eSpeak handles up to septillion natively, so let it do the work.
  if (val > 999999999999ULL) return {};

  std::vector<std::string> result;

  struct Scale { unsigned long long divisor; const std::string* word; };
  Scale scales[] = {
    { 1000000000ULL, &rules.billion  },
    { 1000000ULL,    &rules.million  },
    { 1000ULL,       &rules.thousand },
  };

  for (const auto& s : scales) {
    if (val >= s.divisor && !s.word->empty()) {
      expandGroup(static_cast<unsigned int>(val / s.divisor), result);
      result.push_back(*s.word);
      val %= s.divisor;
    }
  }
  if (val > 0) {
    // British "and" between thousands+ and a sub-100 remainder.
    if (!result.empty() && val < 100 && !rules.conjunction.empty())
      result.push_back(rules.conjunction);
    expandGroup(static_cast<unsigned int>(val), result);
  }

  return result;
}

// ── Mixed token splitting ─────────────────────────────────────────────────
//
// Further split text words at digit->alpha boundaries and around symbols
// that eSpeak expands into spoken words.
// "25Increasing" -> ["25", "Increasing"]
// "100%"         -> ["100", "%"]
// Normal words like "bonus;" are left intact (stripPunct handles trailing).

void splitMixedTokens(std::vector<std::string>& words) {
  std::vector<std::string> result;
  result.reserve(words.size());
  for (const auto& w : words) {
    if (w.size() < 2) {
      result.push_back(w);
      continue;
    }

    size_t start = 0;
    for (size_t i = 1; i < w.size(); ++i) {
      unsigned char prev = static_cast<unsigned char>(w[i - 1]);
      unsigned char cur  = static_cast<unsigned char>(w[i]);

      bool split = false;
      // digit -> alpha: "25Increasing"
      if (std::isdigit(prev) && std::isalpha(cur)) split = true;
      // digit -> expanded symbol: "100%"
      if (std::isdigit(prev) && isExpandedSymbol(static_cast<char>(cur))) split = true;
      // expanded symbol -> alpha/digit: "%EXP", "%100"
      if (isExpandedSymbol(static_cast<char>(prev)) && (std::isalpha(cur) || std::isdigit(cur))) split = true;
      // comma between alpha and digit: "neelix,2649" -> "neelix" + "2649"
      if (prev == ',' && std::isdigit(cur)) split = true;
      if (std::isalpha(prev) && cur == ',') split = true;

      if (split) {
        result.push_back(w.substr(start, i - start));
        start = i;
      }
    }
    if (start < w.size()) {
      result.push_back(w.substr(start));
    }
  }
  words = std::move(result);
}

// ── Compound splitting ────────────────────────────────────────────────────

std::string splitCompoundsInText(
    const std::string& text,
    const std::unordered_map<std::string, std::vector<std::string>>& compoundMap)
{
  if (text.empty() || compoundMap.empty()) return text;

  std::string result;
  result.reserve(text.size() + 32);

  size_t i = 0;
  while (i < text.size()) {
    // Skip whitespace — copy as-is.
    if (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r') {
      result.push_back(text[i]);
      ++i;
      continue;
    }

    // Find the end of this word token.
    size_t wordStart = i;
    while (i < text.size() && text[i] != ' ' && text[i] != '\t' &&
           text[i] != '\n' && text[i] != '\r') {
      ++i;
    }
    std::string token = text.substr(wordStart, i - wordStart);

    // Strip leading and trailing punctuation for lookup.
    size_t lo = 0;
    while (lo < token.size() && std::ispunct(static_cast<unsigned char>(token[lo])))
      ++lo;
    size_t hi = token.size();
    while (hi > lo && std::ispunct(static_cast<unsigned char>(token[hi - 1])))
      --hi;

    if (lo >= hi) {
      // All punctuation — just emit.
      result += token;
      continue;
    }

    std::string core = token.substr(lo, hi - lo);
    std::string key = core;
    for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto it = compoundMap.find(key);
    if (it == compoundMap.end()) {
      result += token;
      continue;
    }

    // Found compound.  Replace core with halves joined by spaces.
    // Preserve leading/trailing punctuation.
    const auto& halves = it->second;

    // Preserve original casing of first letter if the source was capitalized.
    // For simplicity, just emit the halves as lowercase (eSpeak is case-insensitive
    // for phonemization purposes).
    result += token.substr(0, lo);  // leading punctuation
    for (size_t h = 0; h < halves.size(); ++h) {
      if (h > 0) result.push_back('\x1F');  // Unit Separator — not space
      result += halves[h];
    }
    result += token.substr(hi);  // trailing punctuation

    TPREP("  splitCompound: \"%s\" -> halves[%zu]\n", token.c_str(), halves.size());
  }

  return result;
}

// ── English date ordinals ─────────────────────────────────────────────────
//
// "June 6" -> "June 6th", "6 June" -> "6th June"
// Only bare numbers 1-31 adjacent to a month name.

std::string insertDateOrdinals(const std::string& text) {
  // Split into whitespace-separated tokens, preserving whitespace.
  struct Tok { std::string s; bool isWs; };
  std::vector<Tok> toks;
  size_t i = 0;
  while (i < text.size()) {
    size_t start = i;
    bool ws = (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r');
    if (ws) {
      while (i < text.size() && (text[i] == ' ' || text[i] == '\t' ||
             text[i] == '\n' || text[i] == '\r')) ++i;
    } else {
      while (i < text.size() && text[i] != ' ' && text[i] != '\t' &&
             text[i] != '\n' && text[i] != '\r') ++i;
    }
    toks.push_back({text.substr(start, i - start), ws});
  }

  // Look for month + bare number or bare number + month patterns.
  bool changed = false;
  for (size_t t = 0; t < toks.size(); ++t) {
    if (toks[t].isWs) continue;

    // Check if this token is a bare number 1-31 (no existing suffix).
    const std::string& s = toks[t].s;
    // Strip trailing punctuation for the digit check.
    size_t numEnd = s.size();
    while (numEnd > 0 && std::ispunct(static_cast<unsigned char>(s[numEnd - 1])) &&
           !std::isdigit(static_cast<unsigned char>(s[numEnd - 1]))) --numEnd;
    if (numEnd == 0) continue;

    bool allDigits = true;
    for (size_t c = 0; c < numEnd; ++c) {
      if (!std::isdigit(static_cast<unsigned char>(s[c]))) { allDigits = false; break; }
    }
    if (!allDigits) continue;

    int val = std::atoi(s.substr(0, numEnd).c_str());
    if (val < 1 || val > 31) continue;

    // Already has ordinal suffix? (e.g. "6th", "1st")
    if (numEnd < s.size()) {
      std::string trail = s.substr(numEnd);
      std::string trailLower = trail;
      for (auto& c : trailLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (trailLower == "st" || trailLower == "nd" || trailLower == "rd" || trailLower == "th") continue;
    }

    // Look for adjacent month.  Only "month number" (backward) is safe.
    // "number month" (forward) causes false positives like "column 2 March".
    auto prevIsMonth = [&]() -> bool {
      size_t j = t - 1;
      if (j >= toks.size()) return false;
      if (toks[j].isWs && j > 0) --j;
      if (j >= toks.size() || toks[j].isWs) return false;
      return isEnglishMonth(toks[j].s);
    };

    if (prevIsMonth()) {
      // Insert ordinal suffix after the digits, before trailing punctuation.
      // Use the parsed int to strip leading zeros ("06" -> "6th").
      const char* suf = ordinalSuffix(val);
      toks[t].s = std::to_string(val) + suf + s.substr(numEnd);
      changed = true;
      TPREP("  dateOrdinal: \"%s\" -> \"%s\"\n", s.c_str(), toks[t].s.c_str());
    }
  }

  if (!changed) return text;

  std::string result;
  result.reserve(text.size() + 32);
  for (const auto& tok : toks) result += tok.s;
  return result;
}

// ── Time expansion ────────────────────────────────────────────────────────
//
// Expand time patterns so eSpeak reads times naturally:
//   "6:03"       -> "6 oh 3"    (raw colon — Android/iOS/SAPI)
//   "6 colon 03" -> "6 oh 3"    (NVDA-expanded punctuation)
//   "12:45"      -> "12 45"     (two-digit minute)
//   "5:00"       -> "5 o'clock"
// Handles both raw ":" and NVDA's "colon" expansion.

std::string expandTimes(const std::string& text, const std::string& ohDigit) {
  std::string result;
  result.reserve(text.size() + 16);
  size_t i = 0;
  while (i < text.size()) {
    if (std::isdigit(static_cast<unsigned char>(text[i]))) {
      size_t numStart = i;
      while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
      size_t hourLen = i - numStart;

      if (hourLen <= 2) {
        // Try raw colon: "6:03"
        if (i < text.size() && text[i] == ':' &&
            i + 2 < text.size() &&
            std::isdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isdigit(static_cast<unsigned char>(text[i + 2])) &&
            (i + 3 >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i + 3])))) {
          int hour = std::atoi(text.substr(numStart, hourLen).c_str());
          int min  = (text[i + 1] - '0') * 10 + (text[i + 2] - '0');
          if (hour <= 23 && min <= 59) {
            std::string hourStr = text.substr(numStart, hourLen);
            result += expandTimeCore(hourStr, min, text[i + 1], text[i + 2], ohDigit);
            TPREP("  timeExpand(raw): \"%s\" -> ...\n",
                  text.substr(numStart, (i + 3) - numStart).c_str());
            i += 3;
            continue;
          }
        }

        // Try NVDA-expanded: "6 colon 03" (space + "colon" + space + 2 digits)
        if (i + 9 <= text.size() &&
            text.substr(i, 7) == " colon " &&
            std::isdigit(static_cast<unsigned char>(text[i + 7])) &&
            std::isdigit(static_cast<unsigned char>(text[i + 8])) &&
            (i + 9 >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i + 9])))) {
          int hour = std::atoi(text.substr(numStart, hourLen).c_str());
          int min  = (text[i + 7] - '0') * 10 + (text[i + 8] - '0');
          if (hour <= 23 && min <= 59) {
            std::string hourStr = text.substr(numStart, hourLen);
            result += expandTimeCore(hourStr, min, text[i + 7], text[i + 8], ohDigit);
            TPREP("  timeExpand(nvda): \"%s\" -> ...\n",
                  text.substr(numStart, (i + 9) - numStart).c_str());
            i += 9;
            continue;
          }
        }
      }

      result += text.substr(numStart, i - numStart);
      continue;
    }
    result += text[i];
    ++i;
  }
  return result;
}

// ── Hyphenated number separation ──────────────────────────────────────────
//
// Separate digit-hyphen-digit so year splitting can process both halves.
// Does NOT insert a connector word — the platform's punctuation
// announcement handles the dash/hyphen.  Avoids ambiguity in math
// ("5-3") or scores where "to" would mislead.
//
// "2024-2025"      -> "2024 2025"      (raw — Android/iOS/SAPI)
// "2024 dash-2025" -> "2024 dash 2025" (NVDA — just unstick the number)

std::string separateHyphenatedNumbers(const std::string& text) {
  std::string result;
  result.reserve(text.size() + 16);
  for (size_t i = 0; i < text.size(); ++i) {
    // Raw hyphen between digits: "2024-2025" -> "2024 2025"
    if (text[i] == '-' && i > 0 && i + 1 < text.size() &&
        std::isdigit(static_cast<unsigned char>(text[i - 1])) &&
        std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
      result += ' ';
      TPREP("  hyphenSep(raw): at %zu\n", i);
      continue;
    }
    // NVDA-expanded: "dash-2025" — unstick the digit from "dash-"
    // so "2024 dash-2025" -> "2024 dash 2025" and year splitting
    // can process "2025" as a standalone token.
    if (text[i] == '-' && i >= 4 &&
        text[i - 4] == 'd' && text[i - 3] == 'a' &&
        text[i - 2] == 's' && text[i - 1] == 'h' &&
        i + 1 < text.size() &&
        std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
      result += ' ';
      TPREP("  hyphenSep(nvda): at %zu\n", i);
      continue;
    }
    result += text[i];
  }
  return result;
}

// ── Year splitting ────────────────────────────────────────────────────────
//
// Split 4-digit numbers into two 2-digit pairs so eSpeak reads them
// as digit pairs: "1995" -> "19 95" ("nineteen ninety-five").
// Only pure 4-digit tokens (no leading zeros, not part of a larger number).

std::string splitYears(const std::string& text, const std::string& ohDigit) {
  struct Tok { std::string s; bool isWs; };
  std::vector<Tok> toks;
  size_t i = 0;
  while (i < text.size()) {
    size_t start = i;
    bool ws = (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r');
    if (ws) {
      while (i < text.size() && (text[i] == ' ' || text[i] == '\t' ||
             text[i] == '\n' || text[i] == '\r')) ++i;
    } else {
      while (i < text.size() && text[i] != ' ' && text[i] != '\t' &&
             text[i] != '\n' && text[i] != '\r') ++i;
    }
    toks.push_back({text.substr(start, i - start), ws});
  }

  bool changed = false;
  for (auto& tok : toks) {
    if (tok.isWs) continue;

    // Strip trailing punctuation to find the numeric core.
    const std::string& s = tok.s;
    size_t numEnd = s.size();
    while (numEnd > 0 && std::ispunct(static_cast<unsigned char>(s[numEnd - 1])) &&
           !std::isdigit(static_cast<unsigned char>(s[numEnd - 1]))) --numEnd;

    // Strip leading punctuation too.
    size_t numStart = 0;
    while (numStart < numEnd && std::ispunct(static_cast<unsigned char>(s[numStart])) &&
           !std::isdigit(static_cast<unsigned char>(s[numStart]))) ++numStart;

    size_t numLen = numEnd - numStart;
    if (numLen != 4) continue;

    // Must be exactly 4 digits.
    bool allDigits = true;
    for (size_t c = numStart; c < numEnd; ++c) {
      if (!std::isdigit(static_cast<unsigned char>(s[c]))) { allDigits = false; break; }
    }
    if (!allDigits) continue;

    // Don't split if first pair starts with 0 (e.g. "0512").
    if (s[numStart] == '0') continue;

    // Don't split when the second pair is "00" — "4000" should be
    // "four thousand", not "forty oh zero".
    if (s[numStart + 2] == '0' && s[numStart + 3] == '0') continue;

    // Don't split "20XX" when XX is 01-09 — eSpeak says "two thousand one"
    // which is more natural than "twenty oh one".  But DO split other
    // centuries: "1708" -> "17 oh eight", "3709" -> "37 oh nine".
    if (s[numStart] == '2' && s[numStart + 1] == '0' && s[numStart + 2] == '0') continue;

    // Split: "1995" -> "19 95", "3709" -> "37 oh nine"
    // When the second pair has a leading zero (e.g. "09") and the
    // language pack provides an ohDigit word, use it instead of
    // letting eSpeak read "zero" — matches natural English year/number
    // pronunciation (Eloquence-style).  Non-English packs leave ohDigit
    // empty to skip this.
    std::string secondPair;
    if (s[numStart + 2] == '0' && !ohDigit.empty()) {
      secondPair = ohDigit + " " + s.substr(numStart + 3, 1);
    } else {
      secondPair = s.substr(numStart + 2, 2);
    }
    std::string split = s.substr(0, numStart)
        + s.substr(numStart, 2) + " " + secondPair
        + s.substr(numEnd);
    TPREP("  yearSplit: \"%s\" -> \"%s\"\n", s.c_str(), split.c_str());
    tok.s = split;
    changed = true;
  }

  if (!changed) return text;

  std::string result;
  result.reserve(text.size() + 32);
  for (const auto& tok : toks) result += tok.s;
  return result;
}

}  // namespace nvsp_frontend
