#ifndef NVSP_FRONTEND_PACK_H
#define NVSP_FRONTEND_PACK_H

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "yaml_min.h"
#include "utf8.h"

namespace nvsp_frontend {

// Field count matches nvspFrontend_Frame in nvspFrontend.h (47 doubles).
constexpr int kFrameFieldCount = 47;

// Frame field IDs by index (must match struct order).
enum class FieldId : int {
  voicePitch = 0,
  vibratoPitchOffset = 1,
  vibratoSpeed = 2,
  voiceTurbulenceAmplitude = 3,
  glottalOpenQuotient = 4,
  voiceAmplitude = 5,
  aspirationAmplitude = 6,
  cf1 = 7,
  cf2 = 8,
  cf3 = 9,
  cf4 = 10,
  cf5 = 11,
  cf6 = 12,
  cfN0 = 13,
  cfNP = 14,
  cb1 = 15,
  cb2 = 16,
  cb3 = 17,
  cb4 = 18,
  cb5 = 19,
  cb6 = 20,
  cbN0 = 21,
  cbNP = 22,
  caNP = 23,
  fricationAmplitude = 24,
  pf1 = 25,
  pf2 = 26,
  pf3 = 27,
  pf4 = 28,
  pf5 = 29,
  pf6 = 30,
  pb1 = 31,
  pb2 = 32,
  pb3 = 33,
  pb4 = 34,
  pb5 = 35,
  pb6 = 36,
  pa1 = 37,
  pa2 = 38,
  pa3 = 39,
  pa4 = 40,
  pa5 = 41,
  pa6 = 42,
  parallelBypass = 43,
  preFormantGain = 44,
  outputGain = 45,
  endVoicePitch = 46,
};

// Phoneme flags (based on data.py keys).
enum PhonemeFlagBits : std::uint32_t {
  kIsAfricate   = 1u << 0,
  kIsLiquid     = 1u << 1,
  kIsNasal      = 1u << 2,
  kIsSemivowel  = 1u << 3,
  kIsStop       = 1u << 4,
  kIsTap        = 1u << 5,
  kIsTrill      = 1u << 6,
  kIsVoiced     = 1u << 7,
  kIsVowel      = 1u << 8,
  kCopyAdjacent = 1u << 9,
};

struct PhonemeDef {
  std::u32string key;
  std::uint32_t flags = 0;
  // Which frame fields are explicitly specified in YAML.
  std::uint64_t setMask = 0;
  double field[kFrameFieldCount] = {0.0};
};

// In YAML we keep replacements in UTF-8; we convert to UTF-32 during load.
struct RuleWhen {
  bool atWordStart = false;
  bool atWordEnd = false;
  std::string beforeClass; // name from classes
  std::string afterClass;
};

struct ReplacementRule {
  std::u32string from;
  std::vector<std::u32string> to; // first supported one is chosen
  RuleWhen when;
};

struct TransformRule {
  // Match on phoneme properties.
  // Any field left as -1 means "don't care".
  int isVowel = -1;
  int isVoiced = -1;
  int isStop = -1;
  int isAfricate = -1;
  int isNasal = -1;
  int isLiquid = -1;
  int isSemivowel = -1;
  int isTap = -1;
  int isTrill = -1;
  int isFricativeLike = -1; // derived from fricationAmplitude

  // Operations on frame fields.
  std::unordered_map<FieldId, double> set;
  std::unordered_map<FieldId, double> scale;
  std::unordered_map<FieldId, double> add;
};

struct IntonationClause {
  // Percent values, same semantics as ipa_convert.py.
  int preHeadStart = 46;
  int preHeadEnd = 57;
  int headExtendFrom = 4;
  int headStart = 80;
  int headEnd = 50;
  std::vector<int> headSteps;
  int headStressEndDelta = -16;
  int headUnstressedRunStartDelta = -8;
  int headUnstressedRunEndDelta = -5;
  int nucleus0Start = 64;
  int nucleus0End = 8;
  int nucleusStart = 70;
  int nucleusEnd = 18;
  int tailStart = 24;
  int tailEnd = 8;
};

struct LanguagePack {
  std::string langTag; // normalized (lowercase, '-')

  // Settings / knobs.
  double primaryStressDiv = 1.4;
  double secondaryStressDiv = 1.1;

  bool postStopAspirationEnabled = false;
  std::u32string postStopAspirationPhoneme = U"h";

  // Stop closure insertion.
  // "always", "vowel-and-cluster", "after-vowel", "none"
  std::string stopClosureMode = "vowel-and-cluster";
  bool stopClosureClusterGapsEnabled = true;

  // Segment/utterance stitching.
  // NVDA often supplies UI speech in multiple chunks (label/role/value/etc.).
  // When chunks are synthesized separately, phonological context is lost at the
  // boundary and some languages can sound "glued" together. These optional
  // values insert a tiny silence frame BETWEEN consecutive nvspFrontend_queueIPA
  // calls on the same handle.
  //
  // Units are milliseconds at speed=1.0; the frontend divides by speed.
  // Set to 0 to disable.
  double segmentBoundaryGapMs = 0.0;
  double segmentBoundaryFadeMs = 0.0;

  // If true, allow inserting short closure gaps before stops even after nasals.
  // This helps keep stops audible in nasal+stop clusters (e.g. Hungarian "pont" -> n+t).
  // Default false to avoid "clicks" in languages where this sounds unnatural.
  bool stopClosureAfterNasalsEnabled = false;

  // Duration scaling.
  double lengthenedScale = 1.05;
  double lengthenedScaleHu = 1.3;
  bool applyLengthenedScaleToVowelsOnly = true;

  // Language-specific duration tweaks.
  bool huShortAVowelEnabled = true;
  std::u32string huShortAVowelKey = U"ᴒ";
  double huShortAVowelScale = 0.85;

  bool englishLongUShortenEnabled = true;
  std::u32string englishLongUKey = U"u";
  double englishLongUWordFinalScale = 0.80;

  // Output frame defaults.
  double defaultPreFormantGain = 1.0;
  double defaultOutputGain = 1.5;

  // Non-acoustic voice defaults (safe to keep at 0, but configurable).
  double defaultVibratoPitchOffset = 0.0;
  double defaultVibratoSpeed = 0.0;
  double defaultVoiceTurbulenceAmplitude = 0.0;
  double defaultGlottalOpenQuotient = 0.0;

  // Normalization pipeline.
  bool stripAllophoneDigits = true;
  bool stripHyphen = true;

  std::unordered_map<std::u32string, std::u32string> aliases;
  std::vector<ReplacementRule> preReplacements;
  std::vector<ReplacementRule> replacements;
  std::unordered_map<std::string, std::vector<std::u32string>> classes;

  // Transforms applied to phoneme field values after correction.
  std::vector<TransformRule> transforms;

  // Intonation by clause punctuation.
  std::unordered_map<char, IntonationClause> intonation;

  // Tonal support.
  bool tonal = false;
  // Map tone string (e.g. "1", "˥˩") -> contour points (percent values).
  std::unordered_map<std::u32string, std::vector<int>> toneContours;
  bool toneDigitsEnabled = true; // allow 1-5 as tone markers
  // If true: contour points are absolute 0..100 values (same scale as intonation).
  // If false: contour points are treated as offsets from the current syllable baseline.
  bool toneContoursAbsolute = true;
};

struct PackSet {
  std::unordered_map<std::u32string, PhonemeDef> phonemes;
  LanguagePack lang;
};

// Load phonemes.yaml + merged language packs.
// packDir is the directory that contains "packs".
// Returns true on success.
bool loadPackSet(
  const std::string& packDir,
  const std::string& langTag,
  PackSet& out,
  std::string& outError
);

// Utility: does this pack contain a phoneme key?
bool hasPhoneme(const PackSet& pack, const std::u32string& key);

// Map a frame field name (e.g. "cf1") to FieldId. Returns true on success.
bool parseFieldId(const std::string& name, FieldId& out);

} // namespace nvsp_frontend

#endif
