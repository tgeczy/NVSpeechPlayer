/*
TGSpeechBox — Language pack YAML parser.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include "pack.h"
#include "utf8.h"
#include "voice_profile.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace nvsp_frontend {

static bool parseBool(const std::string& s) {
  std::string t;
  t.reserve(s.size());
  for (char c : s) {
    t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (t == "1" || t == "true" || t == "yes" || t == "on") return true;
  if (t == "0" || t == "false" || t == "no" || t == "off") return false;
  return false;
}


static fs::path findPacksRoot(const std::string& packDir, std::string& outError) {
  fs::path p(packDir);
  fs::path direct = p / "phonemes.yaml";
  if (fs::exists(direct)) {
    return p;
  }
  fs::path nested = p / "packs" / "phonemes.yaml";
  if (fs::exists(nested)) {
    return p / "packs";
  }
  outError = "Could not find phonemes.yaml. Expected either: 'phonemes.yaml' or 'packs/phonemes.yaml' under: " + packDir;
  return fs::path();
}

bool parseFieldId(const std::string& name, FieldId& out) {
  // Keep this in sync with FieldId enum and nvspFrontend_Frame.
  // We only list the names that are expected to appear in YAML.
  if (name == "voicePitch") { out = FieldId::voicePitch; return true; }
  if (name == "vibratoPitchOffset") { out = FieldId::vibratoPitchOffset; return true; }
  if (name == "vibratoSpeed") { out = FieldId::vibratoSpeed; return true; }
  if (name == "voiceTurbulenceAmplitude") { out = FieldId::voiceTurbulenceAmplitude; return true; }
  if (name == "glottalOpenQuotient") { out = FieldId::glottalOpenQuotient; return true; }
  if (name == "voiceAmplitude") { out = FieldId::voiceAmplitude; return true; }
  if (name == "aspirationAmplitude") { out = FieldId::aspirationAmplitude; return true; }
  if (name == "cf1") { out = FieldId::cf1; return true; }
  if (name == "cf2") { out = FieldId::cf2; return true; }
  if (name == "cf3") { out = FieldId::cf3; return true; }
  if (name == "cf4") { out = FieldId::cf4; return true; }
  if (name == "cf5") { out = FieldId::cf5; return true; }
  if (name == "cf6") { out = FieldId::cf6; return true; }
  if (name == "cfN0") { out = FieldId::cfN0; return true; }
  if (name == "cfNP") { out = FieldId::cfNP; return true; }
  if (name == "cb1") { out = FieldId::cb1; return true; }
  if (name == "cb2") { out = FieldId::cb2; return true; }
  if (name == "cb3") { out = FieldId::cb3; return true; }
  if (name == "cb4") { out = FieldId::cb4; return true; }
  if (name == "cb5") { out = FieldId::cb5; return true; }
  if (name == "cb6") { out = FieldId::cb6; return true; }
  if (name == "cbN0") { out = FieldId::cbN0; return true; }
  if (name == "cbNP") { out = FieldId::cbNP; return true; }
  if (name == "caNP") { out = FieldId::caNP; return true; }
  if (name == "fricationAmplitude") { out = FieldId::fricationAmplitude; return true; }
  if (name == "pf1") { out = FieldId::pf1; return true; }
  if (name == "pf2") { out = FieldId::pf2; return true; }
  if (name == "pf3") { out = FieldId::pf3; return true; }
  if (name == "pf4") { out = FieldId::pf4; return true; }
  if (name == "pf5") { out = FieldId::pf5; return true; }
  if (name == "pf6") { out = FieldId::pf6; return true; }
  if (name == "pb1") { out = FieldId::pb1; return true; }
  if (name == "pb2") { out = FieldId::pb2; return true; }
  if (name == "pb3") { out = FieldId::pb3; return true; }
  if (name == "pb4") { out = FieldId::pb4; return true; }
  if (name == "pb5") { out = FieldId::pb5; return true; }
  if (name == "pb6") { out = FieldId::pb6; return true; }
  if (name == "pa1") { out = FieldId::pa1; return true; }
  if (name == "pa2") { out = FieldId::pa2; return true; }
  if (name == "pa3") { out = FieldId::pa3; return true; }
  if (name == "pa4") { out = FieldId::pa4; return true; }
  if (name == "pa5") { out = FieldId::pa5; return true; }
  if (name == "pa6") { out = FieldId::pa6; return true; }
  if (name == "parallelBypass") { out = FieldId::parallelBypass; return true; }
  if (name == "preFormantGain") { out = FieldId::preFormantGain; return true; }
  if (name == "outputGain") { out = FieldId::outputGain; return true; }
  if (name == "endVoicePitch") { out = FieldId::endVoicePitch; return true; }
  return false;
}

static std::uint32_t parseFlagKey(const std::string& key) {
  if (key == "_isAfricate") return kIsAfricate;
  if (key == "_isLiquid") return kIsLiquid;
  if (key == "_isNasal") return kIsNasal;
  if (key == "_isSemivowel") return kIsSemivowel;
  if (key == "_isStop") return kIsStop;
  if (key == "_isTap") return kIsTap;
  if (key == "_isTrill") return kIsTrill;
  if (key == "_isVoiced") return kIsVoiced;
  if (key == "_isVowel") return kIsVowel;
  if (key == "_copyAdjacent") return kCopyAdjacent;
  return 0;
}

static bool loadPhonemes(const fs::path& packsRoot, PackSet& out, std::string& outError) {
  const fs::path phonemesPath = packsRoot / "phonemes.yaml";
  yaml_min::Node root;
  std::string yamlErr;
  if (!yaml_min::loadFile(phonemesPath.string(), root, yamlErr)) {
    outError = yamlErr;
    return false;
  }

  const yaml_min::Node* phonemesNode = root.get("phonemes");
  if (!phonemesNode || !phonemesNode->isMap()) {
    outError = "phonemes.yaml must contain a top-level 'phonemes:' map";
    return false;
  }

  out.phonemes.clear();

  for (const auto& kv : phonemesNode->map) {
    const std::string& keyUtf8 = kv.first;
    const yaml_min::Node& defNode = kv.second;
    if (!defNode.isMap()) {
      continue;
    }

    PhonemeDef def;
    def.key = utf8ToU32(keyUtf8);

    // Parse fields.
    for (const auto& ekv : defNode.map) {
      const std::string& fieldName = ekv.first;
      const yaml_min::Node& val = ekv.second;

      if (!fieldName.empty() && fieldName[0] == '_') {
        std::uint32_t bit = parseFlagKey(fieldName);
        if (bit != 0) {
          bool b = false;
          if (val.asBool(b)) {
            if (b) def.flags |= bit;
          }
        }
        continue;
      }

      // Parse frameEx: block for per-phoneme voice quality
      // (same logic as in mergeLanguageFile for language pack overrides)
      if (fieldName == "frameEx" && val.isMap()) {
        for (const auto& fxkv : val.map) {
          const std::string& fxKey = fxkv.first;
          double fxVal;
          if (!fxkv.second.asNumber(fxVal)) continue;
          
          if (fxKey == "creakiness") {
            def.hasCreakiness = true;
            def.creakiness = fxVal;
          } else if (fxKey == "breathiness") {
            def.hasBreathiness = true;
            def.breathiness = fxVal;
          } else if (fxKey == "jitter") {
            def.hasJitter = true;
            def.jitter = fxVal;
          } else if (fxKey == "shimmer") {
            def.hasShimmer = true;
            def.shimmer = fxVal;
          } else if (fxKey == "sharpness") {
            def.hasSharpness = true;
            def.sharpness = fxVal;
          } else if (fxKey == "endCf1") {
            def.hasEndCf1 = true;
            def.endCf1 = fxVal;
          } else if (fxKey == "endCf2") {
            def.hasEndCf2 = true;
            def.endCf2 = fxVal;
          } else if (fxKey == "endCf3") {
            def.hasEndCf3 = true;
            def.endCf3 = fxVal;
          } else if (fxKey == "endPf1") {
            def.hasEndPf1 = true;
            def.endPf1 = fxVal;
          } else if (fxKey == "endPf2") {
            def.hasEndPf2 = true;
            def.endPf2 = fxVal;
          } else if (fxKey == "endPf3") {
            def.hasEndPf3 = true;
            def.endPf3 = fxVal;
          }
        }
        continue;
      }

      // Micro-event burst shaping fields (top-level, not frame fields)
      if (fieldName == "burstDurationMs") {
        double v; if (val.asNumber(v)) { def.hasBurstDurationMs = true; def.burstDurationMs = v; }
        continue;
      }
      if (fieldName == "burstDecayRate") {
        double v; if (val.asNumber(v)) { def.hasBurstDecayRate = true; def.burstDecayRate = v; }
        continue;
      }
      if (fieldName == "burstSpectralTilt") {
        double v; if (val.asNumber(v)) { def.hasBurstSpectralTilt = true; def.burstSpectralTilt = v; }
        continue;
      }
      if (fieldName == "voiceBarAmplitude") {
        double v; if (val.asNumber(v)) { def.hasVoiceBarAmplitude = true; def.voiceBarAmplitude = v; }
        continue;
      }
      if (fieldName == "voiceBarF1") {
        double v; if (val.asNumber(v)) { def.hasVoiceBarF1 = true; def.voiceBarF1 = v; }
        continue;
      }
      if (fieldName == "releaseSpreadMs") {
        double v; if (val.asNumber(v)) { def.hasReleaseSpreadMs = true; def.releaseSpreadMs = v; }
        continue;
      }
      if (fieldName == "fricAttackMs") {
        double v; if (val.asNumber(v)) { def.hasFricAttackMs = true; def.fricAttackMs = v; }
        continue;
      }
      if (fieldName == "fricDecayMs") {
        double v; if (val.asNumber(v)) { def.hasFricDecayMs = true; def.fricDecayMs = v; }
        continue;
      }
      if (fieldName == "durationScale") {
        double v; if (val.asNumber(v)) { def.hasDurationScale = true; def.durationScale = v; }
        continue;
      }
      if (fieldName == "closureGapMs") {
        double v; if (val.asNumber(v)) { def.hasClosureGapMs = true; def.closureGapMs = v; }
        continue;
      }
      // Higher cascade formants F7/F8 (DSP v8, FrameEx fields)
      if (fieldName == "cf7") {
        double v; if (val.asNumber(v)) { def.hasCf7 = true; def.cf7 = v; }
        continue;
      }
      if (fieldName == "cb7") {
        double v; if (val.asNumber(v)) { def.hasCb7 = true; def.cb7 = v; }
        continue;
      }
      if (fieldName == "cf8") {
        double v; if (val.asNumber(v)) { def.hasCf8 = true; def.cf8 = v; }
        continue;
      }
      if (fieldName == "cb8") {
        double v; if (val.asNumber(v)) { def.hasCb8 = true; def.cb8 = v; }
        continue;
      }

      FieldId id;
      if (!parseFieldId(fieldName, id)) {
        continue;
      }
      double num = 0.0;
      if (!val.asNumber(num)) {
        continue;
      }
      int idx = static_cast<int>(id);
      if (idx < 0 || idx >= kFrameFieldCount) {
        continue;
      }
      def.field[idx] = num;
      def.setMask |= (1ull << idx);
    }

    out.phonemes.emplace(def.key, def);
  }

  if (out.phonemes.empty()) {
    outError = "phonemes.yaml loaded but contained zero phonemes";
    return false;
  }

  // Parse optional voiceProfiles section.
  // This is an overlay system for producing different voice qualities
  // (e.g., female voice) without maintaining separate phoneme tables.
  const yaml_min::Node* voiceProfilesNode = root.get("voiceProfiles");
  if (voiceProfilesNode && voiceProfilesNode->isMap()) {
    auto profiles = std::make_unique<VoiceProfileSet>();
    std::string vpErr;
    if (!parseVoiceProfiles(*voiceProfilesNode, *profiles, vpErr)) {
      // Non-fatal: store warning and continue without voice profiles.
      // This helps debug "why does my profile do nothing?" issues.
      if (!out.loadWarnings.empty()) out.loadWarnings += "\n";
      out.loadWarnings += "voiceProfiles parse error: " + vpErr;
      out.voiceProfiles = nullptr;
    } else if (!profiles->profiles.empty()) {
      out.voiceProfiles = std::move(profiles);
    }
  }

  return true;
}

static void applyLanguageDefaults(LanguagePack& lp) {
  // Embed the ipa_convert.py defaults.
  // This keeps packs small: they only need to override what they care about.

  // Default intonation for '.', ',', '?', '!'
  {
    IntonationClause dot;
    dot.preHeadStart = 46;
    dot.preHeadEnd = 57;
    dot.headExtendFrom = 4;
    dot.headStart = 80;
    dot.headEnd = 50;
    dot.headSteps = {100,75,50,25,0,63,38,13,0};
    dot.headStressEndDelta = -16;
    dot.headUnstressedRunStartDelta = -8;
    dot.headUnstressedRunEndDelta = -5;
    dot.nucleus0Start = 64;
    dot.nucleus0End = 8;
    dot.nucleusStart = 70;
    dot.nucleusEnd = 18;
    dot.tailStart = 24;
    dot.tailEnd = 8;
    lp.intonation['.'] = dot;
  }
  {
    IntonationClause comma;
    comma.preHeadStart = 46;
    comma.preHeadEnd = 57;
    comma.headExtendFrom = 4;
    comma.headStart = 80;
    comma.headEnd = 60;
    comma.headSteps = {100,75,50,25,0,63,38,13,0};
    comma.headStressEndDelta = -16;
    comma.headUnstressedRunStartDelta = -8;
    comma.headUnstressedRunEndDelta = -5;
    comma.nucleus0Start = 34;
    comma.nucleus0End = 52;
    comma.nucleusStart = 78;
    comma.nucleusEnd = 34;
    comma.tailStart = 34;
    comma.tailEnd = 52;
    lp.intonation[','] = comma;
  }
  {
    IntonationClause q;
    q.preHeadStart = 45;
    q.preHeadEnd = 56;
    q.headExtendFrom = 3;
    q.headStart = 75;
    q.headEnd = 43;
    q.headSteps = {100,75,50,20,60,35,11,0};
    q.headStressEndDelta = -16;
    q.headUnstressedRunStartDelta = -7;
    q.headUnstressedRunEndDelta = 0;
    q.nucleus0Start = 34;
    q.nucleus0End = 68;
    q.nucleusStart = 86;
    q.nucleusEnd = 21;
    q.tailStart = 34;
    q.tailEnd = 68;
    lp.intonation['?'] = q;
  }
  {
    IntonationClause ex;
    ex.preHeadStart = 46;
    ex.preHeadEnd = 57;
    ex.headExtendFrom = 3;
    ex.headStart = 90;
    ex.headEnd = 50;
    ex.headSteps = {100,75,50,16,82,50,32,16};
    ex.headStressEndDelta = -16;
    ex.headUnstressedRunStartDelta = -9;
    ex.headUnstressedRunEndDelta = 0;
    ex.nucleus0Start = 92;
    ex.nucleus0End = 4;
    ex.nucleusStart = 92;
    ex.nucleusEnd = 80;
    ex.tailStart = 76;
    ex.tailEnd = 4;
    lp.intonation['!'] = ex;
  }
}

static void mergeSettings(LanguagePack& lp, const yaml_min::Node& settings) {
  if (!settings.isMap()) return;

  auto getNum = [&](const char* k, double& field) {
    const yaml_min::Node* n = settings.get(k);
    double v;
    if (n && n->asNumber(v)) field = v;
  };
  auto getBool = [&](const char* k, bool& field) {
    const yaml_min::Node* n = settings.get(k);
    bool v;
    if (n && n->asBool(v)) field = v;
  };
  auto getStr = [&](const char* k, std::string& field) {
    const yaml_min::Node* n = settings.get(k);
    if (n && n->isScalar()) field = n->scalar;
  };

  

// Helpers for nested setting blocks (maps) inside `settings:`.
auto getNumFrom = [&](const yaml_min::Node& map, const char* k, double& out) {
  const yaml_min::Node* n = map.get(k);
  if (n && n->isScalar()) {
    n->asNumber(out);
  }
};

auto getBoolFrom = [&](const yaml_min::Node& map, const char* k, bool& out) {
  const yaml_min::Node* n = map.get(k);
  if (n && n->isScalar()) {
    out = parseBool(n->scalar);
  }
};

auto getStrFrom = [&](const yaml_min::Node& map, const char* k, std::string& out) {
  const yaml_min::Node* n = map.get(k);
  if (n && n->isScalar()) out = n->scalar;
};

auto getStrListFrom = [&](const yaml_min::Node& map, const char* k, std::vector<std::string>& out) {
  const yaml_min::Node* n = map.get(k);
  if (!n) return;
  if (n->isSeq()) {
    std::vector<std::string> tmp;
    tmp.reserve(n->seq.size());
    for (const auto& el : n->seq) {
      if (el.isScalar()) tmp.push_back(el.scalar);
    }
    if (!tmp.empty()) out = tmp;
    return;
  }
  // Also accept a single scalar with comma-separated values.
  if (n->isScalar()) {
    std::vector<std::string> tmp;
    std::string s = n->scalar;
    size_t start = 0;
    while (start < s.size()) {
      size_t comma = s.find(',', start);
      std::string part = (comma == std::string::npos) ? s.substr(start) : s.substr(start, comma - start);
      // trim
      while (!part.empty() && (part.front()==' ' || part.front()=='\t')) part.erase(part.begin());
      while (!part.empty() && (part.back()==' ' || part.back()=='\t')) part.pop_back();
      if (!part.empty()) tmp.push_back(part);
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    if (!tmp.empty()) out = tmp;
  }
};
getNum("primaryStressDiv", lp.primaryStressDiv);
  getNum("secondaryStressDiv", lp.secondaryStressDiv);
  
  // Voice profile name (optional).
  // Set this to apply a voice profile to all phonemes.
  getStr("voiceProfileName", lp.voiceProfileName);

  // Legacy pitch mode (ported from the ee80f4d-era ipa.py / ipa-older.py).
  // Supports both legacy bool syntax and new string enum:
  //   legacyPitchMode: true          -> "legacy"
  //   legacyPitchMode: false         -> "espeak_style"
  //   legacyPitchMode: "legacy"      -> "legacy"
  //   legacyPitchMode: "espeak_style"-> "espeak_style"
  //   legacyPitchMode: "fujisaki_style" -> "fujisaki_style"
  {
    const yaml_min::Node* n = settings.get("legacyPitchMode");
    if (n && n->isScalar()) {
      const std::string& val = n->scalar;
      if (val == "true" || val == "1") {
        lp.legacyPitchMode = "legacy";
      } else if (val == "false" || val == "0") {
        lp.legacyPitchMode = "espeak_style";
      } else {
        // Direct string value: "legacy", "espeak_style", "fujisaki_style"
        lp.legacyPitchMode = val;
      }
    }
  }
  // Optional: scale applied to the caller-provided inflection (0..1) when legacyPitchMode is "legacy".
  getNum("legacyPitchInflectionScale", lp.legacyPitchInflectionScale);

  // Fujisaki pitch model parameters (used when legacyPitchMode = "fujisaki_style")
  getNum("fujisakiPhraseAmp", lp.fujisakiPhraseAmp);
  getNum("fujisakiPrimaryAccentAmp", lp.fujisakiPrimaryAccentAmp);
  getNum("fujisakiSecondaryAccentAmp", lp.fujisakiSecondaryAccentAmp);
  getStr("fujisakiAccentMode", lp.fujisakiAccentMode);
  getNum("fujisakiPhraseLen", lp.fujisakiPhraseLen);
  getNum("fujisakiAccentLen", lp.fujisakiAccentLen);
  getNum("fujisakiAccentDur", lp.fujisakiAccentDur);
  getNum("fujisakiDeclinationRate", lp.fujisakiDeclinationRate);
  getNum("fujisakiPitchCurveExponent", lp.fujisakiPitchCurveExponent);
  getNum("fujisakiMonoAccentDurScale", lp.fujisakiMonoAccentDurScale);
  getNum("fujisakiCompoundDeclinStep", lp.fujisakiCompoundDeclinStep);
  getNum("fujisakiPhraseDecay", lp.fujisakiPhraseDecay);  // deprecated, kept for YAML compat
  getNum("fujisakiDeclinationScale", lp.fujisakiDeclinationScale);
  getNum("fujisakiDeclinationMax", lp.fujisakiDeclinationMax);
  getNum("fujisakiDeclinationPostFloor", lp.fujisakiDeclinationPostFloor);

  // Fujisaki clause-type overrides (flat keys)
  getNum("fujisakiQuestionPhraseAmpScale",   lp.fujisakiQuestion.phraseAmpScale);
  getNum("fujisakiQuestionAccentBoost",      lp.fujisakiQuestion.accentBoost);
  getNum("fujisakiQuestionDeclinationScale", lp.fujisakiQuestion.declinationScale);
  getNum("fujisakiQuestionBasePitchScale",   lp.fujisakiQuestion.basePitchScale);
  getNum("fujisakiQuestionFinalRiseScale",   lp.fujisakiQuestion.finalRiseScale);
  getNum("fujisakiQuestionFinalDropScale",   lp.fujisakiQuestion.finalDropScale);

  getNum("fujisakiExclamationPhraseAmpScale",   lp.fujisakiExclamation.phraseAmpScale);
  getNum("fujisakiExclamationAccentBoost",      lp.fujisakiExclamation.accentBoost);
  getNum("fujisakiExclamationDeclinationScale", lp.fujisakiExclamation.declinationScale);
  getNum("fujisakiExclamationBasePitchScale",   lp.fujisakiExclamation.basePitchScale);
  getNum("fujisakiExclamationFinalRiseScale",   lp.fujisakiExclamation.finalRiseScale);
  getNum("fujisakiExclamationFinalDropScale",   lp.fujisakiExclamation.finalDropScale);

  getNum("fujisakiCommaPhraseAmpScale",   lp.fujisakiComma.phraseAmpScale);
  getNum("fujisakiCommaAccentBoost",      lp.fujisakiComma.accentBoost);
  getNum("fujisakiCommaDeclinationScale", lp.fujisakiComma.declinationScale);
  getNum("fujisakiCommaBasePitchScale",   lp.fujisakiComma.basePitchScale);
  getNum("fujisakiCommaFinalRiseScale",   lp.fujisakiComma.finalRiseScale);
  getNum("fujisakiCommaFinalDropScale",   lp.fujisakiComma.finalDropScale);

  // Impulse pitch model parameters (used when legacyPitchMode = "impulse_style")
  getNum("impulseDeclinationRangeHz", lp.impulseDeclinationRangeHz);
  getNum("impulseDeclinationHzPerSec", lp.impulseDeclinationHzPerSec);
  getNum("impulseRiseHz", lp.impulseRiseHz);
  getNum("impulseHatFallScale", lp.impulseHatFallScale);
  getNum("impulseFirstStressBoostHz", lp.impulseFirstStressBoostHz);
  getNum("impulseSecondStressBoostHz", lp.impulseSecondStressBoostHz);
  getNum("impulseThirdStressBoostHz", lp.impulseThirdStressBoostHz);
  getNum("impulseFourthStressBoostHz", lp.impulseFourthStressBoostHz);
  getNum("impulseStressGain", lp.impulseStressGain);
  getNum("impulseSecondaryStressScale", lp.impulseSecondaryStressScale);
  getNum("impulseTerminalStressHz", lp.impulseTerminalStressHz);
  getNum("impulseQuestionReduction", lp.impulseQuestionReduction);
  getNum("impulseTerminalFallHz", lp.impulseTerminalFallHz);
  getNum("impulseContinuationRiseHz", lp.impulseContinuationRiseHz);
  getNum("impulseQuestionRiseHz", lp.impulseQuestionRiseHz);
  getNum("impulseAssertiveness", lp.impulseAssertiveness);
  getNum("impulseSmoothAlpha", lp.impulseSmoothAlpha);

  // Klatt hat-pattern pitch model parameters (used when legacyPitchMode = "klatt_style")
  getNum("klattHatRiseHz", lp.klattHatRiseHz);
  getNum("klattStress1Hz", lp.klattStress1Hz);
  getNum("klattStress2Hz", lp.klattStress2Hz);
  getNum("klattStress3Hz", lp.klattStress3Hz);
  getNum("klattStress4Hz", lp.klattStress4Hz);
  getNum("klattDeclinationHzPerSec", lp.klattDeclinationHzPerSec);
  getNum("klattFinalFallBelowBaseHz", lp.klattFinalFallBelowBaseHz);
  getNum("klattQuestionRiseHz", lp.klattQuestionRiseHz);
  getNum("klattContinuationRiseHz", lp.klattContinuationRiseHz);
  getNum("klattGlottalLowerHz", lp.klattGlottalLowerHz);
  getNum("klattSmoothAlpha", lp.klattSmoothAlpha);

  getBool("postStopAspirationEnabled", lp.postStopAspirationEnabled);
  {
    const yaml_min::Node* n = settings.get("postStopAspirationPhoneme");
    if (n && n->isScalar()) lp.postStopAspirationPhoneme = utf8ToU32(n->scalar);
  }

  getStr("stopClosureMode", lp.stopClosureMode);
  getBool("stopClosureClusterGapsEnabled", lp.stopClosureClusterGapsEnabled);
  getBool("stopClosureAfterNasalsEnabled", lp.stopClosureAfterNasalsEnabled);

  // Stop closure timing (ms at speed=1.0; divided by current speed in the engine).
  getNum("stopClosureVowelGapMs", lp.stopClosureVowelGapMs);
  getNum("stopClosureVowelFadeMs", lp.stopClosureVowelFadeMs);
  getNum("stopClosureClusterGapMs", lp.stopClosureClusterGapMs);
  getNum("stopClosureClusterFadeMs", lp.stopClosureClusterFadeMs);
  getNum("stopClosureWordBoundaryClusterGapMs", lp.stopClosureWordBoundaryClusterGapMs);
  getNum("stopClosureWordBoundaryClusterFadeMs", lp.stopClosureWordBoundaryClusterFadeMs);
  getNum("stopClosureNasalToStopGapMs", lp.stopClosureNasalToStopGapMs);
  getNum("stopClosureNasalToStopFadeMs", lp.stopClosureNasalToStopFadeMs);

  // Segment boundary timing (ms at speed=1.0; divided by current speed).
  getNum("segmentBoundaryGapMs", lp.segmentBoundaryGapMs);
  getNum("segmentBoundaryFadeMs", lp.segmentBoundaryFadeMs);
  getBool("segmentBoundarySkipVowelToVowel", lp.segmentBoundarySkipVowelToVowel);
  getBool("segmentBoundarySkipVowelToLiquid", lp.segmentBoundarySkipVowelToLiquid);

  // Single-word utterance tuning (key echo / word-by-word reading).
  getBool("singleWordTuningEnabled", lp.singleWordTuningEnabled);
  getNum("singleWordFinalHoldMs", lp.singleWordFinalHoldMs);
  getNum("singleWordFinalLiquidHoldScale", lp.singleWordFinalLiquidHoldScale);
  getNum("singleWordFinalFadeMs", lp.singleWordFinalFadeMs);
  getNum("clauseFinalHoldMs", lp.clauseFinalHoldMs);
  getNum("clauseFinalFadeMs", lp.clauseFinalFadeMs);
  getBool("singleWordClauseTypeOverrideCommaOnly", lp.singleWordClauseTypeOverrideCommaOnly);
  {
    std::string ct;
    getStr("singleWordClauseTypeOverride", ct);
    if (!ct.empty()) lp.singleWordClauseTypeOverride = ct[0];
  }

  getBool("autoTieDiphthongs", lp.autoTieDiphthongs);
  getBool("autoDiphthongOffglideToSemivowel", lp.autoDiphthongOffglideToSemivowel);
  getNum("semivowelOffglideScale", lp.semivowelOffglideScale);

  // Trill amplitude modulation (ms; applies only to `_isTrill` phonemes).
  getNum("trillModulationMs", lp.trillModulationMs);
  getNum("trillModulationFadeMs", lp.trillModulationFadeMs);

  // Optional: spelling diphthong handling in acronym-like (spelled-out) words.
  {
    std::string mode;
    getStr("spellingDiphthongMode", mode);
    if (!mode.empty()) {
      std::string m;
      m.reserve(mode.size());
      for (char c : mode) m.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      // Only accept known modes; unknown values fall back to default.
      if (m == "none" || m == "monophthong") {
        lp.spellingDiphthongMode = m;
      }
    }
  }

  // Optional: intra-word vowel hiatus break on stressed vowel starts.
  getNum("stressedVowelHiatusGapMs", lp.stressedVowelHiatusGapMs);
  getNum("stressedVowelHiatusFadeMs", lp.stressedVowelHiatusFadeMs);
  getNum("wordBoundaryDipMs", lp.wordBoundaryDipMs);
  getNum("wordBoundaryDipDepth", lp.wordBoundaryDipDepth);

  getNum("lengthenedScale", lp.lengthenedScale);
  getNum("lengthenedScaleHu", lp.lengthenedScaleHu);
  getBool("applyLengthenedScaleToVowelsOnly", lp.applyLengthenedScaleToVowelsOnly);
  getNum("lengthenedVowelFinalCodaScale", lp.lengthenedVowelFinalCodaScale);

  // Frontend rule passes (token-level)
  getBool("coarticulationEnabled", lp.coarticulationEnabled);
  getNum("coarticulationStrength", lp.coarticulationStrength);
  getNum("highRateThreshold", lp.highRateThreshold);
  getNum("highRateCoarticulationFloor", lp.highRateCoarticulationFloor);
  getNum("highRateBandwidthWideningFactor", lp.highRateBandwidthWideningFactor);
  getNum("coarticulationWordInitialFadeScale", lp.coarticulationWordInitialFadeScale);
  getBool("coarticulationGraduated", lp.coarticulationGraduated);
  getNum("coarticulationAdjacencyMaxConsonants", lp.coarticulationAdjacencyMaxConsonants);
  getNum("coarticulationLabialF2Locus", lp.coarticulationLabialF2Locus);
  getNum("coarticulationAlveolarF2Locus", lp.coarticulationAlveolarF2Locus);
  getNum("coarticulationVelarF2Locus", lp.coarticulationVelarF2Locus);
  getNum("coarticulationVelarF2LocusFront", lp.coarticulationVelarF2LocusFront);
  getNum("coarticulationVelarF2LocusBack", lp.coarticulationVelarF2LocusBack);
  getNum("coarticulationMitalkK", lp.coarticulationMitalkK);
  getNum("coarticulationF1Scale", lp.coarticulationF1Scale);
  getNum("coarticulationF2Scale", lp.coarticulationF2Scale);
  getNum("coarticulationF3Scale", lp.coarticulationF3Scale);
  getNum("coarticulationLabialScale", lp.coarticulationLabialScale);
  getNum("coarticulationAlveolarScale", lp.coarticulationAlveolarScale);
  getNum("coarticulationPalatalScale", lp.coarticulationPalatalScale);
  getNum("coarticulationVelarScale", lp.coarticulationVelarScale);
  getNum("coarticulationAspirationBlendStart", lp.coarticulationAspirationBlendStart);
  getNum("coarticulationAspirationBlendEnd", lp.coarticulationAspirationBlendEnd);
  getBool("coarticulationVelarPinchEnabled", lp.coarticulationVelarPinchEnabled);
  getNum("coarticulationVelarPinchThreshold", lp.coarticulationVelarPinchThreshold);
  getNum("coarticulationVelarPinchF2Scale", lp.coarticulationVelarPinchF2Scale);
  getNum("coarticulationVelarPinchF3", lp.coarticulationVelarPinchF3);
  getNum("coarticulationCrossSyllableScale", lp.coarticulationCrossSyllableScale);

  // Year splitting
  getBool("yearSplittingEnabled", lp.yearSplittingEnabled);

  // Thousands-separator commas → spaces (per-language).
  getBool("thousandsSeparatorCommaToSpace", lp.thousandsSeparatorCommaToSpace);

  // Dictionary suffix stripping (per-language).
  getStrListFrom(settings, "dictSuffixes", lp.dictSuffixes);
  std::sort(lp.dictSuffixes.begin(), lp.dictSuffixes.end(),
      [](const std::string& a, const std::string& b) { return a.size() > b.size(); });

  // Special coarticulation rules (language-specific Hz deltas)
  getBool("specialCoarticulationEnabled", lp.specialCoarticulationEnabled);
  getNum("specialCoarticMaxDeltaHz", lp.specialCoarticMaxDeltaHz);

  // Cluster timing
  getBool("clusterTimingEnabled", lp.clusterTimingEnabled);
  getNum("clusterTimingFricBeforeStopScale", lp.clusterTimingFricBeforeStopScale);
  getNum("clusterTimingStopBeforeFricScale", lp.clusterTimingStopBeforeFricScale);
  getNum("clusterTimingFricBeforeFricScale", lp.clusterTimingFricBeforeFricScale);
  getNum("clusterTimingStopBeforeStopScale", lp.clusterTimingStopBeforeStopScale);
  getNum("clusterTimingTripleClusterMiddleScale", lp.clusterTimingTripleClusterMiddleScale);
  getNum("clusterTimingWordMedialConsonantScale", lp.clusterTimingWordMedialConsonantScale);
  getNum("clusterTimingWordFinalObstruentScale", lp.clusterTimingWordFinalObstruentScale);
  getNum("clusterTimingAffricateInClusterScale", lp.clusterTimingAffricateInClusterScale);

  // Syllable-aware duration
  getBool("syllableDurationEnabled", lp.syllableDurationEnabled);
  getNum("syllableDurationOnsetScale", lp.syllableDurationOnsetScale);
  getNum("syllableDurationCodaScale", lp.syllableDurationCodaScale);
  getNum("syllableDurationUnstressedOpenNucleusScale", lp.syllableDurationUnstressedOpenNucleusScale);

  // Cluster blend — C→C articulatory anticipation.
  getBool("clusterBlendEnabled", lp.clusterBlendEnabled);
  getNum("clusterBlendStrength", lp.clusterBlendStrength);
  getNum("clusterBlendNasalToStopScale", lp.clusterBlendNasalToStopScale);
  getNum("clusterBlendFricToStopScale", lp.clusterBlendFricToStopScale);
  getNum("clusterBlendStopToFricScale", lp.clusterBlendStopToFricScale);
  getNum("clusterBlendNasalToFricScale", lp.clusterBlendNasalToFricScale);
  getNum("clusterBlendLiquidToStopScale", lp.clusterBlendLiquidToStopScale);
  getNum("clusterBlendLiquidToFricScale", lp.clusterBlendLiquidToFricScale);
  getNum("clusterBlendFricToFricScale", lp.clusterBlendFricToFricScale);
  getNum("clusterBlendStopToStopScale", lp.clusterBlendStopToStopScale);
  getNum("clusterBlendDefaultPairScale", lp.clusterBlendDefaultPairScale);
  getNum("clusterBlendHomorganicScale", lp.clusterBlendHomorganicScale);
  getNum("clusterBlendWordBoundaryScale", lp.clusterBlendWordBoundaryScale);
  getNum("clusterBlendF1Scale", lp.clusterBlendF1Scale);
  getNum("clusterBlendForwardDriftStrength", lp.clusterBlendForwardDriftStrength);

  // Boundary smoothing / crossfade (optional)
  getBool("boundarySmoothingEnabled", lp.boundarySmoothingEnabled);
  getNum("boundarySmoothingF1Scale", lp.boundarySmoothingF1Scale);
  getNum("boundarySmoothingF2Scale", lp.boundarySmoothingF2Scale);
  getNum("boundarySmoothingF3Scale", lp.boundarySmoothingF3Scale);
  getBool("boundarySmoothingPlosiveSpansPhone", lp.boundarySmoothingPlosiveSpansPhone);
  getBool("boundarySmoothingNasalF1Instant", lp.boundarySmoothingNasalF1Instant);
  getBool("boundarySmoothingNasalF2F3SpansPhone", lp.boundarySmoothingNasalF2F3SpansPhone);
  // Fade times (flat-key equivalents of nested boundarySmoothing block)
  getNum("boundarySmoothingVowelToStopFadeMs", lp.boundarySmoothingVowelToStopMs);
  getNum("boundarySmoothingStopToVowelFadeMs", lp.boundarySmoothingStopToVowelMs);
  getNum("boundarySmoothingVowelToFricFadeMs", lp.boundarySmoothingVowelToFricMs);
  getNum("boundarySmoothingFricToVowelFadeMs", lp.boundarySmoothingFricToVowelMs);
  getNum("boundarySmoothingVowelToNasalFadeMs", lp.boundarySmoothingVowelToNasalMs);
  getNum("boundarySmoothingNasalToVowelFadeMs", lp.boundarySmoothingNasalToVowelMs);
  getNum("boundarySmoothingVowelToLiquidFadeMs", lp.boundarySmoothingVowelToLiquidMs);
  getNum("boundarySmoothingLiquidToVowelFadeMs", lp.boundarySmoothingLiquidToVowelMs);
  getNum("boundarySmoothingNasalToStopFadeMs", lp.boundarySmoothingNasalToStopMs);
  getNum("boundarySmoothingLiquidToStopFadeMs", lp.boundarySmoothingLiquidToStopMs);
  getNum("boundarySmoothingFricToStopFadeMs", lp.boundarySmoothingFricToStopMs);
  getNum("boundarySmoothingStopToFricFadeMs", lp.boundarySmoothingStopToFricMs);
  getNum("boundarySmoothingVowelToVowelFadeMs", lp.boundarySmoothingVowelToVowelMs);
  // Place-of-articulation transition speed overrides
  getNum("boundarySmoothingLabialF1Scale", lp.boundarySmoothingLabialF1Scale);
  getNum("boundarySmoothingLabialF2Scale", lp.boundarySmoothingLabialF2Scale);
  getNum("boundarySmoothingLabialF3Scale", lp.boundarySmoothingLabialF3Scale);
  getNum("boundarySmoothingAlveolarF1Scale", lp.boundarySmoothingAlveolarF1Scale);
  getNum("boundarySmoothingAlveolarF2Scale", lp.boundarySmoothingAlveolarF2Scale);
  getNum("boundarySmoothingAlveolarF3Scale", lp.boundarySmoothingAlveolarF3Scale);
  getNum("boundarySmoothingPalatalF1Scale", lp.boundarySmoothingPalatalF1Scale);
  getNum("boundarySmoothingPalatalF2Scale", lp.boundarySmoothingPalatalF2Scale);
  getNum("boundarySmoothingPalatalF3Scale", lp.boundarySmoothingPalatalF3Scale);
  getNum("boundarySmoothingVelarF1Scale", lp.boundarySmoothingVelarF1Scale);
  getNum("boundarySmoothingVelarF2Scale", lp.boundarySmoothingVelarF2Scale);
  getNum("boundarySmoothingVelarF3Scale", lp.boundarySmoothingVelarF3Scale);
  // Syllable-aware transition controls
  getNum("boundarySmoothingWithinSyllableScale", lp.boundarySmoothingWithinSyllableScale);
  getNum("boundarySmoothingWithinSyllableFadeScale", lp.boundarySmoothingWithinSyllableFadeScale);
  getNum("boundarySmoothingHighRateFadeRatioFloor", lp.boundarySmoothingHighRateFadeRatioFloor);
  getNum("boundarySmoothingAffricateToVowelHold", lp.boundarySmoothingAffricateToVowelHold);
  getNum("boundarySmoothingFricativeToVowelHold", lp.boundarySmoothingFricativeToVowelHold);
  getNum("boundarySmoothingStopToVowelHold", lp.boundarySmoothingStopToVowelHold);
  getNum("boundarySmoothingAffricateToVowelVoicingHold", lp.boundarySmoothingAffricateToVowelVoicingHold);
  getNum("boundarySmoothingStopToVowelVoicingHold", lp.boundarySmoothingStopToVowelVoicingHold);

  // Coda noise taper (fricative→stop continuity)
  getBool("codaNoiseTaperEnabled", lp.codaNoiseTaperEnabled);
  getNum("codaNoiseTaperPreGain", lp.codaNoiseTaperPreGain);
  getNum("codaNoiseTaperEarlyFricScale", lp.codaNoiseTaperEarlyFricScale);
  getNum("codaNoiseTaperEarlyAspAmp", lp.codaNoiseTaperEarlyAspAmp);
  getNum("codaNoiseTaperLateFricScale", lp.codaNoiseTaperLateFricScale);
  getNum("codaNoiseTaperLateAspAmp", lp.codaNoiseTaperLateAspAmp);

  // Trajectory limiting (optional)
  getBool("trajectoryLimitEnabled", lp.trajectoryLimitEnabled);
  getNum("trajectoryLimitWindowMs", lp.trajectoryLimitWindowMs);
  getBool("trajectoryLimitApplyAcrossWordBoundary", lp.trajectoryLimitApplyAcrossWordBoundary);
  getNum("trajectoryLimitLiquidRateScale", lp.trajectoryLimitLiquidRateScale);

  // Flat-key parsing for trajectoryLimit fields (NVDA settings panel compatibility)
  {
    std::string applyToStr;
    getStr("trajectoryLimitApplyTo", applyToStr);
    if (!applyToStr.empty()) {
      std::uint64_t mask = 0;
      std::string cleaned;
      for (char c : applyToStr) {
        if (c == '[' || c == ']') continue;
        cleaned += c;
      }
      size_t pos = 0;
      while (pos < cleaned.size()) {
        size_t end = cleaned.find(',', pos);
        if (end == std::string::npos) end = cleaned.size();
        std::string field = cleaned.substr(pos, end - pos);
        size_t start = field.find_first_not_of(" \t");
        if (start != std::string::npos) {
          size_t last = field.find_last_not_of(" \t");
          field = field.substr(start, last - start + 1);
        }
        if (!field.empty()) {
          FieldId fid;
          if (parseFieldId(field, fid)) {
            mask |= (1ULL << static_cast<int>(fid));
          }
        }
        pos = end + 1;
      }
      if (mask != 0) lp.trajectoryLimitApplyMask = mask;
    }
    double valCf1 = 0.0, valCf2 = 0.0, valCf3 = 0.0, valPf2 = 0.0, valPf3 = 0.0;
    getNum("trajectoryLimitMaxHzPerMsCf1", valCf1);
    getNum("trajectoryLimitMaxHzPerMsCf2", valCf2);
    getNum("trajectoryLimitMaxHzPerMsCf3", valCf3);
    getNum("trajectoryLimitMaxHzPerMsPf2", valPf2);
    getNum("trajectoryLimitMaxHzPerMsPf3", valPf3);
    if (valCf1 > 0.0) lp.trajectoryLimitMaxHzPerMs[static_cast<size_t>(FieldId::cf1)] = valCf1;
    if (valCf2 > 0.0) lp.trajectoryLimitMaxHzPerMs[static_cast<size_t>(FieldId::cf2)] = valCf2;
    if (valCf3 > 0.0) lp.trajectoryLimitMaxHzPerMs[static_cast<size_t>(FieldId::cf3)] = valCf3;
    if (valPf2 > 0.0) lp.trajectoryLimitMaxHzPerMs[static_cast<size_t>(FieldId::pf2)] = valPf2;
    if (valPf3 > 0.0) lp.trajectoryLimitMaxHzPerMs[static_cast<size_t>(FieldId::pf3)] = valPf3;
  }

// Liquid dynamics (optional)
getBool("liquidDynamicsEnabled", lp.liquidDynamicsEnabled);
getNum("liquidDynamicsLateralOnglideF1Delta", lp.liquidDynamicsLateralOnglideF1Delta);
getNum("liquidDynamicsLateralOnglideF2Delta", lp.liquidDynamicsLateralOnglideF2Delta);
getNum("liquidDynamicsLateralOnglideDurationPct", lp.liquidDynamicsLateralOnglideDurationPct);
getBool("liquidDynamicsRhoticF3DipEnabled", lp.liquidDynamicsRhoticF3DipEnabled);
getNum("liquidDynamicsRhoticF3Minimum", lp.liquidDynamicsRhoticF3Minimum);
getNum("liquidDynamicsRhoticF3DipDurationPct", lp.liquidDynamicsRhoticF3DipDurationPct);
getBool("liquidDynamicsLabialGlideTransitionEnabled", lp.liquidDynamicsLabialGlideTransitionEnabled);
getNum("liquidDynamicsLabialGlideStartF1", lp.liquidDynamicsLabialGlideStartF1);
getNum("liquidDynamicsLabialGlideStartF2", lp.liquidDynamicsLabialGlideStartF2);
getNum("liquidDynamicsLabialGlideTransitionPct", lp.liquidDynamicsLabialGlideTransitionPct);


  getBool("phraseFinalLengtheningEnabled", lp.phraseFinalLengtheningEnabled);
  getNum("phraseFinalLengtheningFinalSyllableScale", lp.phraseFinalLengtheningFinalSyllableScale);
  getNum("phraseFinalLengtheningPenultimateSyllableScale", lp.phraseFinalLengtheningPenultimateSyllableScale);
  getNum("phraseFinalLengtheningStatementScale", lp.phraseFinalLengtheningStatementScale);
  getNum("phraseFinalLengtheningQuestionScale", lp.phraseFinalLengtheningQuestionScale);
  getBool("phraseFinalLengtheningNucleusOnlyMode", lp.phraseFinalLengtheningNucleusOnlyMode);
  getNum("phraseFinalLengtheningNucleusScale", lp.phraseFinalLengtheningNucleusScale);
  getNum("phraseFinalLengtheningNucleusDiphthongScale", lp.phraseFinalLengtheningNucleusDiphthongScale);
  getNum("phraseFinalLengtheningCodaScale", lp.phraseFinalLengtheningCodaScale);
  getNum("phraseFinalLengtheningCodaStopScale", lp.phraseFinalLengtheningCodaStopScale);
  getNum("phraseFinalLengtheningCodaFricativeScale", lp.phraseFinalLengtheningCodaFricativeScale);
  getNum("phraseFinalLengtheningCodaNasalScale", lp.phraseFinalLengtheningCodaNasalScale);

  // ── Prominence pass (nested block + flat-key fallbacks) ──
  if (const yaml_min::Node* pr = settings.get("prominence"); pr && pr->isMap()) {
    getBoolFrom(*pr, "enabled", lp.prominenceEnabled);
    getNumFrom(*pr, "primaryStressWeight", lp.prominencePrimaryStressWeight);
    getNumFrom(*pr, "secondaryStressWeight", lp.prominenceSecondaryStressWeight);
    getNumFrom(*pr, "secondaryStressLevel", lp.prominenceSecondaryStressLevel);
    getNumFrom(*pr, "longVowelWeight", lp.prominenceLongVowelWeight);
    getStrFrom(*pr, "longVowelMode", lp.prominenceLongVowelMode);
    getNumFrom(*pr, "wordInitialBoost", lp.prominenceWordInitialBoost);
    getNumFrom(*pr, "wordFinalReduction", lp.prominenceWordFinalReduction);
    getNumFrom(*pr, "durationProminentFloorMs", lp.prominenceDurationProminentFloorMs);
    getNumFrom(*pr, "durationPrimaryFloorMs", lp.prominenceDurationPrimaryFloorMs);
    getNumFrom(*pr, "durationReducedCeiling", lp.prominenceDurationReducedCeiling);
    getNumFrom(*pr, "monosyllableFloor", lp.prominenceMonosyllableFloor);
    {
      std::vector<std::string> excStrs;
      getStrListFrom(*pr, "monosyllableExclude", excStrs);
      if (!excStrs.empty()) {
        lp.prominenceMonosyllableExclude.clear();
        for (const auto& s : excStrs) {
          if (!s.empty()) lp.prominenceMonosyllableExclude.push_back(utf8ToU32(s));
        }
      }
    }
    getNumFrom(*pr, "fullVowelFloor", lp.prominenceFullVowelFloor);
    getNumFrom(*pr, "amplitudeBoostDb", lp.prominenceAmplitudeBoostDb);
    getNumFrom(*pr, "amplitudeReductionDb", lp.prominenceAmplitudeReductionDb);
    getBoolFrom(*pr, "pitchFromProminence", lp.prominencePitchFromProminence);
  }
  // Flat-key fallbacks
  getBool("prominenceEnabled", lp.prominenceEnabled);
  getNum("prominencePrimaryStressWeight", lp.prominencePrimaryStressWeight);
  getNum("prominenceSecondaryStressWeight", lp.prominenceSecondaryStressWeight);
  getNum("prominenceSecondaryStressLevel", lp.prominenceSecondaryStressLevel);
  getNum("prominenceLongVowelWeight", lp.prominenceLongVowelWeight);
  getStr("prominenceLongVowelMode", lp.prominenceLongVowelMode);
  getNum("prominenceWordInitialBoost", lp.prominenceWordInitialBoost);
  getNum("prominenceWordFinalReduction", lp.prominenceWordFinalReduction);
  getNum("prominenceDurationProminentFloorMs", lp.prominenceDurationProminentFloorMs);
  getNum("prominenceDurationPrimaryFloorMs", lp.prominenceDurationPrimaryFloorMs);
  getNum("prominenceDurationReducedCeiling", lp.prominenceDurationReducedCeiling);
  getNum("prominenceMonosyllableFloor", lp.prominenceMonosyllableFloor);
  getNum("prominenceFullVowelFloor", lp.prominenceFullVowelFloor);
  getNum("prominenceAmplitudeBoostDb", lp.prominenceAmplitudeBoostDb);
  getNum("prominenceAmplitudeReductionDb", lp.prominenceAmplitudeReductionDb);
  getBool("prominencePitchFromProminence", lp.prominencePitchFromProminence);

  getBool("microprosodyEnabled", lp.microprosodyEnabled);
  getBool("microprosodyVoicelessF0RaiseEnabled", lp.microprosodyVoicelessF0RaiseEnabled);
  getNum("microprosodyVoicelessF0RaiseHz", lp.microprosodyVoicelessF0RaiseHz);
  getNum("microprosodyVoicelessF0RaiseEndHz", lp.microprosodyVoicelessF0RaiseEndHz);
  getBool("microprosodyVoicedF0LowerEnabled", lp.microprosodyVoicedF0LowerEnabled);
  getNum("microprosodyVoicedF0LowerHz", lp.microprosodyVoicedF0LowerHz);
  getNum("microprosodyMinVowelMs", lp.microprosodyMinVowelMs);
  getBool("microprosodyFollowingF0Enabled", lp.microprosodyFollowingF0Enabled);
  getNum("microprosodyFollowingVoicelessRaiseHz", lp.microprosodyFollowingVoicelessRaiseHz);
  getNum("microprosodyFollowingVoicedLowerHz", lp.microprosodyFollowingVoicedLowerHz);
  getNum("microprosodyVoicedFricativeLowerScale", lp.microprosodyVoicedFricativeLowerScale);
  getBool("microprosodyIntrinsicF0Enabled", lp.microprosodyIntrinsicF0Enabled);
  getNum("microprosodyIntrinsicF0HighThreshold", lp.microprosodyIntrinsicF0HighThreshold);
  getNum("microprosodyIntrinsicF0LowThreshold", lp.microprosodyIntrinsicF0LowThreshold);
  getNum("microprosodyIntrinsicF0HighRaiseHz", lp.microprosodyIntrinsicF0HighRaiseHz);
  getNum("microprosodyIntrinsicF0LowDropHz", lp.microprosodyIntrinsicF0LowDropHz);
  getBool("microprosodyPreVoicelessShortenEnabled", lp.microprosodyPreVoicelessShortenEnabled);
  getNum("microprosodyPreVoicelessShortenScale", lp.microprosodyPreVoicelessShortenScale);
  getNum("microprosodyPreVoicelessMinMs", lp.microprosodyPreVoicelessMinMs);
  getBool("microprosodyVoicelessCodaLengthenEnabled", lp.microprosodyVoicelessCodaLengthenEnabled);
  getNum("microprosodyVoicelessCodaLengthenScale", lp.microprosodyVoicelessCodaLengthenScale);
  getNum("microprosodyMaxTotalDeltaHz", lp.microprosodyMaxTotalDeltaHz);

  getNum("nasalMinDurationMs", lp.nasalMinDurationMs);

  // ── Rate compensation (nested YAML) ──
  if (const yaml_min::Node* rc = settings.get("rateCompensation"); rc && rc->isMap()) {
    getBoolFrom(*rc, "enabled", lp.rateCompEnabled);
    if (const yaml_min::Node* md = rc->get("minimumDurations"); md && md->isMap()) {
      getNumFrom(*md, "vowelMs", lp.rateCompVowelFloorMs);
      getNumFrom(*md, "diphthongMs", lp.rateCompDiphthongFloorMs);
      getNumFrom(*md, "fricativeMs", lp.rateCompFricativeFloorMs);
      getNumFrom(*md, "stopMs", lp.rateCompStopFloorMs);
      getNumFrom(*md, "nasalMs", lp.rateCompNasalFloorMs);
      getNumFrom(*md, "liquidMs", lp.rateCompLiquidFloorMs);
      getNumFrom(*md, "affricateMs", lp.rateCompAffricateFloorMs);
      getNumFrom(*md, "semivowelMs", lp.rateCompSemivowelFloorMs);
      getNumFrom(*md, "tapMs", lp.rateCompTapFloorMs);
      getNumFrom(*md, "trillMs", lp.rateCompTrillFloorMs);
      getNumFrom(*md, "voicedConsonantMs", lp.rateCompVoicedConsonantFloorMs);
    }
    getNumFrom(*rc, "wordFinalBonusMs", lp.rateCompWordFinalBonusMs);
    getNumFrom(*rc, "floorSpeedScale", lp.rateCompFloorSpeedScale);
    getBoolFrom(*rc, "clusterProportionGuard", lp.rateCompClusterProportionGuard);
    getNumFrom(*rc, "clusterMaxRatioShift", lp.rateCompClusterMaxRatioShift);
    getNumFrom(*rc, "sonorantContextBonusMs", lp.rateCompSonorantContextBonusMs);
    getNumFrom(*rc, "preRhoticClusterBonusMs", lp.rateCompPreRhoticClusterBonusMs);
    if (const yaml_min::Node* sr = rc->get("schwaReduction"); sr && sr->isMap()) {
      getBoolFrom(*sr, "enabled", lp.rateCompSchwaReductionEnabled);
      getNumFrom(*sr, "threshold", lp.rateCompSchwaThreshold);
      getNumFrom(*sr, "scale", lp.rateCompSchwaScale);
    }
  }

  // ── Rate compensation (flat-key fallbacks) ──
  getBool("rateCompEnabled", lp.rateCompEnabled);
  getNum("rateCompVowelFloorMs", lp.rateCompVowelFloorMs);
  getNum("rateCompDiphthongFloorMs", lp.rateCompDiphthongFloorMs);
  getNum("rateCompFricativeFloorMs", lp.rateCompFricativeFloorMs);
  getNum("rateCompStopFloorMs", lp.rateCompStopFloorMs);
  getNum("rateCompNasalFloorMs", lp.rateCompNasalFloorMs);
  getNum("rateCompLiquidFloorMs", lp.rateCompLiquidFloorMs);
  getNum("rateCompAffricateFloorMs", lp.rateCompAffricateFloorMs);
  getNum("rateCompSemivowelFloorMs", lp.rateCompSemivowelFloorMs);
  getNum("rateCompTapFloorMs", lp.rateCompTapFloorMs);
  getNum("rateCompTrillFloorMs", lp.rateCompTrillFloorMs);
  getNum("rateCompVoicedConsonantFloorMs", lp.rateCompVoicedConsonantFloorMs);
  getNum("rateCompWordFinalBonusMs", lp.rateCompWordFinalBonusMs);
  getNum("rateCompFloorSpeedScale", lp.rateCompFloorSpeedScale);
  getBool("rateCompClusterProportionGuard", lp.rateCompClusterProportionGuard);
  getNum("rateCompClusterMaxRatioShift", lp.rateCompClusterMaxRatioShift);
  getNum("rateCompSonorantContextBonusMs", lp.rateCompSonorantContextBonusMs);
  getNum("rateCompPreRhoticClusterBonusMs", lp.rateCompPreRhoticClusterBonusMs);
  getNum("sonorantContextAmplitudeScale", lp.sonorantContextAmplitudeScale);
  getBool("rateCompSchwaReductionEnabled", lp.rateCompSchwaReductionEnabled);
  getNum("rateCompSchwaThreshold", lp.rateCompSchwaThreshold);
  getNum("rateCompSchwaScale", lp.rateCompSchwaScale);

  // ── BACKWARD COMPAT: old rateReduction* keys map to new fields ──
  {
    bool oldEnabled = false;
    getBool("rateReductionEnabled", oldEnabled);
    if (oldEnabled && !lp.rateCompEnabled) {
      lp.rateCompEnabled = true;
      lp.rateCompSchwaReductionEnabled = true;
    }
    double oldThreshold = 0.0;
    getNum("rateReductionSchwaReductionThreshold", oldThreshold);
    if (oldThreshold > 0.0) lp.rateCompSchwaThreshold = oldThreshold;
    double oldScale = 0.0;
    getNum("rateReductionSchwaScale", oldScale);
    if (oldScale > 0.0) lp.rateCompSchwaScale = oldScale;
  }

  getBool("wordFinalSchwaReductionEnabled", lp.wordFinalSchwaReductionEnabled);
  getNum("wordFinalSchwaScale", lp.wordFinalSchwaScale);
  getNum("wordFinalSchwaMinDurationMs", lp.wordFinalSchwaMinDurationMs);

  getBool("nasalizationAnticipatoryEnabled", lp.nasalizationAnticipatoryEnabled);
  getNum("nasalizationAnticipatoryAmplitude", lp.nasalizationAnticipatoryAmplitude);
  getNum("nasalizationAnticipatoryBlend", lp.nasalizationAnticipatoryBlend);

  getBool("allophoneRulesEnabled", lp.allophoneRulesEnabled);

// Length contrast / gemination (optional)
getBool("lengthContrastEnabled", lp.lengthContrastEnabled);
getNum("lengthContrastShortVowelCeilingMs", lp.lengthContrastShortVowelCeilingMs);
getNum("lengthContrastLongVowelFloorMs", lp.lengthContrastLongVowelFloorMs);
getNum("lengthContrastGeminateClosureScale", lp.lengthContrastGeminateClosureScale);
getNum("lengthContrastGeminateReleaseScale", lp.lengthContrastGeminateReleaseScale);
getNum("lengthContrastPreGeminateVowelScale", lp.lengthContrastPreGeminateVowelScale);

// Diphthong collapse
getBool("diphthongCollapseEnabled", lp.diphthongCollapseEnabled);
getNum("diphthongAmplitudeDipFactor", lp.diphthongAmplitudeDipFactor);
getNum("diphthongMicroFrameIntervalMs", lp.diphthongMicroFrameIntervalMs);
getNum("diphthongDurationScale", lp.diphthongDurationScale);
getNum("diphthongDurationFloorMs", lp.diphthongDurationFloorMs);
getNum("diphthongRateCompensation", lp.diphthongRateCompensation);
getNum("diphthongOnsetHoldExponent", lp.diphthongOnsetHoldExponent);
getNum("diphthongOnsetSettleMs", lp.diphthongOnsetSettleMs);

// Nested settings blocks inside `settings:` (optional; override flat keys)
if (const yaml_min::Node* bs = settings.get("boundarySmoothing"); bs && bs->isMap()) {
  getBoolFrom(*bs, "enabled", lp.boundarySmoothingEnabled);
  getNumFrom(*bs, "f1Scale", lp.boundarySmoothingF1Scale);
  getNumFrom(*bs, "f2Scale", lp.boundarySmoothingF2Scale);
  getNumFrom(*bs, "f3Scale", lp.boundarySmoothingF3Scale);
  getBoolFrom(*bs, "plosiveSpansPhone", lp.boundarySmoothingPlosiveSpansPhone);
  getBoolFrom(*bs, "nasalF1Instant", lp.boundarySmoothingNasalF1Instant);
  getBoolFrom(*bs, "nasalF2F3SpansPhone", lp.boundarySmoothingNasalF2F3SpansPhone);
  getNumFrom(*bs, "vowelToStopFadeMs", lp.boundarySmoothingVowelToStopMs);
  getNumFrom(*bs, "stopToVowelFadeMs", lp.boundarySmoothingStopToVowelMs);
  getNumFrom(*bs, "vowelToFricFadeMs", lp.boundarySmoothingVowelToFricMs);
  getNumFrom(*bs, "fricToVowelFadeMs", lp.boundarySmoothingFricToVowelMs);
  getNumFrom(*bs, "vowelToNasalFadeMs", lp.boundarySmoothingVowelToNasalMs);
  getNumFrom(*bs, "nasalToVowelFadeMs", lp.boundarySmoothingNasalToVowelMs);
  getNumFrom(*bs, "vowelToLiquidFadeMs", lp.boundarySmoothingVowelToLiquidMs);
  getNumFrom(*bs, "liquidToVowelFadeMs", lp.boundarySmoothingLiquidToVowelMs);
  getNumFrom(*bs, "nasalToStopFadeMs", lp.boundarySmoothingNasalToStopMs);
  getNumFrom(*bs, "liquidToStopFadeMs", lp.boundarySmoothingLiquidToStopMs);
  getNumFrom(*bs, "fricToStopFadeMs", lp.boundarySmoothingFricToStopMs);
  getNumFrom(*bs, "stopToFricFadeMs", lp.boundarySmoothingStopToFricMs);
  getNumFrom(*bs, "vowelToVowelFadeMs", lp.boundarySmoothingVowelToVowelMs);

  // Per-place-of-articulation transition speed overrides.
  getNumFrom(*bs, "labialF1Scale", lp.boundarySmoothingLabialF1Scale);
  getNumFrom(*bs, "labialF2Scale", lp.boundarySmoothingLabialF2Scale);
  getNumFrom(*bs, "labialF3Scale", lp.boundarySmoothingLabialF3Scale);
  getNumFrom(*bs, "alveolarF1Scale", lp.boundarySmoothingAlveolarF1Scale);
  getNumFrom(*bs, "alveolarF2Scale", lp.boundarySmoothingAlveolarF2Scale);
  getNumFrom(*bs, "alveolarF3Scale", lp.boundarySmoothingAlveolarF3Scale);
  getNumFrom(*bs, "palatalF1Scale", lp.boundarySmoothingPalatalF1Scale);
  getNumFrom(*bs, "palatalF2Scale", lp.boundarySmoothingPalatalF2Scale);
  getNumFrom(*bs, "palatalF3Scale", lp.boundarySmoothingPalatalF3Scale);
  getNumFrom(*bs, "velarF1Scale", lp.boundarySmoothingVelarF1Scale);
  getNumFrom(*bs, "velarF2Scale", lp.boundarySmoothingVelarF2Scale);
  getNumFrom(*bs, "velarF3Scale", lp.boundarySmoothingVelarF3Scale);

  // Syllable-aware transition controls.
  getNumFrom(*bs, "withinSyllableScale", lp.boundarySmoothingWithinSyllableScale);
  getNumFrom(*bs, "withinSyllableFadeScale", lp.boundarySmoothingWithinSyllableFadeScale);

  // Rate-adaptive fade ratio floor.
  getNumFrom(*bs, "highRateFadeRatioFloor", lp.boundarySmoothingHighRateFadeRatioFloor);

  // Source amplitude hold ratios.
  getNumFrom(*bs, "affricateToVowelHold", lp.boundarySmoothingAffricateToVowelHold);
  getNumFrom(*bs, "fricativeToVowelHold", lp.boundarySmoothingFricativeToVowelHold);
  getNumFrom(*bs, "stopToVowelHold", lp.boundarySmoothingStopToVowelHold);

  // Voicing onset hold ratios.
  getNumFrom(*bs, "affricateToVowelVoicingHold", lp.boundarySmoothingAffricateToVowelVoicingHold);
  getNumFrom(*bs, "stopToVowelVoicingHold", lp.boundarySmoothingStopToVowelVoicingHold);
}

if (const yaml_min::Node* ss = settings.get("syllableStructure"); ss && ss->isMap()) {
  std::vector<std::string> onsetStrs;
  getStrListFrom(*ss, "legalOnsets", onsetStrs);
  lp.legalOnsets.clear();
  for (const auto& s : onsetStrs) {
    if (!s.empty()) lp.legalOnsets.push_back(utf8ToU32(s));
  }
}

if (const yaml_min::Node* ne = settings.get("numberExpansion"); ne && ne->isMap()) {
  getBoolFrom(*ne, "enabled", lp.numberExpansion.enabled);
  getStrListFrom(*ne, "digits", lp.numberExpansion.digits);
  getStrListFrom(*ne, "teens",  lp.numberExpansion.teens);
  getStrListFrom(*ne, "tens",   lp.numberExpansion.tens);
  getStrFrom(*ne, "hundred",     lp.numberExpansion.hundred);
  getStrFrom(*ne, "thousand",    lp.numberExpansion.thousand);
  getStrFrom(*ne, "million",     lp.numberExpansion.million);
  getStrFrom(*ne, "billion",     lp.numberExpansion.billion);
  getStrFrom(*ne, "conjunction", lp.numberExpansion.conjunction);
  getStrFrom(*ne, "ohDigit",     lp.numberExpansion.ohDigit);
}

if (const yaml_min::Node* tl = settings.get("trajectoryLimit"); tl && tl->isMap()) {
  getBoolFrom(*tl, "enabled", lp.trajectoryLimitEnabled);
  getNumFrom(*tl, "windowMs", lp.trajectoryLimitWindowMs);
  getBoolFrom(*tl, "applyAcrossWordBoundary", lp.trajectoryLimitApplyAcrossWordBoundary);
  getNumFrom(*tl, "liquidRateScale", lp.trajectoryLimitLiquidRateScale);

  // applyTo: [cf2, cf3, ...]
  {
    std::vector<std::string> fields;
    getStrListFrom(*tl, "applyTo", fields);
    if (!fields.empty()) {
      std::uint64_t mask = 0;
      for (const std::string& name : fields) {
        FieldId fid;
        if (parseFieldId(name, fid)) {
          mask |= (1ULL << static_cast<int>(fid));
        }
      }
      if (mask != 0) lp.trajectoryLimitApplyMask = mask;
    }
  }

  // maxHzPerMs:
  //   cf2: 18
  //   cf3: 22
  if (const yaml_min::Node* mh = tl->get("maxHzPerMs"); mh && mh->isMap()) {
    for (const auto& kv : mh->map) {
      const std::string& key = kv.first;
      const yaml_min::Node& v = kv.second;
      double hzPerMs = 0.0;
      if (!v.asNumber(hzPerMs)) continue;
      FieldId fid;
      if (!parseFieldId(key, fid)) continue;
      const int idx = static_cast<int>(fid);
      if (idx >= 0 && idx < static_cast<int>(kFrameFieldCount)) {
        lp.trajectoryLimitMaxHzPerMs[static_cast<size_t>(idx)] = hzPerMs;
      }
    }
  }
}

if (const yaml_min::Node* ld = settings.get("liquidDynamics"); ld && ld->isMap()) {
  getBoolFrom(*ld, "enabled", lp.liquidDynamicsEnabled);

  if (const yaml_min::Node* lo = ld->get("lateralOnglide"); lo && lo->isMap()) {
    getNumFrom(*lo, "f1Delta", lp.liquidDynamicsLateralOnglideF1Delta);
    getNumFrom(*lo, "f2Delta", lp.liquidDynamicsLateralOnglideF2Delta);
    getNumFrom(*lo, "durationPct", lp.liquidDynamicsLateralOnglideDurationPct);
  }

  if (const yaml_min::Node* rd = ld->get("rhoticF3Dip"); rd && rd->isMap()) {
    getBoolFrom(*rd, "enabled", lp.liquidDynamicsRhoticF3DipEnabled);
    getNumFrom(*rd, "f3Minimum", lp.liquidDynamicsRhoticF3Minimum);
    getNumFrom(*rd, "dipDurationPct", lp.liquidDynamicsRhoticF3DipDurationPct);
  }

  if (const yaml_min::Node* wg = ld->get("labialGlideTransition"); wg && wg->isMap()) {
    getBoolFrom(*wg, "enabled", lp.liquidDynamicsLabialGlideTransitionEnabled);
    getNumFrom(*wg, "startF1", lp.liquidDynamicsLabialGlideStartF1);
    getNumFrom(*wg, "startF2", lp.liquidDynamicsLabialGlideStartF2);
    getNumFrom(*wg, "transitionPct", lp.liquidDynamicsLabialGlideTransitionPct);
  }
}

if (const yaml_min::Node* lc = settings.get("lengthContrast"); lc && lc->isMap()) {
  getBoolFrom(*lc, "enabled", lp.lengthContrastEnabled);
  getNumFrom(*lc, "shortVowelCeiling", lp.lengthContrastShortVowelCeilingMs);
  getNumFrom(*lc, "longVowelFloor", lp.lengthContrastLongVowelFloorMs);
  getNumFrom(*lc, "geminateClosureScale", lp.lengthContrastGeminateClosureScale);
  getNumFrom(*lc, "geminateReleaseScale", lp.lengthContrastGeminateReleaseScale);
  getNumFrom(*lc, "preGeminateVowelScale", lp.lengthContrastPreGeminateVowelScale);
}

if (const yaml_min::Node* dc = settings.get("diphthongCollapse"); dc && dc->isMap()) {
  getBoolFrom(*dc, "enabled", lp.diphthongCollapseEnabled);
  getNumFrom(*dc, "amplitudeDipFactor", lp.diphthongAmplitudeDipFactor);
  getNumFrom(*dc, "microFrameIntervalMs", lp.diphthongMicroFrameIntervalMs);
  getNumFrom(*dc, "durationScale", lp.diphthongDurationScale);
  getNumFrom(*dc, "durationFloorMs", lp.diphthongDurationFloorMs);
  getNumFrom(*dc, "rateCompensation", lp.diphthongRateCompensation);
  getNumFrom(*dc, "onsetHoldExponent", lp.diphthongOnsetHoldExponent);
  getNumFrom(*dc, "onsetSettleMs", lp.diphthongOnsetSettleMs);

  // Per-pair duration scales: "onset offset" -> scale factor.
  if (const yaml_min::Node* ps = dc->get("pairScales"); ps && ps->isMap()) {
      for (const auto& kv : ps->map) {
          double s = 1.0;
          if (!kv.second.asNumber(s)) continue;
          // Split "onset offset" on first space.
          auto sp = kv.first.find(' ');
          if (sp == std::string::npos) continue;
          std::string onsetUtf8  = kv.first.substr(0, sp);
          std::string offsetUtf8 = kv.first.substr(sp + 1);
          LanguagePack::DiphthongPairScale dps;
          dps.onset  = utf8ToU32(onsetUtf8);
          dps.offset = utf8ToU32(offsetUtf8);
          dps.scale  = s;
          lp.diphthongPairScales.push_back(std::move(dps));
      }
  }
}

// Fujisaki clause-type overrides (nested: fujisakiClauseType: { question: { ... } })
if (const yaml_min::Node* fct = settings.get("fujisakiClauseType"); fct && fct->isMap()) {
  if (const yaml_min::Node* q = fct->get("question"); q && q->isMap()) {
    getNumFrom(*q, "phraseAmpScale",   lp.fujisakiQuestion.phraseAmpScale);
    getNumFrom(*q, "accentBoost",      lp.fujisakiQuestion.accentBoost);
    getNumFrom(*q, "declinationScale", lp.fujisakiQuestion.declinationScale);
    getNumFrom(*q, "basePitchScale",   lp.fujisakiQuestion.basePitchScale);
    getNumFrom(*q, "finalRiseScale",   lp.fujisakiQuestion.finalRiseScale);
    getNumFrom(*q, "finalDropScale",   lp.fujisakiQuestion.finalDropScale);
  }
  if (const yaml_min::Node* e = fct->get("exclamation"); e && e->isMap()) {
    getNumFrom(*e, "phraseAmpScale",   lp.fujisakiExclamation.phraseAmpScale);
    getNumFrom(*e, "accentBoost",      lp.fujisakiExclamation.accentBoost);
    getNumFrom(*e, "declinationScale", lp.fujisakiExclamation.declinationScale);
    getNumFrom(*e, "basePitchScale",   lp.fujisakiExclamation.basePitchScale);
    getNumFrom(*e, "finalRiseScale",   lp.fujisakiExclamation.finalRiseScale);
    getNumFrom(*e, "finalDropScale",   lp.fujisakiExclamation.finalDropScale);
  }
  if (const yaml_min::Node* c = fct->get("comma"); c && c->isMap()) {
    getNumFrom(*c, "phraseAmpScale",   lp.fujisakiComma.phraseAmpScale);
    getNumFrom(*c, "accentBoost",      lp.fujisakiComma.accentBoost);
    getNumFrom(*c, "declinationScale", lp.fujisakiComma.declinationScale);
    getNumFrom(*c, "basePitchScale",   lp.fujisakiComma.basePitchScale);
    getNumFrom(*c, "finalRiseScale",   lp.fujisakiComma.finalRiseScale);
    getNumFrom(*c, "finalDropScale",   lp.fujisakiComma.finalDropScale);
  }
}

// Data-driven allophone rules (replaces old positionalAllophones: block).
if (const yaml_min::Node* ar = settings.get("allophoneRules"); ar && ar->isMap()) {
  getBoolFrom(*ar, "enabled", lp.allophoneRulesEnabled);

  if (const yaml_min::Node* rules = ar->get("rules"); rules && rules->isSeq()) {
    lp.allophoneRules.clear();
    for (const auto& item : rules->seq) {
      if (!item.isMap()) continue;
      AllophoneRule rule;
      getStrFrom(item, "name", rule.name);
      getStrFrom(item, "action", rule.action);
      getStrFrom(item, "tokenType", rule.tokenType);
      getStrFrom(item, "position", rule.position);
      getStrFrom(item, "stress", rule.stress);
      getStrFrom(item, "place", rule.place);

      // Parse phonemes (UTF-8 → u32)
      if (const yaml_min::Node* ph = item.get("phonemes"); ph && ph->isSeq()) {
        for (const auto& el : ph->seq) {
          if (el.isScalar()) rule.phonemes.push_back(utf8ToU32(el.scalar));
        }
      }

      // Parse flags / notFlags
      if (const yaml_min::Node* fl = item.get("flags"); fl && fl->isSeq()) {
        for (const auto& el : fl->seq) {
          if (el.isScalar()) rule.flags.push_back(el.scalar);
        }
      }
      if (const yaml_min::Node* nf = item.get("notFlags"); nf && nf->isSeq()) {
        for (const auto& el : nf->seq) {
          if (el.isScalar()) rule.notFlags.push_back(el.scalar);
        }
      }

      // Parse after / before neighbor filters
      if (const yaml_min::Node* af = item.get("after"); af && af->isSeq()) {
        for (const auto& el : af->seq) {
          if (el.isScalar()) rule.after.push_back(utf8ToU32(el.scalar));
        }
      }
      if (const yaml_min::Node* bf = item.get("before"); bf && bf->isSeq()) {
        for (const auto& el : bf->seq) {
          if (el.isScalar()) rule.before.push_back(utf8ToU32(el.scalar));
        }
      }
      getBoolFrom(item, "beforeSamePhoneme", rule.beforeSamePhoneme);
      getBoolFrom(item, "afterSamePhoneme",  rule.afterSamePhoneme);
      // Parse neighbor flag filters
      if (const yaml_min::Node* af2 = item.get("afterFlags"); af2 && af2->isSeq()) {
        for (const auto& el : af2->seq) {
          if (el.isScalar()) rule.afterFlags.push_back(el.scalar);
        }
      }
      if (const yaml_min::Node* naf = item.get("notAfterFlags"); naf && naf->isSeq()) {
        for (const auto& el : naf->seq) {
          if (el.isScalar()) rule.notAfterFlags.push_back(el.scalar);
        }
      }
      if (const yaml_min::Node* bf2 = item.get("beforeFlags"); bf2 && bf2->isSeq()) {
        for (const auto& el : bf2->seq) {
          if (el.isScalar()) rule.beforeFlags.push_back(el.scalar);
        }
      }
      if (const yaml_min::Node* nbf = item.get("notBeforeFlags"); nbf && nbf->isSeq()) {
        for (const auto& el : nbf->seq) {
          if (el.isScalar()) rule.notBeforeFlags.push_back(el.scalar);
        }
      }

      // Parse replace params
      {
        std::string rTo;
        getStrFrom(item, "replaceTo", rTo);
        if (!rTo.empty()) rule.replaceTo = utf8ToU32(rTo);
        getNumFrom(item, "replaceDurationMs", rule.replaceDurationMs);
        getBoolFrom(item, "replaceRemovesClosure", rule.replaceRemovesClosure);
        getBoolFrom(item, "replaceRemovesAspiration", rule.replaceRemovesAspiration);
        getNumFrom(item, "replaceClosureScale", rule.replaceClosureScale);
        getNumFrom(item, "replaceAspirationScale", rule.replaceAspirationScale);
      }

      // Parse scale params
      getNumFrom(item, "durationScale", rule.durationScale);
      getNumFrom(item, "fadeScale", rule.fadeScale);
      if (const yaml_min::Node* fs = item.get("fieldScales"); fs && fs->isMap()) {
        for (const auto& kv : fs->map) {
          double val = 1.0;
          if (kv.second.isScalar()) {
            kv.second.asNumber(val);
          }
          rule.fieldScales.emplace_back(kv.first, val);
        }
      }

      // Parse shift params
      if (const yaml_min::Node* sh = item.get("fieldShifts"); sh && sh->isSeq()) {
        for (const auto& entry : sh->seq) {
          if (!entry.isMap()) continue;
          AllophoneRule::ShiftEntry se;
          getStrFrom(entry, "field", se.field);
          getNumFrom(entry, "deltaHz", se.deltaHz);
          getNumFrom(entry, "targetHz", se.targetHz);
          getNumFrom(entry, "blend", se.blend);
          if (!se.field.empty()) rule.fieldShifts.push_back(se);
        }
      }

      // Parse insert params
      {
        std::string ins;
        getStrFrom(item, "insertPhoneme", ins);
        if (!ins.empty()) rule.insertPhoneme = utf8ToU32(ins);
        getNumFrom(item, "insertDurationMs", rule.insertDurationMs);
        getNumFrom(item, "insertFadeMs", rule.insertFadeMs);
      }
      if (const yaml_min::Node* ic = item.get("insertContexts"); ic && ic->isSeq()) {
        for (const auto& el : ic->seq) {
          if (el.isScalar()) rule.insertContexts.push_back(el.scalar);
        }
      }

      // Validate: must have an action
      if (!rule.action.empty()) {
        lp.allophoneRules.push_back(std::move(rule));
      }
    }
  }
}

// Backward compat: read old "positionalAllophones: enabled: true" and set the new flag.
if (const yaml_min::Node* pa = settings.get("positionalAllophones"); pa && pa->isMap()) {
  bool oldEnabled = false;
  getBoolFrom(*pa, "enabled", oldEnabled);
  if (oldEnabled && !lp.allophoneRulesEnabled) {
    lp.allophoneRulesEnabled = true;
  }
}

if (const yaml_min::Node* sc = settings.get("specialCoarticulation"); sc && sc->isMap()) {
  getBoolFrom(*sc, "enabled", lp.specialCoarticulationEnabled);
  getNumFrom(*sc, "maxDeltaHz", lp.specialCoarticMaxDeltaHz);

  if (const yaml_min::Node* rules = sc->get("rules"); rules && rules->isSeq()) {
    lp.specialCoarticRules.clear();
    for (const auto& ruleNode : rules->seq) {
      if (!ruleNode.isMap()) continue;
      SpecialCoarticRule rule;
      if (const yaml_min::Node* trig = ruleNode.get("triggers"); trig && trig->isSeq()) {
        for (const auto& t : trig->seq) {
          if (t.isScalar()) rule.triggers.push_back(t.scalar);
        }
      }
      getStrFrom(ruleNode, "name", rule.name);
      getStrFrom(ruleNode, "vowelFilter", rule.vowelFilter);
      getStrFrom(ruleNode, "formant", rule.formant);
      getNumFrom(ruleNode, "deltaHz", rule.deltaHz);
      getStrFrom(ruleNode, "side", rule.side);
      getBoolFrom(ruleNode, "cumulative", rule.cumulative);
      getNumFrom(ruleNode, "unstressedScale", rule.unstressedScale);
      getNumFrom(ruleNode, "phraseFinalStressedScale", rule.phraseFinalStressedScale);
      if (!rule.triggers.empty() && !rule.formant.empty()) lp.specialCoarticRules.push_back(std::move(rule));
    }
  }
}

if (const yaml_min::Node* ct = settings.get("clusterTiming"); ct && ct->isMap()) {
  getBoolFrom(*ct, "enabled", lp.clusterTimingEnabled);
  getNumFrom(*ct, "fricBeforeStopScale", lp.clusterTimingFricBeforeStopScale);
  getNumFrom(*ct, "stopBeforeFricScale", lp.clusterTimingStopBeforeFricScale);
  getNumFrom(*ct, "fricBeforeFricScale", lp.clusterTimingFricBeforeFricScale);
  getNumFrom(*ct, "stopBeforeStopScale", lp.clusterTimingStopBeforeStopScale);
  getNumFrom(*ct, "tripleClusterMiddleScale", lp.clusterTimingTripleClusterMiddleScale);
  getNumFrom(*ct, "affricateInClusterScale", lp.clusterTimingAffricateInClusterScale);
  getNumFrom(*ct, "wordMedialConsonantScale", lp.clusterTimingWordMedialConsonantScale);
  getNumFrom(*ct, "wordFinalObstruentScale", lp.clusterTimingWordFinalObstruentScale);
}

if (const yaml_min::Node* sd = settings.get("syllableDuration"); sd && sd->isMap()) {
  getBoolFrom(*sd, "enabled", lp.syllableDurationEnabled);
  getNumFrom(*sd, "onsetScale", lp.syllableDurationOnsetScale);
  getNumFrom(*sd, "codaScale", lp.syllableDurationCodaScale);
  getNumFrom(*sd, "unstressedOpenNucleusScale", lp.syllableDurationUnstressedOpenNucleusScale);
}

if (const yaml_min::Node* cb = settings.get("clusterBlend"); cb && cb->isMap()) {
  getBoolFrom(*cb, "enabled", lp.clusterBlendEnabled);
  getNumFrom(*cb, "strength", lp.clusterBlendStrength);
  getNumFrom(*cb, "nasalToStopScale", lp.clusterBlendNasalToStopScale);
  getNumFrom(*cb, "fricToStopScale", lp.clusterBlendFricToStopScale);
  getNumFrom(*cb, "stopToFricScale", lp.clusterBlendStopToFricScale);
  getNumFrom(*cb, "nasalToFricScale", lp.clusterBlendNasalToFricScale);
  getNumFrom(*cb, "liquidToStopScale", lp.clusterBlendLiquidToStopScale);
  getNumFrom(*cb, "liquidToFricScale", lp.clusterBlendLiquidToFricScale);
  getNumFrom(*cb, "fricToFricScale", lp.clusterBlendFricToFricScale);
  getNumFrom(*cb, "stopToStopScale", lp.clusterBlendStopToStopScale);
  getNumFrom(*cb, "defaultPairScale", lp.clusterBlendDefaultPairScale);
  getNumFrom(*cb, "homorganicScale", lp.clusterBlendHomorganicScale);
  getNumFrom(*cb, "wordBoundaryScale", lp.clusterBlendWordBoundaryScale);
  getNumFrom(*cb, "f1Scale", lp.clusterBlendF1Scale);
  getNumFrom(*cb, "forwardDriftStrength", lp.clusterBlendForwardDriftStrength);
}

  getBool("huShortAVowelEnabled", lp.huShortAVowelEnabled);
  {
    const yaml_min::Node* n = settings.get("huShortAVowelKey");
    if (n && n->isScalar()) lp.huShortAVowelKey = utf8ToU32(n->scalar);
  }
  getNum("huShortAVowelScale", lp.huShortAVowelScale);

  getBool("englishLongUShortenEnabled", lp.englishLongUShortenEnabled);
  {
    const yaml_min::Node* n = settings.get("englishLongUKey");
    if (n && n->isScalar()) lp.englishLongUKey = utf8ToU32(n->scalar);
  }
  getNum("englishLongUWordFinalScale", lp.englishLongUWordFinalScale);

  getNum("defaultPreFormantGain", lp.defaultPreFormantGain);
  getNum("defaultOutputGain", lp.defaultOutputGain);

  getNum("defaultVibratoPitchOffset", lp.defaultVibratoPitchOffset);
  getNum("defaultVibratoSpeed", lp.defaultVibratoSpeed);
  getNum("defaultVoiceTurbulenceAmplitude", lp.defaultVoiceTurbulenceAmplitude);
  getNum("defaultGlottalOpenQuotient", lp.defaultGlottalOpenQuotient);

  getBool("stripAllophoneDigits", lp.stripAllophoneDigits);
  getBool("stripHyphen", lp.stripHyphen);

  getBool("tonal", lp.tonal);
  getBool("toneDigitsEnabled", lp.toneDigitsEnabled);

  // Optional: toneContoursMode: absolute|relative
  {
    std::string mode;
    getStr("toneContoursMode", mode);
    if (!mode.empty()) {
      std::string m;
      m.reserve(mode.size());
      for (char c : mode) m.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      if (m == "relative") lp.toneContoursAbsolute = false;
      if (m == "absolute") lp.toneContoursAbsolute = true;
    }
  }
  // Optional direct boolean override.
  getBool("toneContoursAbsolute", lp.toneContoursAbsolute);
}

static void mergeAliases(LanguagePack& lp, const yaml_min::Node& aliases) {
  if (!aliases.isMap()) return;
  for (const auto& kv : aliases.map) {
    const std::string& from = kv.first;
    if (!kv.second.isScalar()) continue;
    lp.aliases[utf8ToU32(from)] = utf8ToU32(kv.second.scalar);
  }
}

static void mergeClasses(LanguagePack& lp, const yaml_min::Node& classes) {
  if (!classes.isMap()) return;
  for (const auto& kv : classes.map) {
    const std::string& cname = kv.first;
    const yaml_min::Node& seq = kv.second;
    if (!seq.isSeq()) continue;
    std::vector<std::u32string> items;
    for (const auto& it : seq.seq) {
      if (it.isScalar()) items.push_back(utf8ToU32(it.scalar));
    }
    lp.classes[cname] = std::move(items);
  }
}

static void parseWhen(const yaml_min::Node& whenNode, RuleWhen& when) {
  if (!whenNode.isMap()) return;
  {
    const yaml_min::Node* n = whenNode.get("atWordStart");
    bool b;
    if (n && n->asBool(b)) when.atWordStart = b;
  }
  {
    const yaml_min::Node* n = whenNode.get("atWordEnd");
    bool b;
    if (n && n->asBool(b)) when.atWordEnd = b;
  }
  {
    const yaml_min::Node* n = whenNode.get("beforeClass");
    if (n && n->isScalar()) when.beforeClass = n->scalar;
  }
  {
    const yaml_min::Node* n = whenNode.get("afterClass");
    if (n && n->isScalar()) when.afterClass = n->scalar;
  }
  {
    const yaml_min::Node* n = whenNode.get("notBeforeClass");
    if (n && n->isScalar()) when.notBeforeClass = n->scalar;
  }
  {
    const yaml_min::Node* n = whenNode.get("notAfterClass");
    if (n && n->isScalar()) when.notAfterClass = n->scalar;
  }
  // Cross-word conditions
  {
    const yaml_min::Node* n = whenNode.get("nextWordStartsClass");
    if (n && n->isScalar()) when.nextWordStartsClass = n->scalar;
  }
  {
    const yaml_min::Node* n = whenNode.get("nextWordStartsNotClass");
    if (n && n->isScalar()) when.nextWordStartsNotClass = n->scalar;
  }
  {
    const yaml_min::Node* n = whenNode.get("prevWordEndsClass");
    if (n && n->isScalar()) when.prevWordEndsClass = n->scalar;
  }
  {
    const yaml_min::Node* n = whenNode.get("prevWordEndsNotClass");
    if (n && n->isScalar()) when.prevWordEndsNotClass = n->scalar;
  }
}

static bool parseReplacementList(const yaml_min::Node& node, std::vector<ReplacementRule>& out) {
  if (!node.isSeq()) return false;
  for (const auto& item : node.seq) {
    if (!item.isMap()) continue;
    const yaml_min::Node* fromN = item.get("from");
    const yaml_min::Node* toN = item.get("to");
    if (!fromN || !fromN->isScalar() || !toN) continue;

    ReplacementRule r;
    r.from = utf8ToU32(fromN->scalar);

    if (toN->isScalar()) {
      r.to.push_back(utf8ToU32(toN->scalar));
    } else if (toN->isSeq()) {
      for (const auto& c : toN->seq) {
        if (c.isScalar()) r.to.push_back(utf8ToU32(c.scalar));
      }
    }

    const yaml_min::Node* whenN = item.get("when");
    if (whenN) parseWhen(*whenN, r.when);

    if (!r.from.empty() && !r.to.empty()) out.push_back(std::move(r));
  }
  return true;
}

static bool parseTransformRule(const yaml_min::Node& node, TransformRule& out) {
  if (!node.isMap()) return false;

  auto parseMatchBool = [&](const char* k, int& field) {
    const yaml_min::Node* n = node.get(k);
    if (!n) return;
    bool b;
    if (n->asBool(b)) field = b ? 1 : 0;
  };

  // We accept either top-level keys or a nested 'match:' map.
  const yaml_min::Node* matchNode = node.get("match");
  const yaml_min::Node* m = (matchNode && matchNode->isMap()) ? matchNode : &node;

  auto matchBoolFrom = [&](const yaml_min::Node* mm, const char* k, int& field) {
    const yaml_min::Node* n = mm->get(k);
    if (!n) return;
    bool b;
    if (n->asBool(b)) field = b ? 1 : 0;
  };

  matchBoolFrom(m, "isVowel", out.isVowel);
  matchBoolFrom(m, "isVoiced", out.isVoiced);
  matchBoolFrom(m, "isStop", out.isStop);
  matchBoolFrom(m, "isAfricate", out.isAfricate);
  matchBoolFrom(m, "isNasal", out.isNasal);
  matchBoolFrom(m, "isLiquid", out.isLiquid);
  matchBoolFrom(m, "isSemivowel", out.isSemivowel);
  matchBoolFrom(m, "isTap", out.isTap);
  matchBoolFrom(m, "isTrill", out.isTrill);
  matchBoolFrom(m, "isFricativeLike", out.isFricativeLike);

  auto parseFieldOp = [&](const yaml_min::Node* mapNode, std::unordered_map<FieldId, double>& dest) {
    if (!mapNode || !mapNode->isMap()) return;
    for (const auto& kv : mapNode->map) {
      FieldId id;
      if (!parseFieldId(kv.first, id)) continue;
      double v;
      if (!kv.second.asNumber(v)) continue;
      dest[id] = v;
    }
  };

  parseFieldOp(node.get("set"), out.set);
  parseFieldOp(node.get("scale"), out.scale);
  parseFieldOp(node.get("add"), out.add);

  return true;
}

static void mergeTransforms(LanguagePack& lp, const yaml_min::Node& transforms) {
  if (!transforms.isSeq()) return;
  for (const auto& item : transforms.seq) {
    TransformRule tr;
    if (parseTransformRule(item, tr)) lp.transforms.push_back(std::move(tr));
  }
}

static bool parseIntonationClause(const yaml_min::Node& node, IntonationClause& out) {
  if (!node.isMap()) return false;
  auto getInt = [&](const char* k, int& field) {
    const yaml_min::Node* n = node.get(k);
    double v;
    if (n && n->asNumber(v)) field = static_cast<int>(v);
  };

  getInt("preHeadStart", out.preHeadStart);
  getInt("preHeadEnd", out.preHeadEnd);
  getInt("headExtendFrom", out.headExtendFrom);
  getInt("headStart", out.headStart);
  getInt("headEnd", out.headEnd);
  getInt("headStressEndDelta", out.headStressEndDelta);
  getInt("headUnstressedRunStartDelta", out.headUnstressedRunStartDelta);
  getInt("headUnstressedRunEndDelta", out.headUnstressedRunEndDelta);
  getInt("nucleus0Start", out.nucleus0Start);
  getInt("nucleus0End", out.nucleus0End);
  getInt("nucleusStart", out.nucleusStart);
  getInt("nucleusEnd", out.nucleusEnd);
  getInt("tailStart", out.tailStart);
  getInt("tailEnd", out.tailEnd);

  const yaml_min::Node* steps = node.get("headSteps");
  if (steps && steps->isSeq()) {
    out.headSteps.clear();
    for (const auto& it : steps->seq) {
      double v;
      if (it.asNumber(v)) out.headSteps.push_back(static_cast<int>(v));
    }
  }

  return true;
}

static void mergeIntonation(LanguagePack& lp, const yaml_min::Node& node) {
  if (!node.isMap()) return;
  for (const auto& kv : node.map) {
    if (kv.first.empty()) continue;
    char c = kv.first[0];
    if (c != '.' && c != ',' && c != '?' && c != '!') continue;

    IntonationClause clause = lp.intonation.count(c) ? lp.intonation[c] : IntonationClause{};
    parseIntonationClause(kv.second, clause);
    // Ensure headSteps is not empty.
    if (clause.headSteps.empty()) {
      clause.headSteps = {100,75,50,25,0};
    }
    lp.intonation[c] = std::move(clause);
  }
}

static void mergeToneContours(LanguagePack& lp, const yaml_min::Node& node) {
  if (!node.isMap()) return;
  for (const auto& kv : node.map) {
    const std::u32string toneKey = utf8ToU32(kv.first);
    const yaml_min::Node& v = kv.second;
    std::vector<int> pts;
    if (v.isSeq()) {
      for (const auto& it : v.seq) {
        double n;
        if (it.asNumber(n)) pts.push_back(static_cast<int>(n));
      }
    } else if (v.isScalar()) {
      // Allow a single number.
      double n;
      if (v.asNumber(n)) pts.push_back(static_cast<int>(n));
    }
    if (!pts.empty()) lp.toneContours[toneKey] = std::move(pts);
  }
}

static void mergeNormalization(LanguagePack& lp, const yaml_min::Node& norm) {
  if (!norm.isMap()) return;

  const yaml_min::Node* aliases = norm.get("aliases");
  if (aliases) mergeAliases(lp, *aliases);

  const yaml_min::Node* classes = norm.get("classes");
  if (classes) mergeClasses(lp, *classes);

  const yaml_min::Node* pre = norm.get("preReplacements");
  if (pre) parseReplacementList(*pre, lp.preReplacements);

  const yaml_min::Node* repl = norm.get("replacements");
  if (repl) parseReplacementList(*repl, lp.replacements);

  const yaml_min::Node* stripDigits = norm.get("stripAllophoneDigits");
  if (stripDigits) {
    bool b;
    if (stripDigits->asBool(b)) lp.stripAllophoneDigits = b;
  }

  const yaml_min::Node* stripHyphen = norm.get("stripHyphen");
  if (stripHyphen) {
    bool b;
    if (stripHyphen->asBool(b)) lp.stripHyphen = b;
  }

  // skipReplacements: child packs can remove inherited rules by (from, to) match.
  const yaml_min::Node* skip = norm.get("skipReplacements");
  if (skip && skip->isSeq()) {
    for (const auto& item : skip->seq) {
      if (!item.isMap()) continue;
      const yaml_min::Node* fromN = item.get("from");
      const yaml_min::Node* toN = item.get("to");
      if (!fromN || !fromN->isScalar()) continue;
      LanguagePack::SkipRule sr;
      sr.from = utf8ToU32(fromN->scalar);
      if (toN && toN->isScalar()) sr.to = utf8ToU32(toN->scalar);
      lp.skipReplacements.push_back(std::move(sr));
    }
  }
}

static bool mergeLanguageFile(const fs::path& path, PackSet& out, std::string& outError) {
  yaml_min::Node root;
  std::string yamlErr;
  if (!yaml_min::loadFile(path.string(), root, yamlErr)) {
    outError = yamlErr;
    return false;
  }

  // settings:
  if (const yaml_min::Node* s = root.get("settings")) {
    mergeSettings(out.lang, *s);
  }

  // normalization:
  if (const yaml_min::Node* n = root.get("normalization")) {
    mergeNormalization(out.lang, *n);
  }

  // transforms:
  if (const yaml_min::Node* t = root.get("transforms")) {
    mergeTransforms(out.lang, *t);
  }

  // intonation:
  if (const yaml_min::Node* i = root.get("intonation")) {
    mergeIntonation(out.lang, *i);
  }

  // toneContours:
  if (const yaml_min::Node* tc = root.get("toneContours")) {
    mergeToneContours(out.lang, *tc);
  }

  // phoneme overrides:
  if (const yaml_min::Node* p = root.get("phonemes")) {
    if (p->isMap()) {
      for (const auto& kv : p->map) {
        const std::u32string phonKey = utf8ToU32(kv.first);
        const yaml_min::Node& defNode = kv.second;
        if (!defNode.isMap()) continue;

        PhonemeDef def;
        def.key = phonKey;

        for (const auto& ekv : defNode.map) {
          const std::string& fieldName = ekv.first;
          const yaml_min::Node& val = ekv.second;

          if (!fieldName.empty() && fieldName[0] == '_') {
            std::uint32_t bit = parseFlagKey(fieldName);
            if (bit != 0) {
              bool b;
              if (val.asBool(b) && b) def.flags |= bit;
            }
            continue;
          }
          
          // Parse frameEx: block for per-phoneme voice quality
          if (fieldName == "frameEx" && val.isMap()) {
            for (const auto& fxkv : val.map) {
              const std::string& fxKey = fxkv.first;
              double fxVal;
              if (!fxkv.second.asNumber(fxVal)) continue;
              
              if (fxKey == "creakiness") {
                def.hasCreakiness = true;
                def.creakiness = fxVal;
              } else if (fxKey == "breathiness") {
                def.hasBreathiness = true;
                def.breathiness = fxVal;
              } else if (fxKey == "jitter") {
                def.hasJitter = true;
                def.jitter = fxVal;
              } else if (fxKey == "shimmer") {
                def.hasShimmer = true;
                def.shimmer = fxVal;
              } else if (fxKey == "sharpness") {
                def.hasSharpness = true;
                def.sharpness = fxVal;
              } else if (fxKey == "endCf1") {
                def.hasEndCf1 = true;
                def.endCf1 = fxVal;
              } else if (fxKey == "endCf2") {
                def.hasEndCf2 = true;
                def.endCf2 = fxVal;
              } else if (fxKey == "endCf3") {
                def.hasEndCf3 = true;
                def.endCf3 = fxVal;
              } else if (fxKey == "endPf1") {
                def.hasEndPf1 = true;
                def.endPf1 = fxVal;
              } else if (fxKey == "endPf2") {
                def.hasEndPf2 = true;
                def.endPf2 = fxVal;
              } else if (fxKey == "endPf3") {
                def.hasEndPf3 = true;
                def.endPf3 = fxVal;
              }
            }
            continue;
          }

          // Micro-event burst shaping fields (top-level, not frame fields)
          if (fieldName == "burstDurationMs") {
            double v; if (val.asNumber(v)) { def.hasBurstDurationMs = true; def.burstDurationMs = v; }
            continue;
          }
          if (fieldName == "burstDecayRate") {
            double v; if (val.asNumber(v)) { def.hasBurstDecayRate = true; def.burstDecayRate = v; }
            continue;
          }
          if (fieldName == "burstSpectralTilt") {
            double v; if (val.asNumber(v)) { def.hasBurstSpectralTilt = true; def.burstSpectralTilt = v; }
            continue;
          }
          if (fieldName == "voiceBarAmplitude") {
            double v; if (val.asNumber(v)) { def.hasVoiceBarAmplitude = true; def.voiceBarAmplitude = v; }
            continue;
          }
          if (fieldName == "voiceBarF1") {
            double v; if (val.asNumber(v)) { def.hasVoiceBarF1 = true; def.voiceBarF1 = v; }
            continue;
          }
          if (fieldName == "releaseSpreadMs") {
            double v; if (val.asNumber(v)) { def.hasReleaseSpreadMs = true; def.releaseSpreadMs = v; }
            continue;
          }
          if (fieldName == "fricAttackMs") {
            double v; if (val.asNumber(v)) { def.hasFricAttackMs = true; def.fricAttackMs = v; }
            continue;
          }
          if (fieldName == "fricDecayMs") {
            double v; if (val.asNumber(v)) { def.hasFricDecayMs = true; def.fricDecayMs = v; }
            continue;
          }
          if (fieldName == "durationScale") {
            double v; if (val.asNumber(v)) { def.hasDurationScale = true; def.durationScale = v; }
            continue;
          }
          if (fieldName == "closureGapMs") {
            double v; if (val.asNumber(v)) { def.hasClosureGapMs = true; def.closureGapMs = v; }
            continue;
          }

          FieldId id;
          if (!parseFieldId(fieldName, id)) continue;
          double num;
          if (!val.asNumber(num)) continue;
          int idx = static_cast<int>(id);
          def.field[idx] = num;
          def.setMask |= (1ull << idx);
        }

        out.phonemes[phonKey] = def;
      }
    }
  }

  return true;
}

static std::vector<std::string> splitLangParts(const std::string& langTag) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : langTag) {
    if (c == '-') {
      if (!cur.empty()) parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) parts.push_back(cur);
  return parts;
}

static std::vector<std::string> buildLangFileChain(const std::string& langTag) {
  std::vector<std::string> chain;
  chain.push_back("default");

  std::vector<std::string> parts = splitLangParts(langTag);
  std::string cur;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (!cur.empty()) cur += "-";
    cur += parts[i];
    chain.push_back(cur);
  }

  // Remove duplicates while keeping order.
  std::vector<std::string> unique;
  for (const auto& x : chain) {
    if (std::find(unique.begin(), unique.end(), x) == unique.end()) unique.push_back(x);
  }
  return unique;
}

// Load a stress dictionary TSV file (word<TAB>digits).
// Silent on failure — empty dict just means no stress corrections.
static void loadStressDict(
    const std::string& path,
    std::unordered_map<std::string, std::vector<int>>& dict)
{
  std::ifstream f(path);
  if (!f.is_open()) return;

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    // Remove trailing \r (Windows line endings)
    if (!line.empty() && line.back() == '\r') line.pop_back();

    const auto tab = line.find('\t');
    if (tab == std::string::npos) continue;

    std::string word = line.substr(0, tab);
    if (word.empty()) continue;

    std::vector<int> digits;
    std::istringstream ss(line.substr(tab + 1));
    int d;
    while (ss >> d) {
      digits.push_back(d);
    }
    if (digits.empty()) continue;

    dict.emplace(std::move(word), std::move(digits));
  }
}

// Load a compound word TSV file (word<TAB>half1 half2 ...).
// Silent on failure — empty map just means no compound stress fallback.
static void loadCompoundMap(
    const std::string& path,
    std::unordered_map<std::string, std::vector<std::string>>& map)
{
  std::ifstream f(path);
  if (!f.is_open()) return;

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    if (!line.empty() && line.back() == '\r') line.pop_back();

    const auto tab = line.find('\t');
    if (tab == std::string::npos) continue;

    std::string word = line.substr(0, tab);
    if (word.empty()) continue;

    std::vector<std::string> halves;
    std::istringstream ss(line.substr(tab + 1));
    std::string part;
    while (ss >> part) {
      halves.push_back(std::move(part));
    }
    if (halves.size() < 2) continue;

    map.emplace(std::move(word), std::move(halves));
  }
}

static void loadLetterDict(
    const std::string& path,
    std::unordered_map<std::string, std::string>& dict)
{
  std::ifstream f(path);
  if (!f.is_open()) return;

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    if (line.back() == '\r') line.pop_back();

    const auto tab = line.find('\t');
    if (tab == std::string::npos) continue;

    std::string key = line.substr(0, tab);
    std::string desc = line.substr(tab + 1);
    if (key.empty()) continue;

    dict.emplace(std::move(key), std::move(desc));
  }
}

static void loadPronDict(
    const std::string& path,
    PronDict& dict,
    const std::string& source)
{
  std::ifstream f(path);
  if (!f.is_open()) return;

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    if (!line.empty() && line.back() == '\r') line.pop_back();

    // Parse up to 5 tab-separated columns.
    std::vector<std::string> cols;
    size_t pos = 0;
    for (int i = 0; i < 5; ++i) {
      size_t tab = line.find('\t', pos);
      if (tab == std::string::npos) {
        cols.push_back(line.substr(pos));
        break;
      }
      cols.push_back(line.substr(pos, tab - pos));
      pos = tab + 1;
    }
    if (cols.empty() || cols[0].empty()) continue;
    if (cols.size() < 2 || cols[1].empty()) continue;

    DictEntry e;
    e.fromText = cols[0];
    e.toText   = cols[1];
    e.fromIpa  = cols.size() > 2 ? cols[2] : "";
    e.toIpa    = cols.size() > 3 ? cols[3] : "";
    e.category = cols.size() > 4 ? cols[4] : "";
    e.source   = source;

    // Store with original case. dictReplaceInText tries exact case first,
    // then lowercase fallback — so "Parton" (capitalized) and "parton"
    // (lowercase) can coexist as separate entries.
    dict.entries[e.fromText] = std::move(e);
  }

  // Rebuild dynamic category list.
  std::unordered_set<std::string> catSet;
  for (const auto& kv : dict.entries) {
    if (!kv.second.category.empty())
      catSet.insert(kv.second.category);
  }
  dict.categories.assign(catSet.begin(), catSet.end());
}

bool loadPackSet(
  const std::string& packDir,
  const std::string& langTag,
  PackSet& out,
  std::string& outError,
  const std::string& overrideDir
) {
  std::string err;
  fs::path packsRoot = findPacksRoot(packDir, err);
  if (packsRoot.empty()) {
    outError = err;
    return false;
  }

  if (!loadPhonemes(packsRoot, out, outError)) {
    return false;
  }

  out.lang = LanguagePack{};
  out.lang.langTag = normalizeLangTag(langTag);
  applyLanguageDefaults(out.lang);

  const fs::path langDir = packsRoot / "lang";
  const auto chain = buildLangFileChain(out.lang.langTag);

  // Optional override directory: check <overrideDir>/packs/lang/ first.
  fs::path overrideLangDir;
  if (!overrideDir.empty()) {
    overrideLangDir = fs::path(overrideDir) / "packs" / "lang";
  }

  for (const auto& name : chain) {
    // Try override dir first, then fall back to bundle.
    bool loaded = false;
    if (!overrideLangDir.empty()) {
      fs::path ovFile = overrideLangDir / (name + ".yaml");
      if (fs::exists(ovFile)) {
        if (!mergeLanguageFile(ovFile, out, outError)) {
          return false;
        }
        loaded = true;
      }
    }
    if (!loaded) {
      fs::path file = langDir / (name + ".yaml");
      if (fs::exists(file)) {
        if (!mergeLanguageFile(file, out, outError)) {
          return false;
        }
      }
    }
  }

  // Apply skipReplacements: remove inherited rules that child packs opted out of.
  if (!out.lang.skipReplacements.empty()) {
    auto matchesSkip = [&](const ReplacementRule& rule) {
      for (const auto& sr : out.lang.skipReplacements) {
        if (rule.from != sr.from) continue;
        // If skip specifies a 'to', match it against the rule's first candidate.
        if (!sr.to.empty()) {
          if (rule.to.empty() || rule.to[0] != sr.to) continue;
        }
        return true;
      }
      return false;
    };
    auto& repl = out.lang.replacements;
    repl.erase(std::remove_if(repl.begin(), repl.end(), matchesSkip), repl.end());
    auto& pre = out.lang.preReplacements;
    pre.erase(std::remove_if(pre.begin(), pre.end(), matchesSkip), pre.end());
  }

  // Auto-migrate old stressDiv values into prominence weights when prominence
  // is enabled but the user hasn't customized the weights.
  if (out.lang.prominenceEnabled) {
    if (out.lang.prominencePrimaryStressWeight == 1.4 &&
        out.lang.primaryStressDiv != 1.4) {
      out.lang.prominencePrimaryStressWeight = out.lang.primaryStressDiv;
    }
    if (out.lang.prominenceSecondaryStressWeight == 1.1 &&
        out.lang.secondaryStressDiv != 1.1) {
      out.lang.prominenceSecondaryStressWeight = out.lang.secondaryStressDiv;
    }
  }

  // Load stress dictionary (if one exists for this language).
  // Check override dir first, then fall back to bundle.
  {
    bool loaded = false;
    if (!overrideDir.empty()) {
      const fs::path ovDict = fs::path(overrideDir) / "packs" / "dict" / (out.lang.langTag + "-stress.tsv");
      if (fs::exists(ovDict)) {
        loadStressDict(ovDict.string(), out.stressDict);
        loaded = true;
      }
    }
    if (!loaded) {
      const fs::path dictPath = packsRoot / "dict" / (out.lang.langTag + "-stress.tsv");
      loadStressDict(dictPath.string(), out.stressDict);
    }
  }

  // Load compound word map (if one exists for this language).
  // Check override dir first, then fall back to bundle.
  {
    const auto& tag = out.lang.langTag;
    bool loaded = false;
    if (!overrideDir.empty()) {
      const fs::path ovComp = fs::path(overrideDir) / "packs" / "dict" / (tag + "-compounds.tsv");
      if (fs::exists(ovComp)) {
        loadCompoundMap(ovComp.string(), out.compoundMap);
        loaded = true;
      }
    }
    if (!loaded) {
      const fs::path compoundPath = packsRoot / "dict" / (tag + "-compounds.tsv");
      loadCompoundMap(compoundPath.string(), out.compoundMap);
      // Fallback to base language (e.g., "en" for "en-us").
      if (out.compoundMap.empty()) {
        const auto dash = tag.find('-');
        if (dash != std::string::npos) {
          const fs::path basePath = packsRoot / "dict" / (tag.substr(0, dash) + "-compounds.tsv");
          loadCompoundMap(basePath.string(), out.compoundMap);
        }
      }
    }
  }

  // Load pronunciation dictionary (if one exists for this language).
  // Check override dir first, then fall back to bundle.
  {
    const auto& tag = out.lang.langTag;
    bool loaded = false;
    if (!overrideDir.empty()) {
      const fs::path ovDict = fs::path(overrideDir) / "packs" / "dict" / (tag + "-dict.tsv");
      if (fs::exists(ovDict)) {
        loadPronDict(ovDict.string(), out.pronDict, "main");
        loaded = true;
      }
    }
    if (!loaded) {
      const fs::path dictPath = packsRoot / "dict" / (tag + "-dict.tsv");
      loadPronDict(dictPath.string(), out.pronDict, "main");
      // Fallback to base language (e.g., "en" for "en-us").
      if (out.pronDict.entries.empty()) {
        const auto dash = tag.find('-');
        if (dash != std::string::npos) {
          const fs::path basePath = packsRoot / "dict" / (tag.substr(0, dash) + "-dict.tsv");
          loadPronDict(basePath.string(), out.pronDict, "main");
        }
      }
    }
  }

  // Load user pronunciation dictionary (entries created by the user).
  // These overlay the main dict — user entries win on conflict.
  // Check override dir first, then fall back to bundle.
  {
    const auto& tag = out.lang.langTag;
    bool loaded = false;
    if (!overrideDir.empty()) {
      const fs::path ovUser = fs::path(overrideDir) / "packs" / "dict" / (tag + "-user.tsv");
      if (fs::exists(ovUser)) {
        loadPronDict(ovUser.string(), out.pronDict, "user");
        loaded = true;
      }
    }
    if (!loaded) {
      const fs::path userPath = packsRoot / "dict" / (tag + "-user.tsv");
      if (fs::exists(userPath)) {
        loadPronDict(userPath.string(), out.pronDict, "user");
      }
      // Fallback to base language (e.g., "en" for "en-us").
      if (!loaded) {
        const auto dash = tag.find('-');
        if (dash != std::string::npos) {
          const fs::path basePath = packsRoot / "dict" / (tag.substr(0, dash) + "-user.tsv");
          if (fs::exists(basePath))
            loadPronDict(basePath.string(), out.pronDict, "user");
        }
      }
    }
  }

  // Load character/letter dictionary (if one exists for this language).
  // Check override dir first, then fall back to bundle.
  {
    const auto& tag = out.lang.langTag;
    bool loaded = false;
    if (!overrideDir.empty()) {
      const fs::path ovLetters = fs::path(overrideDir) / "packs" / "dict" / (tag + "-letters.tsv");
      if (fs::exists(ovLetters)) {
        loadLetterDict(ovLetters.string(), out.letterDict);
        loaded = true;
      }
    }
    if (!loaded) {
      const fs::path lettersPath = packsRoot / "dict" / (tag + "-letters.tsv");
      loadLetterDict(lettersPath.string(), out.letterDict);
      if (out.letterDict.empty()) {
        const auto dash = tag.find('-');
        if (dash != std::string::npos) {
          const fs::path basePath = packsRoot / "dict" / (tag.substr(0, dash) + "-letters.tsv");
          loadLetterDict(basePath.string(), out.letterDict);
        }
      }
    }
  }

  // Ensure headSteps exists for each clause.
  for (auto& kv : out.lang.intonation) {
    if (kv.second.headSteps.empty()) kv.second.headSteps = {100,75,50,25,0};
  }

  // Build sorted phoneme keys for greedy longest-match tokenization.
  // Keys are sorted by length descending so longer keys match first.
  out.sortedPhonemeKeys.clear();
  out.sortedPhonemeKeys.reserve(out.phonemes.size());
  for (const auto& kv : out.phonemes) {
    out.sortedPhonemeKeys.push_back(kv.first);
  }
  std::sort(out.sortedPhonemeKeys.begin(), out.sortedPhonemeKeys.end(),
    [](const std::u32string& a, const std::u32string& b) {
      // Sort by length descending; if equal, by lexicographic order for stability
      if (a.size() != b.size()) return a.size() > b.size();
      return a < b;
    });

  return true;
}

void loadDictFiles(
  const std::string& packDir,
  const std::string& langTag,
  std::unordered_map<std::string, std::vector<int>>& stressDict,
  std::unordered_map<std::string, std::vector<std::string>>& compoundMap,
  PronDict& pronDict,
  std::unordered_map<std::string, std::string>& letterDict,
  const std::string& overrideDir)
{
  namespace fs = std::filesystem;
  // packDir may be either the parent containing "packs/" or the packs dir itself.
  const fs::path candidate = fs::path(packDir) / "packs";
  const fs::path packsRoot = fs::exists(candidate) ? candidate : fs::path(packDir);
  const std::string tag = normalizeLangTag(langTag);
  const auto dash = tag.find('-');
  const std::string baseTag = (dash != std::string::npos) ? tag.substr(0, dash) : "";

  // Stress dict.
  {
    bool loaded = false;
    if (!overrideDir.empty()) {
      const fs::path p = fs::path(overrideDir) / "packs" / "dict" / (tag + "-stress.tsv");
      if (fs::exists(p)) { loadStressDict(p.string(), stressDict); loaded = true; }
    }
    if (!loaded) {
      loadStressDict((packsRoot / "dict" / (tag + "-stress.tsv")).string(), stressDict);
      if (stressDict.empty() && !baseTag.empty())
        loadStressDict((packsRoot / "dict" / (baseTag + "-stress.tsv")).string(), stressDict);
    }
  }
  // Compound map.
  {
    bool loaded = false;
    if (!overrideDir.empty()) {
      const fs::path p = fs::path(overrideDir) / "packs" / "dict" / (tag + "-compounds.tsv");
      if (fs::exists(p)) { loadCompoundMap(p.string(), compoundMap); loaded = true; }
    }
    if (!loaded) {
      loadCompoundMap((packsRoot / "dict" / (tag + "-compounds.tsv")).string(), compoundMap);
      if (compoundMap.empty() && !baseTag.empty())
        loadCompoundMap((packsRoot / "dict" / (baseTag + "-compounds.tsv")).string(), compoundMap);
    }
  }
  // Pronunciation dict.
  {
    bool loaded = false;
    if (!overrideDir.empty()) {
      const fs::path p = fs::path(overrideDir) / "packs" / "dict" / (tag + "-dict.tsv");
      if (fs::exists(p)) { loadPronDict(p.string(), pronDict, "main"); loaded = true; }
    }
    if (!loaded) {
      loadPronDict((packsRoot / "dict" / (tag + "-dict.tsv")).string(), pronDict, "main");
      if (pronDict.entries.empty() && !baseTag.empty())
        loadPronDict((packsRoot / "dict" / (baseTag + "-dict.tsv")).string(), pronDict, "main");
    }
  }
  // User pronunciation dict (overlays main).
  {
    bool loaded = false;
    if (!overrideDir.empty()) {
      const fs::path p = fs::path(overrideDir) / "packs" / "dict" / (tag + "-user.tsv");
      if (fs::exists(p)) { loadPronDict(p.string(), pronDict, "user"); loaded = true; }
    }
    if (!loaded) {
      const fs::path userPath = packsRoot / "dict" / (tag + "-user.tsv");
      if (fs::exists(userPath))
        loadPronDict(userPath.string(), pronDict, "user");
      if (!baseTag.empty()) {
        const fs::path basePath = packsRoot / "dict" / (baseTag + "-user.tsv");
        if (fs::exists(basePath))
          loadPronDict(basePath.string(), pronDict, "user");
      }
    }
  }
  // Letter/character dict.
  {
    bool loaded = false;
    if (!overrideDir.empty()) {
      const fs::path p = fs::path(overrideDir) / "packs" / "dict" / (tag + "-letters.tsv");
      if (fs::exists(p)) { loadLetterDict(p.string(), letterDict); loaded = true; }
    }
    if (!loaded) {
      loadLetterDict((packsRoot / "dict" / (tag + "-letters.tsv")).string(), letterDict);
      if (letterDict.empty() && !baseTag.empty())
        loadLetterDict((packsRoot / "dict" / (baseTag + "-letters.tsv")).string(), letterDict);
    }
  }
}

bool hasPhoneme(const PackSet& pack, const std::u32string& key) {
  return pack.phonemes.find(key) != pack.phonemes.end();
}

// ── Public helpers for mobile pack settings editor ──

// Flatten a yaml_min::Node settings block into "key\tvalue\n" lines.
// Nested maps use dot notation: "boundarySmoothing.enabled\ttrue\n"
static void flattenSettings(const yaml_min::Node& node, const std::string& prefix,
                            std::vector<std::pair<std::string, std::string>>& out) {
  if (!node.isMap()) return;
  for (const auto& key : node.keyOrder) {
    auto it = node.map.find(key);
    if (it == node.map.end()) continue;
    const auto& child = it->second;
    std::string fullKey = prefix.empty() ? key : (prefix + "." + key);
    if (child.isScalar()) {
      out.emplace_back(fullKey, child.scalar);
    } else if (child.isMap()) {
      flattenSettings(child, fullKey, out);
    }
    // Skip sequences and other complex types — not scalar-editable.
  }
}

std::string getEffectiveSettings(const std::string& packDir, const std::string& langTag) {
  std::string err;
  fs::path packsRoot = findPacksRoot(packDir, err);
  if (packsRoot.empty()) return {};

  const std::string normalized = normalizeLangTag(langTag);
  const auto chain = buildLangFileChain(normalized);
  const fs::path langDir = packsRoot / "lang";

  // Collect all scalar settings from the file chain, later files override earlier.
  std::unordered_map<std::string, std::string> merged;
  std::vector<std::string> keyOrder;  // preserve first-seen order

  for (const auto& name : chain) {
    fs::path file = langDir / (name + ".yaml");
    if (!fs::exists(file)) continue;

    yaml_min::Node root;
    std::string parseErr;
    if (!yaml_min::loadFile(file.string(), root, parseErr)) continue;

    const yaml_min::Node* settings = root.get("settings");
    if (!settings || !settings->isMap()) continue;

    std::vector<std::pair<std::string, std::string>> flat;
    flattenSettings(*settings, "", flat);
    for (auto& [k, v] : flat) {
      if (merged.find(k) == merged.end()) {
        keyOrder.push_back(k);
      }
      merged[k] = v;
    }
  }

  // Build tab-separated output.
  std::string result;
  result.reserve(keyOrder.size() * 40);
  for (const auto& key : keyOrder) {
    result += key;
    result += '\t';
    result += merged[key];
    result += '\n';
  }
  return result;
}

std::vector<std::string> getAvailableLanguages(const std::string& packDir) {
  std::string err;
  fs::path packsRoot = findPacksRoot(packDir, err);
  if (packsRoot.empty()) return {};

  fs::path langDir = packsRoot / "lang";
  if (!fs::exists(langDir) || !fs::is_directory(langDir)) return {};

  std::vector<std::string> langs;
  for (const auto& entry : fs::directory_iterator(langDir)) {
    if (!entry.is_regular_file()) continue;
    std::string name = entry.path().stem().string();
    if (name == "default") continue;
    langs.push_back(name);
  }
  std::sort(langs.begin(), langs.end());
  return langs;
}

bool applySettingOverrides(LanguagePack& lp, const std::string& yamlSnippet) {
  if (yamlSnippet.empty()) return true;

  // Parse flat "key: value" lines. Support dot-notation by building nested nodes.
  // Simple approach: parse the snippet as YAML, wrap in a settings node.
  yaml_min::Node root;
  std::string err;
  if (!yaml_min::loadString(yamlSnippet, root, err)) return false;
  if (!root.isMap()) return false;

  // The snippet may contain flat keys ("yearSplittingEnabled: true") or
  // dot-notation keys ("boundarySmoothing.enabled: true"). Convert dot-notation
  // to nested nodes for mergeSettings.
  yaml_min::Node expanded;
  expanded.type = yaml_min::Node::Type::Map;

  for (const auto& key : root.keyOrder) {
    auto it = root.map.find(key);
    if (it == root.map.end()) continue;

    auto dotPos = key.find('.');
    if (dotPos == std::string::npos) {
      // Flat key — pass through.
      expanded.map[key] = it->second;
      expanded.keyOrder.push_back(key);
    } else {
      // Dot-notation — build nested structure.
      std::string outer = key.substr(0, dotPos);
      std::string inner = key.substr(dotPos + 1);
      auto& outerNode = expanded.map[outer];
      if (outerNode.type != yaml_min::Node::Type::Map) {
        outerNode.type = yaml_min::Node::Type::Map;
        expanded.keyOrder.push_back(outer);
      }
      yaml_min::Node scalar;
      scalar.type = yaml_min::Node::Type::Scalar;
      scalar.scalar = it->second.scalar;
      outerNode.map[inner] = scalar;
      outerNode.keyOrder.push_back(inner);
    }
  }

  mergeSettings(lp, expanded);
  return true;
}

} // namespace nvsp_frontend
