#include "ipa_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <sstream>

namespace nvsp_frontend {

static inline bool hasFlag(const PhonemeDef* def, std::uint32_t bit) {
  return def && ((def->flags & bit) != 0);
}

static inline bool tokenIsVowel(const Token& t) {
  return t.def && ((t.def->flags & kIsVowel) != 0);
}

static inline bool tokenIsVoiced(const Token& t) {
  return t.def && ((t.def->flags & kIsVoiced) != 0);
}

static inline bool tokenIsStop(const Token& t) {
  return t.def && ((t.def->flags & kIsStop) != 0);
}

static inline bool tokenIsAfricate(const Token& t) {
  return t.def && ((t.def->flags & kIsAfricate) != 0);
}

static inline bool tokenIsTap(const Token& t) {
  return t.def && ((t.def->flags & kIsTap) != 0);
}

static inline bool tokenIsTrill(const Token& t) {
  return t.def && ((t.def->flags & kIsTrill) != 0);
}

static inline bool tokenIsLiquid(const Token& t) {
  return t.def && ((t.def->flags & kIsLiquid) != 0);
}

static inline bool tokenIsSemivowel(const Token& t) {
  return t.def && ((t.def->flags & kIsSemivowel) != 0);
}

static inline bool tokenIsNasal(const Token& t) {
  return t.def && ((t.def->flags & kIsNasal) != 0);
}

static inline double getFieldOrZero(const Token& t, FieldId id) {
  int idx = static_cast<int>(id);
  if ((t.setMask & (1ull << idx)) == 0) return 0.0;
  return t.field[idx];
}

static inline bool tokenIsFricativeLike(const Token& t) {
  // Mirrors ipa_convert.py: fricationAmplitude > 0.05
  return getFieldOrZero(t, FieldId::fricationAmplitude) > 0.05;
}

static inline const PhonemeDef* findPhoneme(const PackSet& pack, const std::u32string& key) {
  auto it = pack.phonemes.find(key);
  if (it == pack.phonemes.end()) return nullptr;
  return &it->second;
}

static inline bool isToneLetter(char32_t c) {
  // Chao tone letters: ˥ ˦ ˧ ˨ ˩ (U+02E5..U+02E9)
  return c >= 0x02E5 && c <= 0x02E9;
}

static inline bool isAsciiDigit(char32_t c) {
  return c >= U'0' && c <= U'9';
}

static inline bool isSpace(char32_t c) {
  return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r';
}

static inline bool isStressMark(char32_t c) {
  return c == U'ˈ' || c == U'ˌ';
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

static inline bool isTieBar(char32_t c) {
  return c == U'͡' || c == U'͜';
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

static void applyRules(std::u32string& text, const PackSet& pack, const std::vector<ReplacementRule>& rules) {
  const bool textHasTie = (text.find(U'͡') != std::u32string::npos) || (text.find(U'͜') != std::u32string::npos);

  for (const auto& rule : rules) {
    if (rule.from.empty()) continue;

    const bool patHasTie = (rule.from.find(U'͡') != std::u32string::npos) || (rule.from.find(U'͜') != std::u32string::npos);
    const bool useLooseTie = (rule.from.size() > 1) && (textHasTie || patHasTie);

    // Fast skip: only safe when we can rely on direct substring search.
    // If tie bars are involved, a pattern like "a͡ɪ" should also match "aɪ", so
    // we can't skip purely on text.find(rule.from).
    if (!useLooseTie) {
      if (text.find(rule.from) == std::u32string::npos) {
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

    size_t i = 0;
    while (i < text.size()) {
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

        if (ok) {
          out.append(to);
          i = matchEnd;
          continue;
        }
      }

      out.push_back(text[i]);
      ++i;
    }

    text.swap(out);
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

static std::u32string normalizeIpaText(const PackSet& pack, const std::string& ipaUtf8) {
  std::u32string t = utf8ToU32(ipaUtf8);

  // Normalize tie bar variants early so pack rules can match reliably.
  replaceAll(t, U"͜", U"͡");

  // 1) Pack pre-replacements (lets you preserve info before we strip chars like '-').
  applyRules(t, pack, pack.lang.preReplacements);

  // 2) Basic cleanup, mirroring ipa_convert.py defaults.
  // Remove ZWJ/ZWNJ.
  replaceAll(t, U"‍", U"");
  replaceAll(t, U"‌", U"");

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

  // Pause/separators.
  replaceAll(t, U"_:", U" ");
  replaceAll(t, U"_", U" ");

  if (pack.lang.stripHyphen) {
    replaceAll(t, U"-", U"");
  }

  // Stress/length markers.
  replaceAll(t, U"'", U"ˈ");
  replaceAll(t, U",", U"ˌ");
  replaceAll(t, U":", U"ː");
  // --- IPA normalisation / fallbacks (match legacy ipa_bestversion.py) ---
  // eSpeak's espeak_TextToPhonemes() IPA mode frequently uses tied sequences
  // to represent syllabic /-l/ endings (e.g. "level" -> ...ə͡l, "cancel" -> ...ə͡l).
  // If we treat these as a single phoneme key, the /l/ can disappear entirely.
  // The legacy Python pipeline normalized these into schwa + l.
  replaceAll(t, U"l̩", U"əl");
  replaceAll(t, U"ɫ̩", U"əl");
  replaceAll(t, U"ə͡l", U"əl");
  replaceAll(t, U"ʊ͡l", U"əl");
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

  collapseWhitespace(t);
  return t;
}

static void correctCopyAdjacent(std::vector<Token>& tokens) {
  const int n = static_cast<int>(tokens.size());
  for (int i = 0; i < n; ++i) {
    Token& cur = tokens[i];
    if (!cur.def) continue;
    if ((cur.def->flags & kCopyAdjacent) == 0) continue;

    // Find adjacent real phoneme.
    const Token* adjacent = nullptr;
    for (int j = i + 1; j < n; ++j) {
      if (tokens[j].def && !tokens[j].silence) {
        adjacent = &tokens[j];
        break;
      }
    }
    if (!adjacent) {
      for (int j = i - 1; j >= 0; --j) {
        if (tokens[j].def && !tokens[j].silence) {
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
      if (syllableStress == 1) {
        curSpeed = baseSpeed / lang.primaryStressDiv;
      } else if (syllableStress == 2) {
        curSpeed = baseSpeed / lang.secondaryStressDiv;
      } else {
        curSpeed = baseSpeed;
      }
    }

    double dur = 60.0 / curSpeed;
    double fade = 10.0 / curSpeed;

    if (t.vowelHiatusGap) {
      dur = lang.stressedVowelHiatusGapMs / baseSpeed;
      fade = lang.stressedVowelHiatusFadeMs / baseSpeed;
    } else if (t.preStopGap) {
      if (t.clusterGap) {
        double baseDur = lang.stopClosureClusterGapMs;
        double baseFade = lang.stopClosureClusterFadeMs;

        // Optional: allow a larger cluster gap at word boundaries.
        if (t.wordStart && lang.stopClosureWordBoundaryClusterGapMs > 0.0) {
          baseDur = lang.stopClosureWordBoundaryClusterGapMs;
        }
        if (t.wordStart && lang.stopClosureWordBoundaryClusterFadeMs > 0.0) {
          baseFade = lang.stopClosureWordBoundaryClusterFadeMs;
        }

        dur = baseDur / curSpeed;
        fade = baseFade / curSpeed;
      } else {
        dur = lang.stopClosureVowelGapMs / curSpeed;
        fade = lang.stopClosureVowelFadeMs / curSpeed;
      }
    } else if (t.postStopAspiration) {
      dur = 20.0 / curSpeed;
    } else if (tokenIsTap(t) || tokenIsTrill(t)) {
      if (tokenIsTrill(t)) {
        dur = 22.0 / curSpeed;
      } else {
        dur = std::min(14.0 / curSpeed, 14.0);
      }
      fade = 0.001;
    } else if (tokenIsStop(t)) {
      dur = std::min(6.0 / curSpeed, 6.0);
      fade = 0.001;
    } else if (tokenIsAfricate(t)) {
      dur = 24.0 / curSpeed;
      fade = 0.001;
    } else if (!tokenIsVoiced(t)) {
      dur = 45.0 / curSpeed;
    } else {
      if (tokenIsVowel(t)) {
        if (last && (tokenIsLiquid(*last) || tokenIsSemivowel(*last))) {
          fade = 25.0 / curSpeed;
        }

        if (t.tiedTo) {
          dur = 40.0 / curSpeed;
        } else if (t.tiedFrom) {
          dur = 20.0 / curSpeed;
          fade = 20.0 / curSpeed;
        } else if (!syllableStress && !t.syllableStart && next && !next->wordStart && (tokenIsLiquid(*next) || tokenIsNasal(*next))) {
          if (tokenIsLiquid(*next)) {
            dur = 30.0 / curSpeed;
          } else {
            dur = 40.0 / curSpeed;
          }
        }
      } else {
        dur = 30.0 / curSpeed;
        if (tokenIsLiquid(t) || tokenIsSemivowel(t)) {
          fade = 20.0 / curSpeed;
        }
      }
    }

    // Optional: semivowel offglide shortening.
    //
    // Some packs render diphthongs as vowel+semivowel sequences (e.g. eɪ -> ej).
    // When that semivowel is followed by a vowel or liquid-like consonant within
    // the same word, giving it a full consonant duration can sound like an
    // unintended micro-break (e.g. "player", "later").
    if (lang.semivowelOffglideScale != 1.0 && tokenIsSemivowel(t)) {
      double s = lang.semivowelOffglideScale;
      if (s <= 0.0) s = 1.0;
      // Keep this bounded to avoid pathological configs.
      if (s < 0.05) s = 0.05;
      if (s > 3.0) s = 3.0;

      const bool prevIsVowel = (last && !last->silence && tokenIsVowel(*last));
      const bool nextOk = (next && !next->silence && !next->wordStart &&
                           (tokenIsVowel(*next) || tokenIsLiquid(*next) || tokenIsTap(*next) || tokenIsTrill(*next)));

      if (prevIsVowel && nextOk) {
        dur *= s;
        fade *= s;
        // Avoid zero/negative durations.
        dur = std::max(dur, 1.0 / curSpeed);
        fade = std::max(fade, 0.001);
        if (fade > dur) fade = dur;
      }
    }

    // Hungarian short vowel tweak (defaults to enabled, safe to disable).
    if (lang.huShortAVowelEnabled && tokenIsVowel(t) && !t.lengthened && t.baseChar != 0) {
      if (t.baseChar == (lang.huShortAVowelKey.empty() ? U'\0' : lang.huShortAVowelKey[0])) {
        dur *= lang.huShortAVowelScale;
      }
    }

    // English word-final long /uː/ shortening.
    if (lang.englishLongUShortenEnabled && tokenIsVowel(t) && t.lengthened && t.baseChar != 0) {
      if (t.baseChar == (lang.englishLongUKey.empty() ? U'\0' : lang.englishLongUKey[0])) {
        if (!next || next->wordStart) {
          dur *= lang.englishLongUWordFinalScale;
          fade = std::min(fade, 14.0 / curSpeed);
        }
      }
    }

    // Lengthened scaling.
    if (t.lengthened) {
      if (!lang.applyLengthenedScaleToVowelsOnly || tokenIsVowel(t)) {
        const bool isHu = (lang.langTag.rfind("hu", 0) == 0);
        dur *= (isHu ? lang.lengthenedScaleHu : lang.lengthenedScale);
      }
    }

    // Optional: additional shortening for lengthened vowels (ː) in a final
    // closed syllable (vowel + word-final consonant(s)).
    //
    // This is intentionally conservative: we only apply it when there are no
    // later vowels before the next word boundary, which avoids false positives
    // in words where a consonant cluster is actually the onset of the next
    // syllable (e.g. "apricot" /ˈeɪprɪ.../).
    if (lang.lengthenedVowelFinalCodaScale != 1.0 && t.lengthened && tokenIsVowel(t)) {
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
    t.fadeMs = fade;
    last = &t;
  }
}

static double pitchFromPercent(double basePitch, double inflection, double percent) {
  // Matches ipa_convert.py:
  // pitch = basePitch * 2 ** (((percent-50)/50) * inflection)
  const double exp = (((percent - 50.0) / 50.0) * inflection);
  return basePitch * std::pow(2.0, exp);
}

static double percentFromPitch(double basePitch, double inflection, double pitch) {
  if (basePitch <= 0.0) return 50.0;
  if (inflection == 0.0) return 50.0;
  const double ratio = pitch / basePitch;
  if (ratio <= 0.0) return 50.0;
  const double exp = std::log2(ratio);
  return 50.0 + (50.0 * exp / inflection);
}

static void setPitchFields(Token& t, double startPitch, double endPitch) {
  const int vp = static_cast<int>(FieldId::voicePitch);
  const int evp = static_cast<int>(FieldId::endVoicePitch);
  t.field[vp] = startPitch;
  t.field[evp] = endPitch;
  t.setMask |= (1ull << vp);
  t.setMask |= (1ull << evp);
}

static void applyPitchPath(std::vector<Token>& tokens, int startIndex, int endIndex,
                           double basePitch, double inflection, int startPct, int endPct) {
  if (startIndex >= endIndex) return;

  const double startPitch = pitchFromPercent(basePitch, inflection, startPct);
  const double endPitch = pitchFromPercent(basePitch, inflection, endPct);

  double voicedDuration = 0.0;
  for (int i = startIndex; i < endIndex; ++i) {
    if (tokenIsVoiced(tokens[i])) voicedDuration += tokens[i].durationMs;
  }

  if (voicedDuration <= 0.0) {
    for (int i = startIndex; i < endIndex; ++i) {
      setPitchFields(tokens[i], startPitch, startPitch);
    }
    return;
  }

  double curDuration = 0.0;
  const double delta = endPitch - startPitch;
  double curPitch = startPitch;

  for (int i = startIndex; i < endIndex; ++i) {
    Token& t = tokens[i];
    const double start = curPitch;

    if (tokenIsVoiced(t)) {
      curDuration += t.durationMs;
      const double ratio = curDuration / voicedDuration;
      curPitch = startPitch + (delta * ratio);
    }

    setPitchFields(t, start, curPitch);
  }
}

static IntonationClause defaultClause(char clause) {
  // Defaults from ipa_convert.py table.
  IntonationClause c;
  if (clause == '.') {
    c.preHeadStart = 46;
    c.preHeadEnd = 57;
    c.headExtendFrom = 4;
    c.headStart = 80;
    c.headEnd = 50;
    c.headSteps = {100,75,50,25,0,63,38,13,0};
    c.headStressEndDelta = -16;
    c.headUnstressedRunStartDelta = -8;
    c.headUnstressedRunEndDelta = -5;
    c.nucleus0Start = 64;
    c.nucleus0End = 8;
    c.nucleusStart = 70;
    c.nucleusEnd = 18;
    c.tailStart = 24;
    c.tailEnd = 8;
  } else if (clause == ',') {
    c.preHeadStart = 46;
    c.preHeadEnd = 57;
    c.headExtendFrom = 4;
    c.headStart = 80;
    c.headEnd = 60;
    c.headSteps = {100,75,50,25,0,63,38,13,0};
    c.headStressEndDelta = -16;
    c.headUnstressedRunStartDelta = -8;
    c.headUnstressedRunEndDelta = -5;
    c.nucleus0Start = 34;
    c.nucleus0End = 52;
    c.nucleusStart = 78;
    c.nucleusEnd = 34;
    c.tailStart = 34;
    c.tailEnd = 52;
  } else if (clause == '?') {
    c.preHeadStart = 45;
    c.preHeadEnd = 56;
    c.headExtendFrom = 3;
    c.headStart = 75;
    c.headEnd = 43;
    c.headSteps = {100,75,50,20,60,35,11,0};
    c.headStressEndDelta = -16;
    c.headUnstressedRunStartDelta = -7;
    c.headUnstressedRunEndDelta = 0;
    c.nucleus0Start = 34;
    c.nucleus0End = 68;
    c.nucleusStart = 86;
    c.nucleusEnd = 21;
    c.tailStart = 34;
    c.tailEnd = 68;
  } else if (clause == '!') {
    c.preHeadStart = 46;
    c.preHeadEnd = 57;
    c.headExtendFrom = 3;
    c.headStart = 90;
    c.headEnd = 50;
    c.headSteps = {100,75,50,16,82,50,32,16};
    c.headStressEndDelta = -16;
    c.headUnstressedRunStartDelta = -9;
    c.headUnstressedRunEndDelta = 0;
    c.nucleus0Start = 92;
    c.nucleus0End = 4;
    c.nucleusStart = 92;
    c.nucleusEnd = 80;
    c.tailStart = 76;
    c.tailEnd = 4;
  } else {
    return defaultClause('.');
  }
  return c;
}

static const IntonationClause& getClauseParams(const LanguagePack& lang, char clause, IntonationClause& storage) {
  auto it = lang.intonation.find(clause);
  if (it != lang.intonation.end()) return it->second;
  storage = defaultClause(clause);
  return storage;
}



static void calculatePitchesLegacy(std::vector<Token>& tokens, const PackSet& pack,
                                  double speed, double basePitch, double inflection, char clauseType) {
  // Port of ipa-older.py calculatePhonemePitches().
  //
  // This is intentionally time-based (uses accumulated voiced duration) rather than
  // table-based, and tends to produce a more predictable "classic" screen reader
  // prosody at higher rates.

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

  double totalVoicedDuration = 0.0;
  double finalInflectionStartTime = 0.0;
  bool needsSetFinalInflectionStartTime = false;
  int finalVoicedIndex = -1;

  const Token* last = nullptr;
  for (size_t i = 0; i < tokens.size(); ++i) {
    const Token& t = tokens[i];

    if (t.wordStart) {
      needsSetFinalInflectionStartTime = true;
    }

    if (tokenIsVoiced(t)) {
      finalVoicedIndex = static_cast<int>(i);
      if (needsSetFinalInflectionStartTime) {
        finalInflectionStartTime = totalVoicedDuration;
        needsSetFinalInflectionStartTime = false;
      }
      totalVoicedDuration += t.durationMs;
    } else if (last && tokenIsVoiced(*last)) {
      // When we leave a voiced segment, count the fade time as part of the voiced run.
      totalVoicedDuration += last->fadeMs;
    }

    last = &t;
  }

  if (totalVoicedDuration <= 0.0) {
    // No voiced frames: set a constant pitch so downstream code has sane values.
    for (Token& t : tokens) {
      setPitchFields(t, basePitch, basePitch);
    }
    return;
  }

  double durationCounter = 0.0;
  double curBasePitch = basePitch;
  double lastEndVoicePitch = basePitch;
  double stressInflection = infl / 1.5;

  Token* lastToken = nullptr;
  bool syllableStress = false;
  bool firstStress = true;

  for (size_t i = 0; i < tokens.size(); ++i) {
    Token& t = tokens[i];

    if (t.syllableStart) {
      syllableStress = (t.stress == 1);
    }

    double voicePitch = lastEndVoicePitch;
    const bool inFinalInflection = (durationCounter >= finalInflectionStartTime);

    // Advance the duration counter.
    if (tokenIsVoiced(t)) {
      durationCounter += t.durationMs;
    } else if (lastToken && tokenIsVoiced(*lastToken)) {
      durationCounter += lastToken->fadeMs;
    }

    const double oldBasePitch = curBasePitch;

    if (infl == 0.0) {
      curBasePitch = basePitch;
    } else if (!inFinalInflection) {
      // Gentle declination across the clause.
      curBasePitch = basePitch / (1.0 + (infl / 25000.0) * durationCounter * speed);
    } else {
      // Final inflection is shaped only over the last word.
      const double denom = (totalVoicedDuration - finalInflectionStartTime);
      double ratio = 0.0;
      if (denom > 0.0) {
        ratio = (durationCounter - finalInflectionStartTime) / denom;
      }

      if (clauseType == '.') {
        ratio /= 1.5;
      } else if (clauseType == '?') {
        ratio = 0.5 - (ratio / 1.2);
      } else if (clauseType == ',') {
        ratio /= 8.0;
      } else {
        ratio = ratio / 1.75;
      }

      curBasePitch = basePitch / (1.0 + (infl * ratio * 1.5));
    }

    double endVoicePitch = curBasePitch;

    // Add a pitch accent on the vowel in the stressed syllable.
    if (syllableStress && tokenIsVowel(t)) {
      if (firstStress) {
        voicePitch = oldBasePitch * (1.0 + stressInflection / 3.0);
        endVoicePitch = curBasePitch * (1.0 + stressInflection);
        firstStress = false;
      } else if (static_cast<int>(i) < finalVoicedIndex) {
        voicePitch = oldBasePitch * (1.0 + stressInflection / 3.0);
        endVoicePitch = oldBasePitch * (1.0 + stressInflection);
      } else {
        voicePitch = basePitch * (1.0 + stressInflection);
      }

      stressInflection *= 0.9;
      stressInflection = std::max(stressInflection, infl / 2.0);
      syllableStress = false;
    }

    // Match the legacy behavior: ensure pitch continuity by snapping the previous
    // token's end pitch to this token's start pitch (useful when accents start).
    if (lastToken) {
      const int evp = static_cast<int>(FieldId::endVoicePitch);
      lastToken->field[evp] = voicePitch;
      lastToken->setMask |= (1ull << evp);
    }

    setPitchFields(t, voicePitch, endVoicePitch);
    lastEndVoicePitch = endVoicePitch;
    lastToken = &t;
  }
}

static void calculatePitches(std::vector<Token>& tokens, const PackSet& pack, double speed, double basePitch, double inflection, char clauseType) {
  if (pack.lang.legacyPitchMode) {
    calculatePitchesLegacy(tokens, pack, speed, basePitch, inflection, clauseType);
    return;
  }

  IntonationClause tmp;
  const IntonationClause& params = getClauseParams(pack.lang, clauseType, tmp);

  int preHeadStart = 0;
  int preHeadEnd = static_cast<int>(tokens.size());

  // Find first stressed syllable.
  for (int i = 0; i < preHeadEnd; ++i) {
    if (tokens[i].syllableStart && tokens[i].stress == 1) {
      preHeadEnd = i;
      break;
    }
  }

  if (preHeadEnd - preHeadStart > 0) {
    applyPitchPath(tokens, preHeadStart, preHeadEnd, basePitch, inflection, params.preHeadStart, params.preHeadEnd);
  }

  int nucleusStart = static_cast<int>(tokens.size());
  int nucleusEnd = nucleusStart;
  int tailStart = nucleusStart;
  int tailEnd = nucleusStart;

  // Find last stressed syllable, scanning backwards.
  for (int i = nucleusEnd - 1; i >= preHeadEnd; --i) {
    if (tokens[i].syllableStart) {
      if (tokens[i].stress == 1) {
        nucleusStart = i;
        break;
      } else {
        nucleusEnd = i;
        tailStart = i;
      }
    }
  }

  const bool hasTail = (tailEnd - tailStart) > 0;
  if (hasTail) {
    applyPitchPath(tokens, tailStart, tailEnd, basePitch, inflection, params.tailStart, params.tailEnd);
  }

  if (nucleusEnd - nucleusStart > 0) {
    if (hasTail) {
      applyPitchPath(tokens, nucleusStart, nucleusEnd, basePitch, inflection, params.nucleusStart, params.nucleusEnd);
    } else {
      applyPitchPath(tokens, nucleusStart, nucleusEnd, basePitch, inflection, params.nucleus0Start, params.nucleus0End);
    }
  }

  // Head section (between preHeadEnd and nucleusStart).
  if (preHeadEnd < nucleusStart) {
    int headStartPitch = params.headStart;
    int headEndPitch = params.headEnd;

    int lastHeadStressStart = -1;
    int lastHeadUnstressedRunStart = -1;
    int stressEndPitch = headEndPitch;

    const std::vector<int>& steps = params.headSteps.empty() ? std::vector<int>{100,75,50,25,0} : params.headSteps;
    const int extendFrom = std::min(std::max(params.headExtendFrom, 0), static_cast<int>(steps.size()));

    int stepIndex = 0;
    auto nextStep = [&]() -> int {
      if (stepIndex < static_cast<int>(steps.size())) {
        return steps[stepIndex++];
      }
      // cycle
      const int cycleLen = static_cast<int>(steps.size()) - extendFrom;
      if (cycleLen <= 0) return steps.back();
      const int idx = extendFrom + ((stepIndex++ - static_cast<int>(steps.size())) % cycleLen);
      return steps[idx];
    };

    for (int i = preHeadEnd; i <= nucleusStart; ++i) {
      Token& t = tokens[i];
      const bool syllableStress = (t.stress == 1);
      if (t.syllableStart) {
        if (lastHeadStressStart >= 0) {
          const int stepPct = nextStep();
          const int stressStartPitch = headEndPitch + static_cast<int>(((headStartPitch - headEndPitch) / 100.0) * stepPct);
          stressEndPitch = stressStartPitch + params.headStressEndDelta;
          applyPitchPath(tokens, lastHeadStressStart, i, basePitch, inflection, stressStartPitch, stressEndPitch);
          lastHeadStressStart = -1;
        }

        if (syllableStress) {
          if (lastHeadUnstressedRunStart >= 0) {
            const int runStartPitch = stressEndPitch + params.headUnstressedRunStartDelta;
            const int runEndPitch = stressEndPitch + params.headUnstressedRunEndDelta;
            applyPitchPath(tokens, lastHeadUnstressedRunStart, i, basePitch, inflection, runStartPitch, runEndPitch);
            lastHeadUnstressedRunStart = -1;
          }
          lastHeadStressStart = i;
        } else if (lastHeadUnstressedRunStart < 0) {
          lastHeadUnstressedRunStart = i;
        }
      }
    }
  }
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

static bool parseToTokens(const PackSet& pack, const std::u32string& text, std::vector<Token>& outTokens, std::string& outError) {
  const LanguagePack& lang = pack.lang;

  bool newWord = true;
  int pendingStress = 0;

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

  for (size_t i = 0; i < n; ++i) {
    const char32_t c = text[i];

    if (c == U' ') {
      newWord = true;
      continue;
    }

    // Primary/secondary stress.
    if (c == U'\u02C8') { // ˈ
      pendingStress = 1;
      continue;
    }
    if (c == U'\u02CC') { // ˌ
      pendingStress = 2;
      continue;
    }

    // Tone markers (only when tonal is enabled).
    if (lang.tonal) {
      if (isToneLetter(c)) {
        // Collect run of tone letters.
        std::u32string run;
        run.push_back(c);
        while (i + 1 < n && isToneLetter(text[i + 1])) {
          run.push_back(text[++i]);
        }
        attachToneStringToSyllable(run);
        continue;
      }
      if (lang.toneDigitsEnabled && (c >= U'1' && c <= U'5')) {
        attachToneToSyllable(c);
        continue;
      }
    }

    const bool isLengthened = (i + 1 < n && text[i + 1] == U'\u02D0'); // ː
    const bool isTiedTo = (i + 1 < n && text[i + 1] == U'\u0361');     // ͡
    const bool isTiedFrom = (i > 0 && text[i - 1] == U'\u0361');       // ͡

    const PhonemeDef* def = nullptr;
    bool tiedTo = false;
    bool lengthened = false;

    if (isTiedTo) {
      // Try combined key (char + tie + next char).
      if (i + 2 < n) {
        std::u32string k;
        k.push_back(text[i]);
        k.push_back(text[i + 1]);
        k.push_back(text[i + 2]);
        def = findPhoneme(pack, k);
        if (def) {
          // consume tie + next
          i += 2;
          tiedTo = true;
        } else {
          // consume only tie (leave the next char to be parsed separately)
          i += 1;
          tiedTo = true;
        }
      } else {
        // dangling tie bar, ignore it
        continue;
      }
    } else if (isLengthened) {
      std::u32string k;
      k.push_back(text[i]);
      k.push_back(text[i + 1]);
      def = findPhoneme(pack, k);
      if (def) {
        i += 1;
        lengthened = true;
      }
    }

    if (!def) {
      std::u32string k;
      k.push_back(c);
      def = findPhoneme(pack, k);
      if (!def) {
        // Unknown char: drop it (safe default).
        continue;
      }
    }

    Token t;
    t.def = def;
    t.setMask = def->setMask;
    for (int f = 0; f < kFrameFieldCount; ++f) {
      t.field[f] = def->field[f];
    }

    t.baseChar = c;
    t.tiedFrom = isTiedFrom;
    t.tiedTo = tiedTo;
    t.lengthened = (lengthened || isLengthened);

    const int stress = pendingStress;
    pendingStress = 0;

    // Helper to access last token safely by index.
    auto haveLast = [&]() -> bool {
      return lastIndex >= 0 && lastIndex < static_cast<int>(outTokens.size());
    };

    // Syllable start detection.
    if (haveLast()) {
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
    if (stress == 0 && (tokenIsStop(t) || tokenIsAfricate(t))) {
      bool needGap = false;
      bool clusterGap = false;

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
        }
      } else {
        // none
      }

      if (needGap) {
        Token gap;
        gap.silence = true;
        gap.preStopGap = true;
        gap.clusterGap = clusterGap;
        // Preserve word boundary information for timing tweaks.
        // The gap is inserted *before* the stop/affricate token `t`.
        gap.wordStart = t.wordStart;
        outTokens.push_back(gap);
        // IMPORTANT: do NOT update lastIndex here; Python keeps lastPhoneme as the
        // previous *real* phoneme, not the inserted gap.
      }
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

static inline bool isAutoDiphthongOffglideCandidate(char32_t c) {
  // Heuristic for “diphthong offglide” vowels.
  //
  // Many IPA sources (including eSpeak) represent diphthongs as two vowels.
  // Some languages (or some eSpeak outputs) omit an explicit tie-bar/non-syllabic
  // mark. When enabled via packs, we can treat certain vowel+vowel sequences as a
  // diphthong by marking them as tied (as if U+0361 were present).
  //
  // We keep this intentionally conservative: only “high” vowels which commonly
  // act as offglides are considered.
  switch (c) {
    case U'i':
    case U'ɪ': // ɪ
    case U'u':
    case U'ʊ': // ʊ
    case U'y':
    case U'ʏ': // ʏ
    case U'ɯ': // ɯ
    case U'ɨ': // ɨ
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
    case U'ɪ':
    case U'ɨ':
      target = U'j';
      break;
    case U'u':
    case U'ʊ':
      target = U'w';
      break;
    default:
      return nullptr;
  }

  std::u32string k;
  k.push_back(target);
  return findPhoneme(pack, k);
}

static void autoTieDiphthongs(const PackSet& pack, std::vector<Token>& tokens) {
  if (!pack.lang.autoTieDiphthongs) return;

  int prevReal = -1;
  const int n = static_cast<int>(tokens.size());
  for (int i = 0; i < n; ++i) {
    Token& cur = tokens[i];
    if (!cur.def || cur.silence) continue;

    if (prevReal >= 0) {
      Token& prev = tokens[prevReal];

      const bool prevVowelLike = tokenIsVowel(prev) || tokenIsSemivowel(prev);
      const bool curVowelLike = tokenIsVowel(cur) || tokenIsSemivowel(cur);

      // Only consider within-syllable vowel-like sequences.
      // If the current token starts a new syllable (explicit stress, word start,
      // etc.), treat it as hiatus instead.
      if (prevVowelLike && curVowelLike && !cur.wordStart && !cur.syllableStart) {
        // Skip if the IPA already encoded tying, or the vowel is explicitly long.
        if (!prev.tiedTo && !prev.tiedFrom && !cur.tiedTo && !cur.tiedFrom && !cur.lengthened) {
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

static void applySpellingDiphthongMode(const PackSet& pack, std::vector<Token>& tokens) {
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
            const bool isIshVowel = tokenIsVowel(off) && (off.baseChar == U'ɪ' || off.baseChar == U'i');
            if (isJ || isIshVowel) {
              // Only treat this as standalone /eɪ/ if the offglide is followed
              // by the next syllable (next letter) or the end of the word.
              size_t k = j + 1;
              while (k < wordEnd && (tokens[k].silence || !tokens[k].def)) ++k;

              if (k >= wordEnd || tokens[k].syllableStart) {
                // Monophthongize: keep the /e/ nucleus, drop the offglide.
                // Mark the nucleus as lengthened to preserve a letter-name feel.
                t.lengthened = true;
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
bool convertIpaToTokens(
  const PackSet& pack,
  const std::string& ipaUtf8,
  double speed,
  double basePitch,
  double inflection,
  char clauseType,
  std::vector<Token>& outTokens,
  std::string& outError
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

  const std::u32string normalized = normalizeIpaText(pack, ipaUtf8);
  if (normalized.empty()) {
    return true;
  }

  if (!parseToTokens(pack, normalized, outTokens, outError)) {
    return false;
  }

  if (outTokens.empty()) {
    return true;
  }

  // Optional: auto-tie diphthongs when IPA does not include an explicit tie-bar.
  autoTieDiphthongs(pack, outTokens);

  // Optional: spelling diphthong handling (e.g. acronym letter names).
  applySpellingDiphthongMode(pack, outTokens);

  // Copy-adjacent correction (h, inserted aspirations, etc.).
  correctCopyAdjacent(outTokens);

  // Transforms (language-specific tuning for aspiration, fricatives, etc.).
  applyTransforms(pack.lang, outTokens);

  // Ensure voice defaults (vibrato, GOQ, gains) exist.
  for (Token& t : outTokens) {
    if (!t.def || t.silence) continue;
    setDefaultVoiceFields(pack.lang, t);
  }

  // Timing.
  calculateTimes(outTokens, pack, speed);

  // Pitch.
  calculatePitches(outTokens, pack, speed, basePitch, inflection, clauseType);

  // Tone overlay (optional).
  applyToneContours(outTokens, pack, basePitch, inflection);

  return true;
}

void emitFrames(
  const PackSet& pack,
  const std::vector<Token>& tokens,
  int userIndexBase,
  nvspFrontend_FrameCallback cb,
  void* userData
) {
  if (!cb) return;

  // We intentionally treat nvspFrontend_Frame as a dense sequence of doubles.
  // Enforce that assumption at compile time so future edits fail loudly.
  static_assert(sizeof(nvspFrontend_Frame) == sizeof(double) * kFrameFieldCount,
                "nvspFrontend_Frame must remain exactly kFrameFieldCount doubles with no padding");
  static_assert(std::is_standard_layout<nvspFrontend_Frame>::value,
                "nvspFrontend_Frame must remain standard-layout");
  static_assert(std::is_trivially_copyable<nvspFrontend_Frame>::value,
                "nvspFrontend_Frame must remain trivially copyable");

  const bool trillEnabled = (pack.lang.trillModulationMs > 0.0);

  const int vp = static_cast<int>(FieldId::voicePitch);
  const int evp = static_cast<int>(FieldId::endVoicePitch);
  const int va = static_cast<int>(FieldId::voiceAmplitude);
  const int fa = static_cast<int>(FieldId::fricationAmplitude);

  // Trill modulation constants.
  //
  // We implement the trill as an amplitude modulation on voiceAmplitude using a
  // sequence of short frames (micro-frames). This keeps the speechPlayer.dll ABI
  // stable (no extra fields) while avoiding pack-level hacks such as duplicating
  // phoneme tokens.
  //
  // These constants were chosen to produce an audible trill without introducing
  // clicks or an overly "tremolo" sound. Packs can tune the modulation speed and
  // fade via settings, but not the depth (kept fixed for simplicity).
  constexpr double kTrillCloseFactor = 0.22;   // voiceAmplitude multiplier during closure
  constexpr double kTrillCloseFrac = 0.28;     // fraction of cycle spent in closure
  constexpr double kTrillFricFloor = 0.12;     // minimum fricationAmplitude during closure (if frication is present)
  constexpr double kMinCycleMs = 6.0;          // avoid pathological configs
  constexpr double kMaxCycleMs = 120.0;
  constexpr double kMinPhaseMs = 1.0;

  for (const Token& t : tokens) {
    if (t.silence || !t.def) {
      cb(userData, nullptr, t.durationMs, t.fadeMs, userIndexBase);
      continue;
    }

    // Build a dense array of doubles and memcpy into the frame.
    // This avoids UB from treating a struct as an array via pointer arithmetic.
    double base[kFrameFieldCount] = {};
    const std::uint64_t mask = t.setMask;
    for (int f = 0; f < kFrameFieldCount; ++f) {
      if ((mask & (1ull << f)) == 0) continue;
      base[f] = t.field[f];
    }

    // Optional trill modulation (only when `_isTrill` is true for the phoneme).
    if (trillEnabled && tokenIsTrill(t) && t.durationMs > 0.0) {
      double totalDur = t.durationMs;

      // Cycle length is an absolute milliseconds value from the pack.
      double cycleMs = pack.lang.trillModulationMs;
      if (cycleMs < kMinCycleMs) cycleMs = kMinCycleMs;
      if (cycleMs > kMaxCycleMs) cycleMs = kMaxCycleMs;

      // For short trills, compress the cycle so we still get at least one closure dip.
      if (cycleMs > totalDur) cycleMs = totalDur;

      // Split the cycle into an "open" and "closure" phase.
      double closeMs = cycleMs * kTrillCloseFrac;
      double openMs = cycleMs - closeMs;

      // Keep both phases non-trivial (prevents zero-length frames).
      if (openMs < kMinPhaseMs) {
        openMs = kMinPhaseMs;
        closeMs = std::max(kMinPhaseMs, cycleMs - openMs);
      }
      if (closeMs < kMinPhaseMs) {
        closeMs = kMinPhaseMs;
        openMs = std::max(kMinPhaseMs, cycleMs - closeMs);
      }

      // Fade between micro-frames. If not configured, choose a small default
      // relative to the cycle.
      double microFadeMs = pack.lang.trillModulationFadeMs;
      if (microFadeMs <= 0.0) {
        microFadeMs = std::min(2.0, cycleMs * 0.12);
      }

      const bool hasVoiceAmp = ((mask & (1ull << va)) != 0);
      const bool hasFricAmp = ((mask & (1ull << fa)) != 0);
      const double baseVoiceAmp = base[va];
      const double baseFricAmp = base[fa];

      const double startPitch = base[vp];
      const double endPitch = base[evp];
      const double pitchDelta = endPitch - startPitch;

      double remaining = totalDur;
      double pos = 0.0;
      bool highPhase = true;

      while (remaining > 1e-9) {
        double phaseDur = highPhase ? openMs : closeMs;
        if (phaseDur > remaining) phaseDur = remaining;

        // Interpolate pitch over the original token's duration so pitch remains continuous.
        double t0 = (totalDur > 0.0) ? (pos / totalDur) : 0.0;
        double t1 = (totalDur > 0.0) ? ((pos + phaseDur) / totalDur) : 1.0;

        double seg[kFrameFieldCount];
        std::memcpy(seg, base, sizeof(seg));

        seg[vp] = startPitch + pitchDelta * t0;
        seg[evp] = startPitch + pitchDelta * t1;

        if (!highPhase) {
          if (hasVoiceAmp) {
            seg[va] = baseVoiceAmp * kTrillCloseFactor;
          }
          // Add a small noise burst on closure to make the trill more perceptible,
          // but only if the phoneme already has a frication path.
          if (hasFricAmp) {
            seg[fa] = std::max(baseFricAmp, kTrillFricFloor);
          }
        }

        nvspFrontend_Frame frame;
        std::memcpy(&frame, seg, sizeof(frame));

        // Internal fades use microFadeMs; the final micro-frame must use the
        // token's original fade (transition to the next phoneme).
        const bool isLast = (remaining - phaseDur) <= 1e-9;
        double fadeOut = isLast ? t.fadeMs : microFadeMs;
        if (isLast && fadeOut < microFadeMs) fadeOut = microFadeMs;

        // Prevent fade dominating very short micro-frames.
        if (fadeOut > phaseDur) fadeOut = phaseDur;

        cb(userData, &frame, phaseDur, fadeOut, userIndexBase);

        remaining -= phaseDur;
        pos += phaseDur;
        highPhase = !highPhase;

        // If the remaining duration is too small to fit another phase, let the loop
        // handle it naturally by truncating phaseDur above.
      }

      continue;
    }

    nvspFrontend_Frame frame;
    std::memcpy(&frame, base, sizeof(frame));

    cb(userData, &frame, t.durationMs, t.fadeMs, userIndexBase);
  }
}

} // namespace nvsp_frontend
