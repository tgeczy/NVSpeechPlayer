/*
TGSpeechBox — Generic data query API implementation (ABI v5+).
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include "frontend_handle.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "data_query.h"
#include "pack.h"
#include "utf8.h"

// ── Generic Data Query API (ABI v5+) ──────────────────────────────

// Parse a DICTIONARY langTag into sub-type prefix, actual language, and
// optional search query.
// "stress:en-us"       -> subType="stress", lang="en-us", search=""
// "stress:en-us?aard"  -> subType="stress", lang="en-us", search="aard"
// "en-us?monster"      -> subType="",       lang="en-us", search="monster"
// "en-us"              -> subType="",       lang="en-us", search=""
// "types"              -> subType="types",  lang="",      search=""
static void parseDictLangTag(const char* langTagUtf8,
                             std::string& subType, std::string& lang,
                             std::string& search) {
  std::string raw(langTagUtf8);
  search.clear();

  // Extract search query after '?'.
  auto qPos = raw.find('?');
  if (qPos != std::string::npos) {
    search = raw.substr(qPos + 1);
    // Lowercase search for case-insensitive matching.
    for (auto& c : search)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    raw = raw.substr(0, qPos);
  }

  auto colonPos = raw.find(':');
  if (colonPos != std::string::npos) {
    subType = raw.substr(0, colonPos);
    lang = raw.substr(colonPos + 1);
  } else if (raw == "types") {
    subType = "types";
    lang.clear();
  } else {
    subType.clear();
    lang = std::move(raw);
  }
}

// Helper: get dict data for a language, loading from disk if needed for
// cross-language browsing.  Returns pointers to the appropriate dicts.
struct DictRefs {
  const std::unordered_map<std::string, std::vector<int>>* stressDict;
  const std::unordered_map<std::string, std::vector<std::string>>* compoundMap;
  const nvsp_frontend::PronDict* pronDict;
  const std::unordered_map<std::string, std::string>* letterDict;
};

static DictRefs getDictRefs(nvsp_frontend::Handle* h, const std::string& lang) {
  using namespace nvsp_frontend;
  if (lang.empty() || lang == h->langTag) {
    return { &h->pack.stressDict, &h->pack.compoundMap,
             &h->pack.pronDict, &h->pack.letterDict };
  }
  if (!h->crossLangDicts || h->crossLangDicts->lang != lang) {
    h->crossLangDicts = std::make_unique<Handle::CrossLangDicts>();
    h->crossLangDicts->lang = lang;
    loadDictFiles(h->packDir, lang,
                  h->crossLangDicts->stressDict,
                  h->crossLangDicts->compoundMap,
                  h->crossLangDicts->pronDict,
                  h->crossLangDicts->letterDict,
                  h->overrideDir);
  }
  return { &h->crossLangDicts->stressDict, &h->crossLangDicts->compoundMap,
           &h->crossLangDicts->pronDict, &h->crossLangDicts->letterDict };
}

// ── Phoneme Preview (direct DSP pump, no pipeline) ────────────────

extern "C" {

NVSP_FRONTEND_API int nvspFrontend_previewPhoneme(
  nvspFrontend_handle_t handle,
  const char* phonemeKeyUtf8,
  double pitchHz,
  double durationMs,
  nvspFrontend_FrameExCallback cb,
  void* userData
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h || !phonemeKeyUtf8 || !phonemeKeyUtf8[0] || !cb) return 0;

  std::lock_guard<std::mutex> lock(h->mu);

  if (!h->packLoaded) return 0;

  // Look up phoneme directly in the loaded pack.
  std::u32string phonU32 = utf8ToU32(std::string(phonemeKeyUtf8));
  auto it = h->pack.phonemes.find(phonU32);
  if (it == h->pack.phonemes.end()) return 0;

  const PhonemeDef& def = it->second;

  // Build frame directly from PhonemeDef field array.
  // FieldId enum maps 1:1 to nvspFrontend_Frame struct layout.
  nvspFrontend_Frame frame;
  std::memset(&frame, 0, sizeof(frame));
  static_assert(sizeof(nvspFrontend_Frame) == kFrameFieldCount * sizeof(double),
    "Frame/FieldId layout mismatch");
  double* fp = reinterpret_cast<double*>(&frame);
  for (int i = 0; i < kFrameFieldCount; ++i) {
    fp[i] = def.field[i];
  }

  // Override pitch with requested value.
  frame.voicePitch = pitchHz;
  frame.endVoicePitch = pitchHz;

  // Set sensible defaults for a steady-state preview.
  if (frame.preFormantGain <= 0.0) frame.preFormantGain = 1.0;
  if (frame.outputGain <= 0.0) frame.outputGain = 1.0;

  // Build frameEx from PhonemeDef's optional fields.
  nvspFrontend_FrameEx frameEx;
  std::memset(&frameEx, 0, sizeof(frameEx));
  // Set NAN for end-formant fields (no ramping).
  frameEx.endCf1 = NAN; frameEx.endCf2 = NAN; frameEx.endCf3 = NAN;
  frameEx.endPf1 = NAN; frameEx.endPf2 = NAN; frameEx.endPf3 = NAN;
  if (def.hasCreakiness)  frameEx.creakiness  = def.creakiness;
  if (def.hasBreathiness) frameEx.breathiness = def.breathiness;
  if (def.hasJitter)      frameEx.jitter      = def.jitter;
  if (def.hasShimmer)     frameEx.shimmer     = def.shimmer;
  if (def.hasSharpness)   frameEx.sharpness   = def.sharpness;

  // Emit a steady-state phoneme frame.
  if (durationMs <= 0.0) durationMs = 300.0;
  double fadeMs = 15.0;
  cb(userData, &frame, &frameEx, durationMs, fadeMs, 0);

  // Trailing silence for clean fade-out.
  cb(userData, nullptr, nullptr, 0.0, fadeMs, 0);

  return 1;
}

// ── Data Query API ────────────────────────────────────────────────

NVSP_FRONTEND_API int nvspFrontend_getDataCount(
  nvspFrontend_handle_t handle,
  int domain,
  const char* langTagUtf8
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h || !langTagUtf8) return -1;

  std::lock_guard<std::mutex> lock(h->mu);

  if (domain == NVSP_DATA_SETTINGS) {
    if (!langTagUtf8[0]) return -1;  // settings requires a lang tag
    const std::string lang(langTagUtf8);
    if (!h->dataCache.settingsValid || h->dataCache.langTag != lang) {
      tgsb_data::buildSettingsCache(h->dataCache, h->packDir, lang);
    }
    return static_cast<int>(h->dataCache.settings.size());
  }

  if (domain == NVSP_DATA_PHONEMES) {
    const std::string lang(langTagUtf8);  // "" = all phonemes
    if (!h->dataCache.phonemesValid || h->dataCache.phonemesLangTag != lang) {
      tgsb_data::buildPhonemesCache(h->dataCache, h->packDir, lang);
    }
    return static_cast<int>(h->dataCache.phonemes.size());
  }

  if (domain == NVSP_DATA_DICTIONARY) {
    if (!h->packLoaded) return 0;

    std::string subType, lang, search;
    parseDictLangTag(langTagUtf8, subType, lang, search);

    if (subType == "config") {
      return 4;  // four dict types
    }

    if (subType == "types") {
      // All four types always available (users can add overrides).
      return 4;
    }

    auto refs = getDictRefs(h, lang);
    std::string effectiveSub = subType.empty() ? "" : subType;

    // Fast path: no search, just return size.
    if (search.empty()) {
      if (subType == "character") return static_cast<int>(refs.letterDict->size());
      if (subType == "stress")   return static_cast<int>(refs.stressDict->size());
      if (subType == "compound") return static_cast<int>(refs.compoundMap->size());
      // pronDict
      if (!h->dataCache.dictionaryValid || h->dataCache.dictionarySubType != effectiveSub ||
          h->dataCache.dictionaryLangTag != lang) {
        tgsb_data::buildDictionaryCache(h->dataCache, *refs.pronDict);
        h->dataCache.dictionarySubType = effectiveSub;
        h->dataCache.dictionaryLangTag = lang;
      }
      return static_cast<int>(h->dataCache.dictionary.size());
    }

    // Search path: build cache if needed, then count matches.
    if (!h->dataCache.dictionaryValid || h->dataCache.dictionarySubType != effectiveSub ||
        h->dataCache.dictionaryLangTag != lang) {
      if (subType == "character") tgsb_data::buildCharacterDictCache(h->dataCache, *refs.letterDict);
      else if (subType == "stress") tgsb_data::buildStressDictCache(h->dataCache, *refs.stressDict);
      else if (subType == "compound") tgsb_data::buildCompoundDictCache(h->dataCache, *refs.compoundMap);
      else tgsb_data::buildDictionaryCache(h->dataCache, *refs.pronDict);
      h->dataCache.dictionarySubType = effectiveSub;
      h->dataCache.dictionaryLangTag = lang;
    }
    return tgsb_data::countDictionaryMatches(h->dataCache, search);
  }

  if (domain == NVSP_DATA_FRAMETRACE) {
    // Frame trace is per-utterance state on the handle; no cache build needed.
    return static_cast<int>(h->frameTrace.size());
  }

  // Unsupported domain.
  return -1;
}

NVSP_FRONTEND_API char* nvspFrontend_queryData(
  nvspFrontend_handle_t handle,
  int domain,
  const char* langTagUtf8,
  int offset,
  int limit
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h || !langTagUtf8) return nullptr;

  std::lock_guard<std::mutex> lock(h->mu);

  if (domain == NVSP_DATA_SETTINGS) {
    if (!langTagUtf8[0]) return nullptr;  // settings requires a lang tag
    const std::string lang(langTagUtf8);
    if (!h->dataCache.settingsValid || h->dataCache.langTag != lang) {
      tgsb_data::buildSettingsCache(h->dataCache, h->packDir, lang);
    }

    std::string json = tgsb_data::serializeSettingsJson(h->dataCache, offset, limit);
    if (json.empty()) return nullptr;

    char* out = static_cast<char*>(std::malloc(json.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, json.c_str(), json.size() + 1);
    return out;
  }

  if (domain == NVSP_DATA_PHONEMES) {
    const std::string lang(langTagUtf8);  // "" = all phonemes
    if (!h->dataCache.phonemesValid || h->dataCache.phonemesLangTag != lang) {
      tgsb_data::buildPhonemesCache(h->dataCache, h->packDir, lang);
    }

    std::string json = tgsb_data::serializePhonemesJson(h->dataCache, offset, limit);
    if (json.empty()) return nullptr;

    char* out = static_cast<char*>(std::malloc(json.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, json.c_str(), json.size() + 1);
    return out;
  }

  if (domain == NVSP_DATA_DICTIONARY) {
    if (!h->packLoaded) return nullptr;

    std::string subType, lang, search;
    parseDictLangTag(langTagUtf8, subType, lang, search);

    if (subType == "config") {
      // Return enabled/disabled state for each dict type for this language.
      const auto& dis = h->disabledDictTypes.count(lang)
          ? h->disabledDictTypes.at(lang) : std::unordered_set<std::string>{};
      const char* types[] = {"character", "compound", "pronounce", "stress"};
      std::string json = "[";
      for (int i = 0; i < 4; ++i) {
        if (i > 0) json += ',';
        json += "{\"type\":\"";
        json += types[i];
        json += "\",\"enabled\":";
        json += dis.count(types[i]) == 0 ? "true" : "false";
        json += '}';
      }
      json += ']';
      char* out = static_cast<char*>(std::malloc(json.size() + 1));
      if (!out) return nullptr;
      std::memcpy(out, json.c_str(), json.size() + 1);
      return out;
    }

    if (subType == "types") {
      // Return JSON array of available dict types with counts for requested lang.
      auto refs = getDictRefs(h, lang);
      std::string json = "[";
      json += "{\"type\":\"character\",\"count\":";
      json += std::to_string(refs.letterDict->size());
      json += "},{\"type\":\"compound\",\"count\":";
      json += std::to_string(refs.compoundMap->size());
      json += "},{\"type\":\"pronounce\",\"count\":";
      json += std::to_string(refs.pronDict->entries.size());
      json += "},{\"type\":\"stress\",\"count\":";
      json += std::to_string(refs.stressDict->size());
      json += "}]";

      char* out = static_cast<char*>(std::malloc(json.size() + 1));
      if (!out) return nullptr;
      std::memcpy(out, json.c_str(), json.size() + 1);
      return out;
    }

    // Rebuild cache if invalid, sub-type changed, or language changed.
    auto refs = getDictRefs(h, lang);
    std::string effectiveSubType = subType.empty() ? "" : subType;
    if (!h->dataCache.dictionaryValid || h->dataCache.dictionarySubType != effectiveSubType ||
        h->dataCache.dictionaryLangTag != lang) {
      if (subType == "stress") tgsb_data::buildStressDictCache(h->dataCache, *refs.stressDict);
      else if (subType == "compound") tgsb_data::buildCompoundDictCache(h->dataCache, *refs.compoundMap);
      else if (subType == "character") tgsb_data::buildCharacterDictCache(h->dataCache, *refs.letterDict);
      else tgsb_data::buildDictionaryCache(h->dataCache, *refs.pronDict);
      h->dataCache.dictionarySubType = effectiveSubType;
      h->dataCache.dictionaryLangTag = lang;
    }

    std::string json = tgsb_data::serializeDictionaryJson(h->dataCache, offset, limit, search);
    if (json.empty()) return nullptr;

    char* out = static_cast<char*>(std::malloc(json.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, json.c_str(), json.size() + 1);
    return out;
  }

  if (domain == NVSP_DATA_FRAMETRACE) {
    // Serialize the per-utterance phoneme→frame-index map as a JSON array.
    // IPA keys are UTF-8 and do not contain JSON-escape-requiring characters
    // (", \, or control chars), so naive concatenation is safe here.
    const auto& trace = h->frameTrace;
    const int total = static_cast<int>(trace.size());
    int start = offset < 0 ? 0 : offset;
    if (start > total) start = total;
    int end = (limit <= 0) ? total : (start + limit);
    if (end > total) end = total;

    std::string json = "[";
    for (int i = start; i < end; ++i) {
      if (i > start) json += ',';
      json += "{\"frameIndex\":";
      json += std::to_string(trace[i].frameIndex);
      json += ",\"phonemeKey\":\"";
      json += trace[i].phonemeKey;
      json += "\"}";
    }
    json += ']';

    char* out = static_cast<char*>(std::malloc(json.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, json.c_str(), json.size() + 1);
    return out;
  }

  // Unsupported domain.
  return nullptr;
}

NVSP_FRONTEND_API int nvspFrontend_setData(
  nvspFrontend_handle_t handle,
  int domain,
  const char* langTagUtf8,
  const char* keyUtf8,
  const char* valueUtf8
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h || !langTagUtf8 || !keyUtf8 || !keyUtf8[0]) return 0;
  if (!valueUtf8) valueUtf8 = "";

  std::lock_guard<std::mutex> lock(h->mu);

  if (domain == NVSP_DATA_SETTINGS) {
    if (!langTagUtf8[0]) return 0;  // settings requires a lang tag
    const std::string lang(langTagUtf8);
    const std::string key(keyUtf8);
    const std::string value(valueUtf8);

    // Build a YAML snippet "key: value\n" and apply via existing path.
    // Only effective if langTag matches the currently loaded language.
    if (h->packLoaded && h->langTag == lang) {
      std::string snippet = key + ": " + value + "\n";
      if (!applySettingOverrides(h->pack.lang, snippet)) {
        setError(h, "Failed to apply setting override");
        return 0;
      }
    }

    h->dataCache.invalidate();
    return 1;
  }

  if (domain == NVSP_DATA_PHONEMES) {
    const std::string dotKey(keyUtf8);   // e.g. "ipa.cf2"
    const std::string value(valueUtf8);

    // Parse "phonemeKey.fieldName" (first dot splits phoneme from field).
    // Handle frameEx: "ipa.frameEx.breathiness" -> phoneme="ipa", field path.
    auto firstDot = dotKey.find('.');
    if (firstDot == std::string::npos) return 0;

    std::string phonemeKeyUtf8 = dotKey.substr(0, firstDot);
    std::string fieldPath = dotKey.substr(firstDot + 1);  // "cf2" or "frameEx.breathiness"
    if (phonemeKeyUtf8.empty() || fieldPath.empty()) return 0;

    // Apply in-memory override to the loaded pack if applicable.
    if (h->packLoaded) {
      std::u32string phonU32 = utf8ToU32(phonemeKeyUtf8);
      auto it = h->pack.phonemes.find(phonU32);
      if (it == h->pack.phonemes.end()) {
#ifdef __ANDROID__
        // Phoneme key not found in loaded pack.
#endif
        h->dataCache.invalidate();
        return 0;  // phoneme not found -- fail explicitly
      }
      {
        PhonemeDef& def = it->second;

        // Check if it's a frameEx sub-field.
        if (fieldPath.substr(0, 8) == "frameEx.") {
          std::string fxField = fieldPath.substr(8);
          double num = parseDouble(value);
          if (fxField == "creakiness")  { def.hasCreakiness = true; def.creakiness = num; }
          else if (fxField == "breathiness") { def.hasBreathiness = true; def.breathiness = num; }
          else if (fxField == "jitter")      { def.hasJitter = true; def.jitter = num; }
          else if (fxField == "shimmer")     { def.hasShimmer = true; def.shimmer = num; }
          else if (fxField == "sharpness")   { def.hasSharpness = true; def.sharpness = num; }
          else if (fxField == "endCf1")      { def.hasEndCf1 = true; def.endCf1 = num; }
          else if (fxField == "endCf2")      { def.hasEndCf2 = true; def.endCf2 = num; }
          else if (fxField == "endCf3")      { def.hasEndCf3 = true; def.endCf3 = num; }
          else if (fxField == "endPf1")      { def.hasEndPf1 = true; def.endPf1 = num; }
          else if (fxField == "endPf2")      { def.hasEndPf2 = true; def.endPf2 = num; }
          else if (fxField == "endPf3")      { def.hasEndPf3 = true; def.endPf3 = num; }
        }
        // Check flag fields.
        else if (!fieldPath.empty() && fieldPath[0] == '_') {
          bool b = (value == "true" || value == "1");
          uint32_t bit = 0;
          if (fieldPath == "_isAffricate")  bit = kIsAfricate;
          else if (fieldPath == "_isLiquid")     bit = kIsLiquid;
          else if (fieldPath == "_isNasal")      bit = kIsNasal;
          else if (fieldPath == "_isSemivowel")  bit = kIsSemivowel;
          else if (fieldPath == "_isStop")       bit = kIsStop;
          else if (fieldPath == "_isTap")        bit = kIsTap;
          else if (fieldPath == "_isTrill")      bit = kIsTrill;
          else if (fieldPath == "_isVoiced")     bit = kIsVoiced;
          else if (fieldPath == "_isVowel")      bit = kIsVowel;
          else if (fieldPath == "_copyAdjacent") bit = kCopyAdjacent;
          if (bit) {
            if (b) def.flags |= bit;
            else   def.flags &= ~bit;
          }
        }
        // Check micro-event fields.
        else if (fieldPath == "burstDurationMs") {
          def.hasBurstDurationMs = true; def.burstDurationMs = parseDouble(value);
        } else if (fieldPath == "burstDecayRate") {
          def.hasBurstDecayRate = true; def.burstDecayRate = parseDouble(value);
        } else if (fieldPath == "burstSpectralTilt") {
          def.hasBurstSpectralTilt = true; def.burstSpectralTilt = parseDouble(value);
        } else if (fieldPath == "voiceBarAmplitude") {
          def.hasVoiceBarAmplitude = true; def.voiceBarAmplitude = parseDouble(value);
        } else if (fieldPath == "voiceBarF1") {
          def.hasVoiceBarF1 = true; def.voiceBarF1 = parseDouble(value);
        } else if (fieldPath == "releaseSpreadMs") {
          def.hasReleaseSpreadMs = true; def.releaseSpreadMs = parseDouble(value);
        } else if (fieldPath == "fricAttackMs") {
          def.hasFricAttackMs = true; def.fricAttackMs = parseDouble(value);
        } else if (fieldPath == "fricDecayMs") {
          def.hasFricDecayMs = true; def.fricDecayMs = parseDouble(value);
        } else if (fieldPath == "durationScale") {
          def.hasDurationScale = true; def.durationScale = parseDouble(value);
        }
        // Frame fields (FieldId-based).
        else {
          FieldId fid;
          if (parseFieldId(fieldPath, fid)) {
            int idx = static_cast<int>(fid);
            if (idx >= 0 && idx < kFrameFieldCount) {
              double newVal = parseDouble(value);
              def.field[idx] = newVal;
              def.setMask |= (1ull << idx);
            }
          }
        }
      }
    }

    h->dataCache.invalidate();
    return 1;
  }

  if (domain == NVSP_DATA_DICTIONARY) {
    std::string subType, lang, search;
    parseDictLangTag(langTagUtf8, subType, lang, search);

    // "config" sub-type: enable/disable dict types per language.
    // key = dict type ("stress", "compound", "pronounce", "character")
    // value = "true" (enable) or "false" (disable)
    if (subType == "config") {
      const std::string dictType(keyUtf8);
      const std::string val(valueUtf8);
      if (val == "false") {
        h->disabledDictTypes[lang].insert(dictType);
      } else {
        auto it = h->disabledDictTypes.find(lang);
        if (it != h->disabledDictTypes.end()) {
          it->second.erase(dictType);
          if (it->second.empty()) h->disabledDictTypes.erase(it);
        }
      }
      return 1;
    }

    const std::string fromText(keyUtf8);
    const std::string value(valueUtf8);

    // Use original case for key — dictReplaceInText tries exact case
    // first, then lowercase fallback.
    const std::string& lk = fromText;

    if (subType == "character") {
      if (value.empty()) {
        h->pack.letterDict.erase(lk);
      } else {
        // Value is description string or JSON {"toText":"..."}.
        std::string desc = value;
        auto pos = value.find("\"toText\":");
        if (pos != std::string::npos) {
          auto sq = value.find('"', pos + 9);
          if (sq != std::string::npos) {
            auto eq = value.find('"', sq + 1);
            if (eq != std::string::npos) {
              desc = value.substr(sq + 1, eq - sq - 1);
            }
          }
        }
        h->pack.letterDict[lk] = std::move(desc);
      }
      h->dataCache.dictionaryValid = false;
      return 1;
    }
    if (subType == "stress") {
      if (value.empty()) {
        h->pack.stressDict.erase(lk);
      } else {
        // Parse space-separated digits: "1 0 2" -> {1, 0, 2}
        std::vector<int> digits;
        std::istringstream iss(value);
        iss.imbue(std::locale::classic());
        int d;
        while (iss >> d) digits.push_back(d);
        if (digits.empty()) return 0;
        h->pack.stressDict[lk] = std::move(digits);
      }
      h->dataCache.dictionaryValid = false;
      return 1;
    }

    if (subType == "compound") {
      if (value.empty()) {
        h->pack.compoundMap.erase(lk);
      } else {
        // Parse space-separated parts: "lock box" -> {"lock", "box"}
        std::vector<std::string> parts;
        std::istringstream iss(value);
        std::string part;
        while (iss >> part) parts.push_back(std::move(part));
        if (parts.empty()) return 0;
        h->pack.compoundMap[lk] = std::move(parts);
      }
      h->dataCache.dictionaryValid = false;
      return 1;
    }

    // Default: pronDict (backward compatible).
    if (value.empty()) {
      // Delete entry.
      h->pack.pronDict.entries.erase(lk);
    } else {
      // Parse JSON value: {"toText":"...", "fromIpa":"...", "toIpa":"...",
      //                     "category":"...", "masked":false}
      // Simple extraction -- find quoted values after known keys.
      auto extractStr = [&](const char* field) -> std::string {
        auto pos = value.find(field);
        if (pos == std::string::npos) return "";
        pos = value.find('"', pos + std::strlen(field));
        if (pos == std::string::npos) return "";
        ++pos;  // skip opening quote
        auto end = value.find('"', pos);
        if (end == std::string::npos) return "";
        return value.substr(pos, end - pos);
      };

      DictEntry e;
      e.fromText = fromText;
      e.toText   = extractStr("\"toText\":");
      e.fromIpa  = extractStr("\"fromIpa\":");
      e.toIpa    = extractStr("\"toIpa\":");
      e.category = extractStr("\"category\":");
      e.source   = "user";
      e.masked   = (value.find("\"masked\":true") != std::string::npos);

      // If toText is empty but masked is set, update mask on existing entry.
      if (e.toText.empty() && e.masked) {
        auto it = h->pack.pronDict.entries.find(lk);
        if (it != h->pack.pronDict.entries.end()) {
          it->second.masked = true;
          h->dataCache.dictionaryValid = false;
          return 1;
        }
        return 0;
      }
      // Unmask: masked=false with empty toText.
      if (e.toText.empty() && !e.masked &&
          value.find("\"masked\":false") != std::string::npos) {
        auto it = h->pack.pronDict.entries.find(lk);
        if (it != h->pack.pronDict.entries.end()) {
          it->second.masked = false;
          h->dataCache.dictionaryValid = false;
          return 1;
        }
        return 0;
      }

      if (e.toText.empty()) return 0;  // toText is required for new entries

      h->pack.pronDict.entries[lk] = std::move(e);
    }

    h->dataCache.dictionaryValid = false;
    return 1;
  }

  // Unsupported domain.
  return 0;
}

} // extern "C"
