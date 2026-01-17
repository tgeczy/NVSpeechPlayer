#include "pack.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace nvsp_frontend {

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

  getNum("primaryStressDiv", lp.primaryStressDiv);
  getNum("secondaryStressDiv", lp.secondaryStressDiv);

  getBool("postStopAspirationEnabled", lp.postStopAspirationEnabled);
  {
    const yaml_min::Node* n = settings.get("postStopAspirationPhoneme");
    if (n && n->isScalar()) lp.postStopAspirationPhoneme = utf8ToU32(n->scalar);
  }

  getStr("stopClosureMode", lp.stopClosureMode);
  getBool("stopClosureClusterGapsEnabled", lp.stopClosureClusterGapsEnabled);
  // Optional: insert a tiny silence between consecutive queueIPA calls
  // to compensate for NVDA UI segmentation.
  getNum("segmentBoundaryGapMs", lp.segmentBoundaryGapMs);
  getNum("segmentBoundaryFadeMs", lp.segmentBoundaryFadeMs);
  getBool("stopClosureAfterNasalsEnabled", lp.stopClosureAfterNasalsEnabled);

  getNum("lengthenedScale", lp.lengthenedScale);
  getNum("lengthenedScaleHu", lp.lengthenedScaleHu);
  getBool("applyLengthenedScaleToVowelsOnly", lp.applyLengthenedScaleToVowelsOnly);

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

bool loadPackSet(
  const std::string& packDir,
  const std::string& langTag,
  PackSet& out,
  std::string& outError
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
  for (const auto& name : chain) {
    fs::path file = langDir / (name + ".yaml");
    if (fs::exists(file)) {
      if (!mergeLanguageFile(file, out, outError)) {
        return false;
      }
    }
  }

  // Ensure headSteps exists for each clause.
  for (auto& kv : out.lang.intonation) {
    if (kv.second.headSteps.empty()) kv.second.headSteps = {100,75,50,25,0};
  }

  return true;
}

bool hasPhoneme(const PackSet& pack, const std::u32string& key) {
  return pack.phonemes.find(key) != pack.phonemes.end();
}

} // namespace nvsp_frontend
