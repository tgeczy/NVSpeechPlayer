/*
TGSpeechBox — Generic data query layer implementation.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include "data_query.h"
#include "pack.h"     // getEffectiveSettings, parseFieldId, parseFlagKey
#include "utf8.h"     // normalizeLangTag
#include "yaml_min.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Forward declaration of internal pack.cpp helper.
namespace nvsp_frontend {
std::string getEffectiveSettings(const std::string& packDir,
                                 const std::string& langTag);
}

namespace tgsb_data {

using nvsp_frontend::yaml_min::Node;
namespace yaml_min = nvsp_frontend::yaml_min;

// ── Type detection ──────────────────────────────────────────────────

FieldType detectType(const std::string& value) {
  if (value == "true" || value == "false") return FieldType::Bool;

  // Try parsing as a number.
  if (!value.empty()) {
    char* end = nullptr;
    errno = 0;
    (void)std::strtod(value.c_str(), &end);
    if (errno == 0 && end != value.c_str() && *end == '\0') {
      return FieldType::Float;
    }
  }
  return FieldType::String;
}

// ── Group extraction ────────────────────────────────────────────────

std::string extractGroup(const std::string& key) {
  auto dot = key.find('.');
  if (dot == std::string::npos) return {};
  return key.substr(0, dot);
}

// ── Settings cache builder ──────────────────────────────────────────

void buildSettingsCache(DataCache& cache,
                        const std::string& packDir,
                        const std::string& langTag) {
  cache.settings.clear();
  cache.langTag = langTag;

  // Reuse existing getEffectiveSettings which reads the YAML file chain
  // and returns flattened "key\tvalue\n" lines.
  std::string raw = nvsp_frontend::getEffectiveSettings(packDir, langTag);
  if (raw.empty()) {
    cache.settingsValid = true;
    return;
  }

  // Parse the tab-separated lines into typed records.
  const char* p = raw.c_str();
  const char* end = p + raw.size();

  while (p < end) {
    // Find tab separator.
    const char* tab = p;
    while (tab < end && *tab != '\t') ++tab;
    if (tab >= end) break;

    // Find newline.
    const char* nl = tab + 1;
    while (nl < end && *nl != '\n') ++nl;

    std::string key(p, tab);
    std::string value(tab + 1, nl);

    SettingRecord rec;
    rec.key   = std::move(key);
    rec.value = std::move(value);
    rec.type  = detectType(rec.value);
    rec.group = extractGroup(rec.key);
    cache.settings.push_back(std::move(rec));

    p = (nl < end) ? nl + 1 : end;
  }

  cache.settingsValid = true;
}

// ── Phoneme helpers ──────────────────────────────────────────────────

// Classify a phoneme based on its flag keys in the YAML node.
static std::string classifyPhoneme(const yaml_min::Node& defNode) {
  auto getBoolKey = [&](const char* k) -> bool {
    const yaml_min::Node* n = defNode.get(k);
    if (!n) return false;
    bool b = false;
    n->asBool(b);
    return b;
  };

  // Order matters — more specific first.
  if (getBoolKey("_isAffricate"))  return "affricate";
  if (getBoolKey("_isTap"))       return "tap";
  if (getBoolKey("_isTrill"))     return "trill";
  if (getBoolKey("_isStop"))      return "stop";
  if (getBoolKey("_isNasal"))     return "nasal";
  if (getBoolKey("_isLiquid"))    return "liquid";
  if (getBoolKey("_isSemivowel")) return "semivowel";
  if (getBoolKey("_isVowel"))     return "vowel";

  // Check if it has frication (fricative-like).
  const yaml_min::Node* fricNode = defNode.get("fricationAmplitude");
  if (fricNode) {
    double v = 0;
    if (fricNode->asNumber(v) && v > 0) return "fricative";
  }
  // Check aspiration (e.g. /h/).
  const yaml_min::Node* aspNode = defNode.get("aspirationAmplitude");
  if (aspNode) {
    double v = 0;
    if (aspNode->asNumber(v) && v > 0) return "fricative";
  }

  return "other";
}

// Known flag key names.
static const char* const kFlagKeys[] = {
  "_isAffricate", "_isLiquid", "_isNasal", "_isSemivowel",
  "_isStop", "_isTap", "_isTrill", "_isVoiced", "_isVowel",
  "_copyAdjacent"
};
static constexpr size_t kNumFlagKeys = sizeof(kFlagKeys) / sizeof(kFlagKeys[0]);

// Known frameEx sub-keys.
static const char* const kFrameExKeys[] = {
  "creakiness", "breathiness", "jitter", "shimmer", "sharpness",
  "endCf1", "endCf2", "endCf3", "endPf1", "endPf2", "endPf3"
};
static constexpr size_t kNumFrameExKeys = sizeof(kFrameExKeys) / sizeof(kFrameExKeys[0]);

// Known micro-event keys (top-level on phoneme, not frame fields).
static const char* const kMicroEventKeys[] = {
  "burstDurationMs", "burstDecayRate", "burstSpectralTilt",
  "voiceBarAmplitude", "voiceBarF1", "releaseSpreadMs",
  "fricAttackMs", "fricDecayMs", "durationScale", "closureGapMs",
  "cf7", "cb7", "cf8", "cb8"
};
static constexpr size_t kNumMicroEventKeys = sizeof(kMicroEventKeys) / sizeof(kMicroEventKeys[0]);

// Format a double without unnecessary trailing zeros.
static std::string fmtDouble(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.10g", v);
  return buf;
}

// Emit PhonemeRecords for one phoneme node into the output vector.
static void emitPhonemeRecords(const std::string& phonemeKey,
                               const yaml_min::Node& defNode,
                               const std::string& phonemeClass,
                               const std::string& mappingFrom,
                               std::vector<PhonemeRecord>& out) {
  // Emit flag fields.
  for (size_t i = 0; i < kNumFlagKeys; ++i) {
    const yaml_min::Node* val = defNode.get(kFlagKeys[i]);
    if (!val) continue;
    bool b = false;
    if (!val->asBool(b)) continue;

    PhonemeRecord rec;
    rec.key = phonemeKey + "." + kFlagKeys[i];
    rec.type = FieldType::Bool;
    rec.value = b ? "true" : "false";
    rec.group = phonemeKey;
    rec.phonemeClass = phonemeClass;
    rec.mappingFrom = mappingFrom;
    out.push_back(std::move(rec));
  }

  // Emit frame fields (FieldId-based: cf1, cb2, pa3, etc.).
  for (const auto& kv : defNode.map) {
    const std::string& fieldName = kv.first;
    // Skip flags, frameEx, and micro-event keys — handled separately.
    if (!fieldName.empty() && fieldName[0] == '_') continue;
    if (fieldName == "frameEx") continue;

    // Check if it's a micro-event key.
    bool isMicro = false;
    for (size_t i = 0; i < kNumMicroEventKeys; ++i) {
      if (fieldName == kMicroEventKeys[i]) { isMicro = true; break; }
    }

    double num = 0;
    if (!kv.second.asNumber(num)) continue;

    PhonemeRecord rec;
    rec.key = phonemeKey + "." + fieldName;
    rec.type = FieldType::Float;
    rec.value = fmtDouble(num);
    rec.group = phonemeKey;
    rec.phonemeClass = phonemeClass;
    rec.mappingFrom = mappingFrom;
    out.push_back(std::move(rec));
  }

  // Emit frameEx sub-fields.
  const yaml_min::Node* fxNode = defNode.get("frameEx");
  if (fxNode && fxNode->isMap()) {
    for (size_t i = 0; i < kNumFrameExKeys; ++i) {
      const yaml_min::Node* val = fxNode->get(kFrameExKeys[i]);
      if (!val) continue;
      double num = 0;
      if (!val->asNumber(num)) continue;

      PhonemeRecord rec;
      rec.key = phonemeKey + ".frameEx." + kFrameExKeys[i];
      rec.type = FieldType::Float;
      rec.value = fmtDouble(num);
      rec.group = phonemeKey;
      rec.phonemeClass = phonemeClass;
      rec.mappingFrom = mappingFrom;
      out.push_back(std::move(rec));
    }
  }
}

// Find phonemes.yaml given a packDir.
static std::string findPhonemesPath(const std::string& packDir) {
  namespace fs = std::filesystem;
  fs::path direct = fs::path(packDir) / "phonemes.yaml";
  if (fs::exists(direct)) return direct.string();
  fs::path nested = fs::path(packDir) / "packs" / "phonemes.yaml";
  if (fs::exists(nested)) return nested.string();
  return {};
}

// Find the lang directory given a packDir.
static std::string findLangDir(const std::string& packDir) {
  namespace fs = std::filesystem;
  // Check packDir/lang/
  fs::path direct = fs::path(packDir) / "lang";
  if (fs::exists(direct) && fs::is_directory(direct)) return direct.string();
  // Check packDir/packs/lang/
  fs::path nested = fs::path(packDir) / "packs" / "lang";
  if (fs::exists(nested) && fs::is_directory(nested)) return nested.string();
  return {};
}

// Decompose a replacement "to" string into individual phoneme keys by
// greedy matching against the phoneme table.  For example, "a_gb͡ɪ_gb"
// decomposes to ["a_gb", "ɪ_gb"] (tie bar U+0361 is a separator).
// Returns matched phoneme keys.  Unmatched portions are silently skipped
// (stress marks, length marks, tie bars are not phoneme keys).
static std::vector<std::string> decomposeTarget(
    const std::string& target,
    const std::unordered_set<std::string>& phonemeKeys) {
  std::vector<std::string> result;

  // First check if the whole string is a phoneme key.
  if (phonemeKeys.count(target)) {
    result.push_back(target);
    return result;
  }

  // Convert to u32 for codepoint-level iteration.
  std::u32string u32 = nvsp_frontend::utf8ToU32(target);

  // Greedy longest-match from each position.
  size_t pos = 0;
  while (pos < u32.size()) {
    char32_t c = u32[pos];

    // Skip tie bars (U+0361, U+035C), stress marks, length marks.
    if (c == 0x0361 || c == 0x035C ||  // tie bars
        c == 0x02C8 || c == 0x02CC ||  // primary/secondary stress
        c == 0x02D0 || c == 0x02D1) {  // length marks
      ++pos;
      continue;
    }

    // Try longest match first (up to 8 codepoints).
    bool matched = false;
    size_t maxLen = std::min(u32.size() - pos, size_t(8));
    for (size_t len = maxLen; len >= 1; --len) {
      std::u32string sub = u32.substr(pos, len);
      // Convert back to UTF-8 for lookup.
      std::string subUtf8;
      for (char32_t ch : sub) {
        if (ch < 0x80) {
          subUtf8 += static_cast<char>(ch);
        } else if (ch < 0x800) {
          subUtf8 += static_cast<char>(0xC0 | (ch >> 6));
          subUtf8 += static_cast<char>(0x80 | (ch & 0x3F));
        } else if (ch < 0x10000) {
          subUtf8 += static_cast<char>(0xE0 | (ch >> 12));
          subUtf8 += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
          subUtf8 += static_cast<char>(0x80 | (ch & 0x3F));
        } else {
          subUtf8 += static_cast<char>(0xF0 | (ch >> 18));
          subUtf8 += static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
          subUtf8 += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
          subUtf8 += static_cast<char>(0x80 | (ch & 0x3F));
        }
      }
      if (phonemeKeys.count(subUtf8)) {
        result.push_back(subUtf8);
        pos += len;
        matched = true;
        break;
      }
    }
    if (!matched) {
      ++pos;  // skip unrecognized codepoint
    }
  }

  return result;
}

// Collect replacement "to" targets from a language's normalization.replacements.
// Decomposes compound targets into individual phoneme keys.
// Returns a map of phonemeKey → vector of "from" strings.
static std::unordered_map<std::string, std::vector<std::string>>
collectReplacementTargets(const std::string& packDir, const std::string& langTag,
                          const std::unordered_set<std::string>& phonemeKeys) {
  std::unordered_map<std::string, std::vector<std::string>> targets;

  std::string langDir = findLangDir(packDir);
  if (langDir.empty()) return targets;

  // Build the lang file chain: default, en, en-gb, etc.
  std::string normalized = nvsp_frontend::normalizeLangTag(langTag);
  std::vector<std::string> chain;
  chain.push_back("default");
  std::string cur;
  for (char c : normalized) {
    if (c == '-') {
      if (!cur.empty()) {
        chain.push_back(cur);
        cur += '-';
      }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) chain.push_back(cur);

  // Deduplicate.
  std::vector<std::string> unique;
  std::unordered_set<std::string> seen;
  for (const auto& x : chain) {
    if (seen.insert(x).second) unique.push_back(x);
  }

  namespace fs = std::filesystem;
  for (const auto& name : unique) {
    fs::path file = fs::path(langDir) / (name + ".yaml");
    if (!fs::exists(file)) continue;

    yaml_min::Node root;
    std::string err;
    if (!yaml_min::loadFile(file.string(), root, err)) continue;

    const yaml_min::Node* norm = root.get("normalization");
    if (!norm || !norm->isMap()) continue;

    const yaml_min::Node* repl = norm->get("replacements");
    if (!repl || !repl->isSeq()) continue;

    for (const auto& item : repl->seq) {
      if (!item.isMap()) continue;
      const yaml_min::Node* fromNode = item.get("from");
      const yaml_min::Node* toNode = item.get("to");
      if (!fromNode || !toNode) continue;
      std::string from = fromNode->asString();
      std::string to = toNode->asString();
      if (to.empty()) continue;  // deletion rules aren't phoneme references

      // Decompose compound target into individual phoneme keys.
      auto parts = decomposeTarget(to, phonemeKeys);
      for (const auto& pk : parts) {
        targets[pk].push_back(from);
      }
    }
  }

  return targets;
}

// ── Phonemes cache builder ───────────────────────────────────────────

void buildPhonemesCache(DataCache& cache,
                        const std::string& packDir,
                        const std::string& langTag) {
  cache.phonemes.clear();
  cache.phonemesLangTag = langTag;

  std::string phonemesPath = findPhonemesPath(packDir);
  if (phonemesPath.empty()) {
    cache.phonemesValid = true;
    return;
  }

  yaml_min::Node root;
  std::string yamlErr;
  if (!yaml_min::loadFile(phonemesPath, root, yamlErr)) {
    cache.phonemesValid = true;
    return;
  }

  const yaml_min::Node* phonemesNode = root.get("phonemes");
  if (!phonemesNode || !phonemesNode->isMap()) {
    cache.phonemesValid = true;
    return;
  }

  // Build set of all phoneme keys for decomposition lookups.
  std::unordered_set<std::string> phonemeKeySet;
  for (const auto& kv : phonemesNode->map) {
    phonemeKeySet.insert(kv.first);
  }

  if (langTag.empty()) {
    // All phonemes mode — emit every phoneme from phonemes.yaml.
    // Use keyOrder for stable ordering.
    const auto& keys = phonemesNode->keyOrder;
    for (const auto& phonemeKey : keys) {
      auto it = phonemesNode->map.find(phonemeKey);
      if (it == phonemesNode->map.end()) continue;
      const yaml_min::Node& defNode = it->second;
      if (!defNode.isMap()) continue;

      std::string cls = classifyPhoneme(defNode);
      emitPhonemeRecords(phonemeKey, defNode, cls, "", cache.phonemes);
    }
  } else {
    // Language-filtered mode — only phonemes that are replacement targets.
    auto targets = collectReplacementTargets(packDir, langTag, phonemeKeySet);
    if (targets.empty()) {
      cache.phonemesValid = true;
      return;
    }

    // Emit in phonemes.yaml order, filtered to targets only.
    const auto& keys = phonemesNode->keyOrder;
    for (const auto& phonemeKey : keys) {
      auto targetIt = targets.find(phonemeKey);
      if (targetIt == targets.end()) continue;

      auto it = phonemesNode->map.find(phonemeKey);
      if (it == phonemesNode->map.end()) continue;
      const yaml_min::Node& defNode = it->second;
      if (!defNode.isMap()) continue;

      // Build a comma-separated "from" string for context.
      // Deduplicate since multiple rules can map to the same phoneme.
      std::vector<std::string> fromList;
      std::unordered_set<std::string> fromSeen;
      for (const auto& f : targetIt->second) {
        if (fromSeen.insert(f).second) fromList.push_back(f);
      }
      std::string mappingFrom;
      for (size_t i = 0; i < fromList.size(); ++i) {
        if (i > 0) mappingFrom += ", ";
        mappingFrom += fromList[i];
      }

      std::string cls = classifyPhoneme(defNode);
      emitPhonemeRecords(phonemeKey, defNode, cls, mappingFrom, cache.phonemes);
    }
  }

  cache.phonemesValid = true;
}

// ── String field options ─────────────────────────────────────────────

// Returns a list of valid values for enum-like string settings, or empty.
static std::vector<const char*> getStringOptions(const std::string& key) {
  if (key == "stopClosureMode")
    return {"always", "after-vowel", "vowel-and-cluster"};
  if (key == "toneContoursMode")
    return {"absolute", "relative"};
  if (key == "spellingDiphthongMode")
    return {"none", "monophthong"};
  if (key == "prominence.longVowelMode")
    return {"never", "always", "unstressed-only"};
  if (key == "singleWordClauseTypeOverride")
    return {".", ",", "?", "!"};
  return {};
}

// ── JSON helpers ────────────────────────────────────────────────────

// Escape a string for JSON output (handles quotes, backslashes, control chars).
static void jsonEscapeAppend(std::string& out, const std::string& s) {
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

static const char* fieldTypeName(FieldType t) {
  switch (t) {
    case FieldType::Float:  return "float";
    case FieldType::Bool:   return "bool";
    case FieldType::String: return "string";
  }
  return "string";
}

// ── Settings JSON serializer ────────────────────────────────────────

std::string serializeSettingsJson(const DataCache& cache, int offset, int limit) {
  const auto& vec = cache.settings;
  const int total = static_cast<int>(vec.size());

  // Clamp offset.
  if (offset < 0) offset = 0;
  if (offset >= total) return "[]";

  // Determine end index.
  int endIdx = total;
  if (limit > 0 && offset + limit < total) {
    endIdx = offset + limit;
  }

  std::string out;
  out.reserve((endIdx - offset) * 120);  // rough estimate
  out += '[';

  bool first = true;
  for (int i = offset; i < endIdx; ++i) {
    const auto& rec = vec[i];

    if (!first) out += ',';
    first = false;

    out += '{';

    // "key": "..."
    out += "\"key\":";
    jsonEscapeAppend(out, rec.key);

    // "type": "float"|"bool"|"string"
    out += ",\"type\":\"";
    out += fieldTypeName(rec.type);
    out += '"';

    // "value": ...  (typed: number/bool unquoted, string quoted)
    out += ",\"value\":";
    if (rec.type == FieldType::Bool) {
      out += rec.value;  // "true" or "false" — valid JSON literals
    } else if (rec.type == FieldType::Float) {
      out += rec.value;  // numeric literal
    } else {
      jsonEscapeAppend(out, rec.value);
    }

    // "group": "..."
    out += ",\"group\":";
    jsonEscapeAppend(out, rec.group);

    // "options": ["a","b","c"]  (only for enum-like string fields)
    if (rec.type == FieldType::String) {
      auto opts = getStringOptions(rec.key);
      if (!opts.empty()) {
        out += ",\"options\":[";
        for (size_t j = 0; j < opts.size(); ++j) {
          if (j > 0) out += ',';
          jsonEscapeAppend(out, opts[j]);
        }
        out += ']';
      }
    }

    out += '}';
  }

  out += ']';
  return out;
}

// ── Phonemes JSON serializer ─────────────────────────────────────────

std::string serializePhonemesJson(const DataCache& cache, int offset, int limit) {
  const auto& vec = cache.phonemes;
  const int total = static_cast<int>(vec.size());

  if (offset < 0) offset = 0;
  if (offset >= total) return "[]";

  int endIdx = total;
  if (limit > 0 && offset + limit < total) {
    endIdx = offset + limit;
  }

  std::string out;
  out.reserve((endIdx - offset) * 140);
  out += '[';

  bool first = true;
  for (int i = offset; i < endIdx; ++i) {
    const auto& rec = vec[i];

    if (!first) out += ',';
    first = false;

    out += '{';

    // "key": "ɪ.cf2"
    out += "\"key\":";
    jsonEscapeAppend(out, rec.key);

    // "type": "float"|"bool"|"string"
    out += ",\"type\":\"";
    out += fieldTypeName(rec.type);
    out += '"';

    // "value": ...
    out += ",\"value\":";
    if (rec.type == FieldType::Bool) {
      out += rec.value;
    } else if (rec.type == FieldType::Float) {
      out += rec.value;
    } else {
      jsonEscapeAppend(out, rec.value);
    }

    // "group": "ɪ"
    out += ",\"group\":";
    jsonEscapeAppend(out, rec.group);

    // "class": "vowel"
    out += ",\"class\":";
    jsonEscapeAppend(out, rec.phonemeClass);

    // "mappingFrom": "ɜː" (only in lang-filtered view)
    if (!rec.mappingFrom.empty()) {
      out += ",\"mappingFrom\":";
      jsonEscapeAppend(out, rec.mappingFrom);
    }

    out += '}';
  }

  out += ']';
  return out;
}

// ── Dictionary cache builder ─────────────────────────────────────────

void buildDictionaryCache(DataCache& cache,
                          const nvsp_frontend::PronDict& dict) {
  cache.dictionary.clear();
  cache.dictionary.reserve(dict.entries.size());

  for (const auto& kv : dict.entries) {
    DictRecord rec;
    rec.key      = kv.second.fromText;
    rec.toText   = kv.second.toText;
    rec.fromIpa  = kv.second.fromIpa;
    rec.toIpa    = kv.second.toIpa;
    rec.category = kv.second.category;
    rec.source   = kv.second.source;
    rec.masked   = kv.second.masked;
    cache.dictionary.push_back(std::move(rec));
  }

  // Sort alphabetically by key for stable ordering.
  std::sort(cache.dictionary.begin(), cache.dictionary.end(),
            [](const DictRecord& a, const DictRecord& b) {
              return a.key < b.key;
            });

  cache.dictionaryValid = true;
}

// ── Stress dictionary cache builder ──────────────────────────────────

void buildStressDictCache(DataCache& cache,
                          const std::unordered_map<std::string, std::vector<int>>& stressDict) {
  cache.dictionary.clear();
  cache.dictionary.reserve(stressDict.size());

  for (const auto& kv : stressDict) {
    DictRecord rec;
    rec.key = kv.first;
    // Join stress digits with spaces: {1, 0, 2} → "1 0 2"
    std::string joined;
    for (size_t i = 0; i < kv.second.size(); ++i) {
      if (i > 0) joined += ' ';
      joined += std::to_string(kv.second[i]);
    }
    rec.toText = std::move(joined);
    rec.source = "main";
    cache.dictionary.push_back(std::move(rec));
  }

  std::sort(cache.dictionary.begin(), cache.dictionary.end(),
            [](const DictRecord& a, const DictRecord& b) {
              return a.key < b.key;
            });

  cache.dictionaryValid = true;
}

// ── Compound map cache builder ───────────────────────────────────────

void buildCompoundDictCache(DataCache& cache,
                            const std::unordered_map<std::string, std::vector<std::string>>& compoundMap) {
  cache.dictionary.clear();
  cache.dictionary.reserve(compoundMap.size());

  for (const auto& kv : compoundMap) {
    DictRecord rec;
    rec.key = kv.first;
    // Join parts with spaces: {"lock", "box"} → "lock box"
    std::string joined;
    for (size_t i = 0; i < kv.second.size(); ++i) {
      if (i > 0) joined += ' ';
      joined += kv.second[i];
    }
    rec.toText = std::move(joined);
    rec.source = "main";
    cache.dictionary.push_back(std::move(rec));
  }

  std::sort(cache.dictionary.begin(), cache.dictionary.end(),
            [](const DictRecord& a, const DictRecord& b) {
              return a.key < b.key;
            });

  cache.dictionaryValid = true;
}

// ── Character/letter dictionary cache builder ────────────────────────

void buildCharacterDictCache(DataCache& cache,
                             const std::unordered_map<std::string, std::string>& letterDict) {
  cache.dictionary.clear();
  cache.dictionary.reserve(letterDict.size());

  for (const auto& kv : letterDict) {
    DictRecord rec;
    rec.key = kv.first;
    rec.toText = kv.second;
    rec.source = "main";
    cache.dictionary.push_back(std::move(rec));
  }

  std::sort(cache.dictionary.begin(), cache.dictionary.end(),
            [](const DictRecord& a, const DictRecord& b) {
              return a.key < b.key;
            });

  cache.dictionaryValid = true;
}

// ── Dictionary JSON serializer ───────────────────────────────────────

// Case-insensitive prefix match helper.
static bool keyMatchesSearch(const std::string& key, const std::string& search) {
  if (search.empty()) return true;
  if (key.size() < search.size()) return false;
  for (size_t i = 0; i < search.size(); ++i) {
    char a = static_cast<char>(std::tolower(static_cast<unsigned char>(key[i])));
    if (a != search[i]) return false;  // search is already lowered
  }
  return true;
}

std::string serializeDictionaryJson(const DataCache& cache, int offset, int limit,
                                    const std::string& search) {
  const auto& vec = cache.dictionary;

  if (offset < 0) offset = 0;

  std::string out;
  out.reserve(std::min(static_cast<int>(vec.size()), limit > 0 ? limit : 200) * 200);
  out += '[';

  bool first = true;
  int matched = 0;
  int emitted = 0;

  for (const auto& rec : vec) {
    if (!keyMatchesSearch(rec.key, search)) continue;

    if (matched < offset) { ++matched; continue; }
    ++matched;

    if (limit > 0 && emitted >= limit) break;

    if (!first) out += ',';
    first = false;

    out += '{';

    out += "\"key\":";
    jsonEscapeAppend(out, rec.key);

    out += ",\"fromText\":";
    jsonEscapeAppend(out, rec.key);

    out += ",\"toText\":";
    jsonEscapeAppend(out, rec.toText);

    out += ",\"fromIpa\":";
    jsonEscapeAppend(out, rec.fromIpa);

    out += ",\"toIpa\":";
    jsonEscapeAppend(out, rec.toIpa);

    out += ",\"category\":";
    jsonEscapeAppend(out, rec.category);

    out += ",\"source\":";
    jsonEscapeAppend(out, rec.source);

    out += ",\"masked\":";
    out += rec.masked ? "true" : "false";

    out += '}';
    ++emitted;
  }

  out += ']';
  return out;
}

int countDictionaryMatches(const DataCache& cache, const std::string& search) {
  if (search.empty()) return static_cast<int>(cache.dictionary.size());
  int count = 0;
  for (const auto& rec : cache.dictionary) {
    if (keyMatchesSearch(rec.key, search)) ++count;
  }
  return count;
}

} // namespace tgsb_data
