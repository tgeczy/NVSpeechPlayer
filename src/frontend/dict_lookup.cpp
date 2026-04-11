/*
TGSpeechBox — Pronunciation dictionary lookup implementation.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include "dict_lookup.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Debug logging (shares TPARSER_DEBUG_LOG toggle from text_parser.cpp).
#if 0
#include <cstdio>
#include <cstdlib>
static FILE* dlLogFile() {
  static FILE* f = nullptr;
  if (!f) {
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = std::getenv("TMP");
    if (!tmp) tmp = "/tmp";
    std::string path = std::string(tmp) + "/tgsb_dict_lookup.log";
    f = std::fopen(path.c_str(), "a");
  }
  return f;
}
#define DLLOG(...) do { FILE* _f = dlLogFile(); if (_f) { std::fprintf(_f, __VA_ARGS__); std::fflush(_f); } } while(0)
#else
#define DLLOG(...) ((void)0)
#endif

namespace nvsp_frontend {

// Strip Unicode variation selectors (U+FE0E text, U+FE0F emoji) from a
// UTF-8 string.  iOS often appends U+FE0F when copying emoji, which
// prevents dictionary lookup from matching the bare codepoint.
// U+FE0E = EF B8 8E, U+FE0F = EF B8 8F in UTF-8.
static std::string stripVariationSelectors(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (i + 2 < s.size() &&
        static_cast<uint8_t>(s[i])   == 0xEF &&
        static_cast<uint8_t>(s[i+1]) == 0xB8 &&
        (static_cast<uint8_t>(s[i+2]) == 0x8E ||
         static_cast<uint8_t>(s[i+2]) == 0x8F)) {
      i += 2;  // skip 3-byte sequence
      continue;
    }
    out.push_back(s[i]);
  }
  return out;
}

// Return the byte length of a Unicode punctuation codepoint at position i,
// or 0 if the byte at i is not the start of one.  Covers ASCII punctuation
// plus common multi-byte quotation marks and Spanish inverted punctuation.
static size_t unicodePunctLen(const std::string& s, size_t i) {
  auto b = static_cast<uint8_t>(s[i]);

  // ASCII punctuation — single byte.
  if (b < 0x80 && std::ispunct(b)) return 1;

  // 2-byte: C2 xx
  if (b == 0xC2 && i + 1 < s.size()) {
    auto b1 = static_cast<uint8_t>(s[i + 1]);
    if (b1 == 0xA1 || b1 == 0xAB || b1 == 0xBB || b1 == 0xBF)
      return 2;  // ¡ « » ¿
  }

  // 3-byte: E2 80 xx
  if (b == 0xE2 && i + 2 < s.size() && static_cast<uint8_t>(s[i + 1]) == 0x80) {
    auto b2 = static_cast<uint8_t>(s[i + 2]);
    // U+2018–U+201F: quotation marks (98–9F)
    // U+2026: ellipsis (A6)
    // U+2013–U+2014: en/em dash (93–94)
    if ((b2 >= 0x93 && b2 <= 0x9F) || b2 == 0xA6)
      return 3;
  }

  return 0;
}

std::string dictReplaceInText(const std::string& text, const PronDict& dict,
    const std::vector<std::string>& suffixes,
    std::unordered_map<std::string, std::string>* ipaOverrides) {
  if (text.empty() || dict.entries.empty()) return text;

  std::string result;
  result.reserve(text.size() + 32);

  size_t i = 0;
  while (i < text.size()) {
    // Skip whitespace.
    if (text[i] == ' ' || text[i] == '\t' ||
        text[i] == '\n' || text[i] == '\r') {
      result.push_back(text[i]);
      ++i;
      continue;
    }

    // Extract word token.
    size_t wordStart = i;
    while (i < text.size() && text[i] != ' ' && text[i] != '\t' &&
           text[i] != '\n' && text[i] != '\r') {
      ++i;
    }
    std::string token = text.substr(wordStart, i - wordStart);
    DLLOG("  token: \"%s\" (len=%zu)\n", token.c_str(), token.size());

    // Strip leading/trailing punctuation for lookup.
    // Uses Unicode-aware check so curly quotes, inverted punctuation,
    // en/em dashes, and ellipsis are stripped alongside ASCII punctuation.
    size_t lo = 0;
    while (lo < token.size()) {
      size_t plen = unicodePunctLen(token, lo);
      if (!plen) break;
      lo += plen;
    }
    size_t hi = token.size();
    while (hi > lo) {
      // Scan backwards: find the start of the last UTF-8 character.
      size_t back = hi - 1;
      while (back > lo && (static_cast<uint8_t>(token[back]) & 0xC0) == 0x80)
        --back;
      size_t plen = unicodePunctLen(token, back);
      if (!plen || back + plen != hi) break;
      hi = back;
    }

    DLLOG("    lo=%zu hi=%zu core=\"%s\"\n", lo, hi,
          std::string(token, lo, hi - lo).c_str());

    if (lo >= hi) {
      result += token;
      continue;
    }

    // Case-sensitive lookup: try exact case first (e.g., "Parton" as proper
    // noun), then fallback to lowercase (e.g., "parton" as common word).
    // This lets users have different pronunciations for capitalized vs
    // lowercase variants of the same word.
    // Strip variation selectors so "🦯\uFE0F" matches dict key "🦯".
    std::string exactKey = stripVariationSelectors(
        std::string(token, lo, hi - lo));
    std::string lowerKey;
    lowerKey.reserve(hi - lo);
    for (size_t k = lo; k < hi; ++k)
      lowerKey.push_back(static_cast<char>(
          std::tolower(static_cast<unsigned char>(token[k]))));

    auto it = dict.entries.find(exactKey);
    if (it == dict.entries.end() || it->second.masked) {
      // Fallback to lowercase.
      it = dict.entries.find(lowerKey);
      if (it == dict.entries.end() || it->second.masked) {
        std::string core(token, lo, hi - lo);

        // Suffix stripping: try removing each suffix (longest-first) and
        // look up the stem.  "launching" → strip "ing" → "launch" → match.
        bool suffixHandled = false;
        if (!suffixes.empty()) {
          for (const auto& sfx : suffixes) {
            if (lowerKey.size() <= sfx.size()) continue;
            size_t stemLen = lowerKey.size() - sfx.size();
            if (stemLen < 3) continue;
            if (lowerKey.compare(stemLen, sfx.size(), sfx) != 0) continue;

            std::string stemExact = stripVariationSelectors(
                core.substr(0, stemLen));
            std::string stemLower = lowerKey.substr(0, stemLen);

            auto sit = dict.entries.find(stemExact);
            if (sit == dict.entries.end() || sit->second.masked)
              sit = dict.entries.find(stemLower);
            if (sit == dict.entries.end() || sit->second.masked) continue;

            // For suffix-stripped matches, always use text respelling so
            // eSpeak phonemizes the suffix naturally in context (e.g. "'s"
            // becomes /z/ not "ess").  IPA-only entries (no toText) are
            // skipped — they need explicit suffixed dict entries.
            if (sit->second.toText.empty()) continue;

            std::string origSuffix = core.substr(stemLen);
            std::string rep;
            rep.reserve(sit->second.toText.size() + origSuffix.size());
            for (char c : sit->second.toText)
              rep += (c == '-') ? ' ' : c;
            rep += origSuffix;
            DLLOG("  suffixReplace: \"%s\" -> \"%s\"\n",
                  core.c_str(), rep.c_str());
            result += token.substr(0, lo);
            result += rep;
            result += token.substr(hi);
            suffixHandled = true;
            break;
          }
        }
        if (suffixHandled) continue;

        // Token not found as a whole.  If it contains hyphens, split into
        // hyphen-separated parts and look up each part individually so that
        // "this-pentagon-official" still matches a dict entry for "pentagon"
        // even when the full hyphenated string has no entry.
        if (core.find('-') != std::string::npos) {
          DLLOG("  hyphen-split fallback for \"%s\"\n", core.c_str());
          std::string rebuilt;
          rebuilt.reserve(core.size() + 16);
          size_t pos = 0;
          while (pos < core.size()) {
            size_t dash = core.find('-', pos);
            if (dash == std::string::npos) dash = core.size();
            std::string part = core.substr(pos, dash - pos);
            if (!part.empty()) {
              std::string partLower;
              partLower.reserve(part.size());
              for (char c : part)
                partLower.push_back(static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c))));
              std::string partExact = stripVariationSelectors(part);
              auto pit = dict.entries.find(partExact);
              if (pit == dict.entries.end() || pit->second.masked)
                pit = dict.entries.find(partLower);
              if (pit != dict.entries.end() && !pit->second.masked) {
                if (ipaOverrides && !pit->second.toIpa.empty()) {
                  (*ipaOverrides)[partLower] = pit->second.toIpa;
                  rebuilt += part;
                } else {
                  std::string rep;
                  for (char c : pit->second.toText)
                    rep += (c == '-') ? ' ' : c;
                  DLLOG("    part \"%s\" -> \"%s\"\n", part.c_str(), rep.c_str());
                  rebuilt += rep;
                }
              } else {
                rebuilt += part;
              }
            }
            if (dash < core.size()) {
              rebuilt += '-';
              pos = dash + 1;
            } else {
              pos = dash;
            }
          }
          result += token.substr(0, lo);
          result += rebuilt;
          result += token.substr(hi);
        } else {
          result += token;
        }
        continue;
      }
    }

    // If this entry has a toIpa override, don't replace the text — leave
    // the original word so eSpeak phonemizes it naturally.  Record the
    // override for downstream IPA splicing in runTextParser.
    if (ipaOverrides && !it->second.toIpa.empty()) {
      DLLOG("  dictIpaOverride: \"%s\" -> ipa \"%s\"\n",
            lowerKey.c_str(), it->second.toIpa.c_str());
      (*ipaOverrides)[lowerKey] = it->second.toIpa;
      result += token;  // keep original word
      continue;
    }

    // Convert hyphens to spaces in toText — users write them as pronunciation
    // guides ("shi-kah-go") but eSpeak treats hyphens inconsistently as word
    // boundaries in isolation vs phrase context.  Converting to spaces makes
    // "shi-kah-go" and "shi kah go" produce identical results.
    std::string replacement;
    replacement.reserve(it->second.toText.size());
    for (char c : it->second.toText) {
      replacement += (c == '-') ? ' ' : c;
    }

    DLLOG("  dictReplace: \"%s\" -> \"%s\"\n",
          it->second.fromText.c_str(), replacement.c_str());

    // Replace core with cleaned toText, preserving surrounding punctuation.
    result += token.substr(0, lo);
    result += replacement;
    result += token.substr(hi);
  }

  return result;
}

}  // namespace nvsp_frontend
