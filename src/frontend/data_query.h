/*
TGSpeechBox — Generic data query layer.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.

Provides typed, paginated access to pack settings, phoneme data, and
(future) dictionary data without re-reading YAML from disk on every call.
*/

#ifndef TGSB_DATA_QUERY_H
#define TGSB_DATA_QUERY_H

#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations — avoid pulling in pack.h.
namespace nvsp_frontend {
struct LanguagePack;
struct PronDict;
}

namespace tgsb_data {

// ── Domain IDs (must match NVSP_DATA_* defines in nvspFrontend.h) ──
constexpr int kDomainSettings   = 0;
constexpr int kDomainPhonemes   = 1;
constexpr int kDomainDictionary = 2;
constexpr int kDomainFrameTrace = 3;  // per-utterance phoneme→frame-index map
constexpr int kDomainPassTrace  = 4;  // per-utterance per-pass token snapshots

// ── Field types ──
enum class FieldType { Float, Bool, String };

// ── Cached setting record ──
struct SettingRecord {
  std::string key;          // dot-notation key (e.g. "boundarySmoothing.enabled")
  FieldType   type;
  std::string value;        // stringified value
  std::string group;        // first dot-segment, or "" for top-level
};

// ── Cached phoneme record ──
// Each field within a phoneme is a separate record.
// key: "phoneme.fieldName" (e.g. "ɪ.cf2", "h.frameEx.breathiness")
// group: the phoneme key (e.g. "ɪ", "h")
// For the lang-filtered view, mappingFrom is set (e.g. "ɜː" for a "ɜː→ɝː" rule).
struct PhonemeRecord {
  std::string key;          // "phonemeKey.fieldName"
  FieldType   type;
  std::string value;        // stringified value
  std::string group;        // phoneme IPA key
  std::string phonemeClass; // "vowel", "stop", "fricative", etc.
  std::string mappingFrom;  // non-empty only in lang-filtered view
};

// ── Cached dictionary record ──
struct DictRecord {
  std::string key;       // fromText (case-preserved for display)
  std::string toText;
  std::string fromIpa;
  std::string toIpa;
  std::string category;
  std::string source;    // "main" or "user"
  bool masked = false;
};

// ── Frame-trace entry ──
// One entry per phoneme per utterance. Emitted by frame_emit at the start
// of each Token, capturing the output-frame index where that phoneme begins.
// Lets tests correlate a word-context emission back to individual phonemes
// without modifying the realtime FrameExCallback signature.
struct FrameTraceEntry {
  int frameIndex;          // output frame count at which this phoneme starts
  std::string phonemeKey;  // UTF-8 IPA key (e.g. "ɣ", "l", "a")
};

// ── Pass snapshot ──
// One entry per non-silence token per pass. Populated by pass_pipeline after
// each pass runs, capturing the Token's acoustic field state at that point
// in the pipeline. Lets tests observe how passes mutate phoneme parameters —
// critical for diagnosing word-context regressions where a distinction
// survives in isolation but gets compressed through the pass chain.
struct PassSnapshot {
  std::string passName;    // "coarticulation", "boundary_smoothing", etc.
  int tokenIndex;          // index of this token in the current tokens vector
  std::string phonemeKey;  // UTF-8 IPA key (empty if silence/gap — though silence is filtered out)
  // Acoustic field state after the pass ran.
  // Resolved from Token.field if setMask bit is set, else PhonemeDef default.
  double cf1, cf2, cf3;
  double pf1, pf2, pf3;
  double voiceAmplitude;
  double aspirationAmplitude;
  double fricationAmplitude;
  double durationMs;
  double fadeMs;
};

// ── Per-domain cache ──
struct DataCache {
  std::string langTag;
  std::vector<SettingRecord> settings;
  bool settingsValid = false;

  std::string phonemesLangTag;  // "" for all, lang tag for filtered
  std::vector<PhonemeRecord> phonemes;
  bool phonemesValid = false;

  std::vector<DictRecord> dictionary;
  bool dictionaryValid = false;
  std::string dictionarySubType;  // "", "stress", "compound", or "character"
  std::string dictionaryLangTag;  // which language the cache was built for

  void invalidate() {
    settingsValid = false;
    phonemesValid = false;
    dictionaryValid = false;
    dictionarySubType.clear();
    dictionaryLangTag.clear();
  }
};

// ── Cache builders ──

// Build settings cache from the pack YAML file chain.
// packDir: root dir containing packs/.  langTag: e.g. "en-us".
void buildSettingsCache(DataCache& cache,
                        const std::string& packDir,
                        const std::string& langTag);

// Build phonemes cache from phonemes.yaml.
// If langTag is empty: all phonemes from base phonemes.yaml.
// If langTag is non-empty: only phonemes referenced as replacement targets
// in that language's normalization.replacements, with mappingFrom populated.
void buildPhonemesCache(DataCache& cache,
                        const std::string& packDir,
                        const std::string& langTag);

// Build dictionary cache from an in-memory PronDict.
void buildDictionaryCache(DataCache& cache,
                          const nvsp_frontend::PronDict& dict);

// Build dictionary cache from an in-memory stress dictionary.
void buildStressDictCache(DataCache& cache,
                          const std::unordered_map<std::string, std::vector<int>>& stressDict);

// Build dictionary cache from an in-memory compound map.
void buildCompoundDictCache(DataCache& cache,
                            const std::unordered_map<std::string, std::vector<std::string>>& compoundMap);

// Build dictionary cache from an in-memory character/letter dict.
void buildCharacterDictCache(DataCache& cache,
                             const std::unordered_map<std::string, std::string>& letterDict);

// ── JSON serializers ──

// Serialize a slice of the settings cache to a JSON array string.
// offset/limit: pagination (limit=0 means all from offset).
std::string serializeSettingsJson(const DataCache& cache, int offset, int limit);

// Serialize a slice of the phonemes cache to a JSON array string.
std::string serializePhonemesJson(const DataCache& cache, int offset, int limit);

// Serialize a slice of the dictionary cache to a JSON array string.
// If search is non-empty, only entries whose key starts with search are included
// (case-insensitive prefix match). Pagination applies after filtering.
std::string serializeDictionaryJson(const DataCache& cache, int offset, int limit,
                                    const std::string& search = "");

// Count dictionary entries matching a search prefix (case-insensitive).
// If search is empty, returns total cache size.
int countDictionaryMatches(const DataCache& cache, const std::string& search);

// ── Type detection ──

// Infer FieldType from a scalar value string.
FieldType detectType(const std::string& value);

// Extract the group prefix from a dot-notation key.
// "boundarySmoothing.enabled" → "boundarySmoothing"
// "primaryStressDiv"          → ""
std::string extractGroup(const std::string& key);

} // namespace tgsb_data

#endif // TGSB_DATA_QUERY_H
