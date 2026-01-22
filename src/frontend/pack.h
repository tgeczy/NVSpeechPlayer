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
  // Legacy pitch mode.
  //
  // When enabled, the frontend uses the older time-based pitch curve math
  // from the ee80f4d-era ipa.py (sometimes referred to as ipa-older.py).
  // This tends to sound more like classic screen-reader prosody at higher
  // rates than the newer table-based intonation model.
  bool legacyPitchMode = false;

  // Optional scaling applied to the caller-provided inflection (0..1) when
  // legacyPitchMode is enabled.
  //
  // Historical NVSpeechPlayer defaults used a lower inflection setting (e.g. 35)
  // than modern defaults (e.g. 60). Feeding those higher values into the legacy
  // math can sound overly excited.
  //
  // 1.0 preserves the historical behavior exactly.
  // A value around 0.58 maps 0.60 -> 0.35.
  double legacyPitchInflectionScale = 0.58;

  bool postStopAspirationEnabled = false;
  std::u32string postStopAspirationPhoneme = U"h";

  // Stop closure insertion.
  // "always", "vowel-and-cluster", "after-vowel", "none"
  std::string stopClosureMode = "vowel-and-cluster";
  bool stopClosureClusterGapsEnabled = true;

  // If true, allow inserting short closure gaps before stops even after nasals.
  // This helps keep stops audible in nasal+stop clusters (e.g. Hungarian "pont" -> n+t).
  // Default false to avoid "clicks" in languages where this sounds unnatural.
  bool stopClosureAfterNasalsEnabled = false;

  // Stop closure timing (ms at speed=1.0; divided by current speed).
  // These control the duration/fade of inserted silence frames (preStopGap)
  // before stops/affricates.
  double stopClosureVowelGapMs = 41.0;
  double stopClosureVowelFadeMs = 10.0;
  double stopClosureClusterGapMs = 22.0;
  double stopClosureClusterFadeMs = 4.0;

  // Optional: if >0, override cluster gap timing specifically at word boundaries
  // (when the stop/affricate is at word start).
  double stopClosureWordBoundaryClusterGapMs = 0.0;
  double stopClosureWordBoundaryClusterFadeMs = 0.0;

  // Segment boundary timing (ms at speed=1.0; divided by current speed).
  // If non-zero, the frontend inserts a tiny silence frame between consecutive
  // nvspFrontend_queueIPA calls on the same handle. This helps when callers
  // stitch UI speech from multiple chunks (label / role / value), where
  // transitions can otherwise feel abrupt in some languages.
  double segmentBoundaryGapMs = 0.0;
  double segmentBoundaryFadeMs = 0.0;
  // If true (default), suppress the segmentBoundary* silence insertion when the
  // previous chunk ends with a vowel/semivowel and the next chunk starts with a
  // vowel/semivowel. This avoids audible "holes" in vowel-to-vowel transitions
  // (e.g. diphthongs across chunks).
  bool segmentBoundarySkipVowelToVowel = true;

  // If true, also suppress the segmentBoundary* silence insertion when the
  // previous chunk ends with a vowel/semivowel and the next chunk starts with a
  // liquid-like consonant (liquids/taps/trills).
  //
  // This helps avoid audible seams in cases like vowel+R transitions across
  // chunks (e.g. "play" + "er"), where a tiny boundary pause can sound like an
  // unnatural syllable break.
  //
  // Disabled by default to preserve existing behavior.
  bool segmentBoundarySkipVowelToLiquid = false;

  // Automatic diphthong handling (optional).
  //
  // eSpeak usually encodes diphthongs as a sequence of two vowel qualities,
  // sometimes with a tie bar (U+0361). If tie bars are absent, the frontend
  // can optionally treat some vowel+vowel sequences as a diphthong by marking
  // them as tied internally. This makes the second vowel behave like a short
  // offglide without requiring a dedicated diphthong phoneme in phonemes.yaml.
  //
  // This is off by default to avoid changing languages that rely on vowel
  // hiatus (two syllables). Enable per-language in packs if desired.
  bool autoTieDiphthongs = false;
  // If true, and autoTieDiphthongs is enabled, try to replace the offglide
  // vowel with a semivowel (e.g. i/ɪ -> j, u/ʊ -> w) when those phonemes exist
  // in phonemes.yaml. This can make diphthong movement more obvious.
  bool autoDiphthongOffglideToSemivowel = false;

  // Semivowel offglide shortening (optional).
  //
  // In some packs (notably English), diphthongs are rendered as vowel+semivowel
  // sequences (e.g. eɪ -> ej). When that offglide semivowel is followed by a
  // vowel or a liquid-like consonant (e.g. "player" /plˈejɚ/, "later" /lˈejɾɚ/),
  // giving the semivowel a full consonant duration can sound like a tiny
  // syllable break.
  //
  // If semivowelOffglideScale != 1.0, the engine multiplies the duration/fade
  // of semivowel tokens that occur *within a word* in the pattern:
  //   vowel + semivowel + (vowel | liquid | tap | trill)
  //
  // Default 1.0 = disabled.
  double semivowelOffglideScale = 1.0;

  // Trill amplitude modulation (optional).
  //
  // Some languages use a true trill for /r/. Formant synthesis can render this
  // more naturally when we modulate the trill's voicing rather than relying on
  // pack-level hacks (e.g. duplicating 'r' tokens).
  //
  // When trillModulationMs > 0, the frontend will render `_isTrill` phonemes as
  // a series of micro-frames that apply an amplitude modulation to voiceAmplitude.
  //
  // - trillModulationMs: modulation cycle duration in milliseconds.
  // - trillModulationFadeMs: fade duration (ms) between micro-frames (0 = auto).
  //
  // NOTE: These values are treated as absolute milliseconds (not scaled by speed).
  double trillModulationMs = 0.0;
  double trillModulationFadeMs = 0.0;

  // Intra-word vowel hiatus break (optional).
  //
  // If enabled (>0), insert a short silence between two adjacent vowels
  // when the second vowel is explicitly stressed (ˈ or ˌ) and both
  // vowels are within the same word.
  //
  // This mainly helps spelled-out acronyms (e.g. NVDA -> ... iːˈeɪ)
  // where the vowel-to-vowel transition can otherwise sound like an extra glide.
  //
  // Disabled by default.
  double stressedVowelHiatusGapMs = 0.0;
  double stressedVowelHiatusFadeMs = 0.0;

  // Spelling diphthong handling (optional).
  //
  // Some eSpeak IPA outputs for spelled-out acronyms run letter names together
  // (e.g. NVDA -> ˌɛnvˌiːdˌiːˈeɪ). If the letter name contains an explicit
  // diphthong, the vowel movement can sound like an extra glide when it follows
  // another vowel.
  //
  // When spellingDiphthongMode is set to 'monophthong', the frontend will
  // selectively render certain letter-name diphthongs (currently just English
  // letter 'A' /eɪ/) as a long monophthong in acronym-like words, reducing the
  // audible 'y' glide without inserting a pause.
  //
  // Allowed values: 'none' (default), 'monophthong'.
  std::string spellingDiphthongMode = "none";

  // Duration scaling.
  double lengthenedScale = 1.05;
  double lengthenedScaleHu = 1.3;
  bool applyLengthenedScaleToVowelsOnly = true;

  // Optional: additional scaling for lengthened vowels (ː) when they occur in a
  // final closed syllable.
  //
  // Specifically, this multiplier is applied when:
  //   - the current token is a vowel with a length mark (ː)
  //   - the vowel is followed (within the same word) by one or more consonants
  //   - there are no later vowel nuclei before the next word boundary
  //
  // This gives packs a clean way to slightly shorten long vowels before
  // word-final consonant clusters (e.g. "rules" /ruːlz/) without affecting
  // open-syllable cases like "ruler" /ruːlə/.
  //
  // Default 1.0 = disabled.
  double lengthenedVowelFinalCodaScale = 1.0;

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
