#include "nvsp_runtime.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <cctype>
#include <sstream>

namespace nvsp_editor {

static std::string wideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return {};
  std::string out(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
  out.resize(static_cast<size_t>(len - 1));
  return out;
}

static unsigned int msToSamples(double ms, int sampleRate) {
  if (ms <= 0) return 1;
  double s = (ms / 1000.0) * static_cast<double>(sampleRate);
  if (s < 1.0) s = 1.0;
  if (s > static_cast<double>(0xFFFFFFFFu)) s = static_cast<double>(0xFFFFFFFFu);
  return static_cast<unsigned int>(s);
}

static bool tryGetBool(const Node& mapNode, const char* key, bool& out) {
  const Node* n = mapNode.get(key);
  if (!n) return false;
  bool b = false;
  if (n->asBool(b)) {
    out = b;
    return true;
  }
  return false;
}

static bool tryGetNumber(const Node& mapNode, const char* key, double& out) {
  const Node* n = mapNode.get(key);
  if (!n) return false;
  double v = 0;
  if (n->asNumber(v)) {
    out = v;
    return true;
  }
  return false;
}

struct FieldMap {
  const char* name;
  speechPlayer_frameParam_t speechPlayer_frame_t::* member;
};

static const FieldMap kFieldMap[] = {
  {"voicePitch", &speechPlayer_frame_t::voicePitch},
  {"vibratoPitchOffset", &speechPlayer_frame_t::vibratoPitchOffset},
  {"vibratoSpeed", &speechPlayer_frame_t::vibratoSpeed},
  {"voiceTurbulenceAmplitude", &speechPlayer_frame_t::voiceTurbulenceAmplitude},
  {"glottalOpenQuotient", &speechPlayer_frame_t::glottalOpenQuotient},
  {"voiceAmplitude", &speechPlayer_frame_t::voiceAmplitude},
  {"aspirationAmplitude", &speechPlayer_frame_t::aspirationAmplitude},

  {"cf1", &speechPlayer_frame_t::cf1},
  {"cf2", &speechPlayer_frame_t::cf2},
  {"cf3", &speechPlayer_frame_t::cf3},
  {"cf4", &speechPlayer_frame_t::cf4},
  {"cf5", &speechPlayer_frame_t::cf5},
  {"cf6", &speechPlayer_frame_t::cf6},
  {"cfN0", &speechPlayer_frame_t::cfN0},
  {"cfNP", &speechPlayer_frame_t::cfNP},

  {"cb1", &speechPlayer_frame_t::cb1},
  {"cb2", &speechPlayer_frame_t::cb2},
  {"cb3", &speechPlayer_frame_t::cb3},
  {"cb4", &speechPlayer_frame_t::cb4},
  {"cb5", &speechPlayer_frame_t::cb5},
  {"cb6", &speechPlayer_frame_t::cb6},
  {"cbN0", &speechPlayer_frame_t::cbN0},
  {"cbNP", &speechPlayer_frame_t::cbNP},

  {"caNP", &speechPlayer_frame_t::caNP},

  {"fricationAmplitude", &speechPlayer_frame_t::fricationAmplitude},

  {"pf1", &speechPlayer_frame_t::pf1},
  {"pf2", &speechPlayer_frame_t::pf2},
  {"pf3", &speechPlayer_frame_t::pf3},
  {"pf4", &speechPlayer_frame_t::pf4},
  {"pf5", &speechPlayer_frame_t::pf5},
  {"pf6", &speechPlayer_frame_t::pf6},

  {"pb1", &speechPlayer_frame_t::pb1},
  {"pb2", &speechPlayer_frame_t::pb2},
  {"pb3", &speechPlayer_frame_t::pb3},
  {"pb4", &speechPlayer_frame_t::pb4},
  {"pb5", &speechPlayer_frame_t::pb5},
  {"pb6", &speechPlayer_frame_t::pb6},

  {"pa1", &speechPlayer_frame_t::pa1},
  {"pa2", &speechPlayer_frame_t::pa2},
  {"pa3", &speechPlayer_frame_t::pa3},
  {"pa4", &speechPlayer_frame_t::pa4},
  {"pa5", &speechPlayer_frame_t::pa5},
  {"pa6", &speechPlayer_frame_t::pa6},

  {"parallelBypass", &speechPlayer_frame_t::parallelBypass},
  {"preFormantGain", &speechPlayer_frame_t::preFormantGain},
  {"outputGain", &speechPlayer_frame_t::outputGain},
  {"endVoicePitch", &speechPlayer_frame_t::endVoicePitch},
};

static size_t kFieldCount() {
  return sizeof(kFieldMap) / sizeof(kFieldMap[0]);
}

const std::vector<std::string>& NvspRuntime::frameParamNames() {
  static std::vector<std::string> names;
  if (names.empty()) {
    names.reserve(kFieldCount());
    for (const auto& f : kFieldMap) names.emplace_back(f.name);
  }
  return names;
}

static void applyPhonemeMapToFrame(const Node& phonemeMap, speechPlayer_frame_t& frame, bool& outIsVowel) {
  outIsVowel = false;

  // Defaults that keep the preview audible.
  frame.voicePitch = 120.0;
  frame.endVoicePitch = 120.0;
  frame.preFormantGain = 1.0;
  frame.outputGain = 1.0;

  bool isVowel = false;
  if (tryGetBool(phonemeMap, "_isVowel", isVowel)) {
    outIsVowel = isVowel;
  }

  for (const auto& f : kFieldMap) {
    double v = 0.0;
    if (tryGetNumber(phonemeMap, f.name, v)) {
      frame.*(f.member) = v;
    }
  }

  // If the table doesn't provide output gain, make it a little louder for preview.
  if (frame.outputGain <= 0.0) {
    frame.outputGain = 1.2;
  }
}

NvspRuntime::NvspRuntime() {
  // No static layout assumptions: we convert frames field-by-field in the callback.
  m_speech.frameParams.assign(frameParamNames().size(), 50);
}

NvspRuntime::~NvspRuntime() {
  unload();
}

void NvspRuntime::setSpeechSettings(const SpeechSettings& s) {
  m_speech = s;
  if (m_speech.voiceName.empty()) m_speech.voiceName = "Adam";
  // Normalize pauseMode (matches NVDA driver: off | short | long).
  {
    std::string pm = m_speech.pauseMode;
    for (auto& ch : pm) ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
    if (pm != "off" && pm != "short" && pm != "long") pm = "short";
    m_speech.pauseMode = pm;
  }
  if (m_speech.frameParams.size() != frameParamNames().size()) {
    m_speech.frameParams.assign(frameParamNames().size(), 50);
  }
}

SpeechSettings NvspRuntime::getSpeechSettings() const {
  return m_speech;
}

static int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static void applyMul(speechPlayer_frame_t& frame, speechPlayer_frameParam_t speechPlayer_frame_t::* member, double mul) {
  frame.*member = static_cast<speechPlayer_frameParam_t>(static_cast<double>(frame.*member) * mul);
}

static void applyAbs(speechPlayer_frame_t& frame, speechPlayer_frameParam_t speechPlayer_frame_t::* member, double val) {
  frame.*member = static_cast<speechPlayer_frameParam_t>(val);
}

static const FieldMap* findField(const char* name) {
  for (const auto& f : kFieldMap) {
    if (std::strcmp(f.name, name) == 0) return &f;
  }
  return nullptr;
}

void NvspRuntime::applySpeechSettingsToFrame(speechPlayer_frame_t& frame) const {
  // 1) Voice preset (from NVDA driver's __init__.py)
  const std::string voice = m_speech.voiceName.empty() ? "Adam" : m_speech.voiceName;

  auto mulByName = [&](const char* field, double mul) {
    const FieldMap* f = findField(field);
    if (f) applyMul(frame, f->member, mul);
  };
  auto absByName = [&](const char* field, double val) {
    const FieldMap* f = findField(field);
    if (f) applyAbs(frame, f->member, val);
  };

  if (voice == "Adam") {
    mulByName("cb1", 1.3);
    mulByName("pa6", 1.3);
    mulByName("fricationAmplitude", 0.85);
  } else if (voice == "Benjamin") {
    mulByName("cf1", 1.01);
    mulByName("cf2", 1.02);
    absByName("cf4", 3770);
    absByName("cf5", 4100);
    absByName("cf6", 5000);
    mulByName("cfNP", 0.9);
    mulByName("cb1", 1.3);
    mulByName("fricationAmplitude", 0.7);
    mulByName("pa6", 1.3);
  } else if (voice == "Caleb") {
    absByName("aspirationAmplitude", 1);
    absByName("voiceAmplitude", 0);
  } else if (voice == "David") {
    mulByName("voicePitch", 0.75);
    mulByName("endVoicePitch", 0.75);
    mulByName("cf1", 0.75);
    mulByName("cf2", 0.85);
    mulByName("cf3", 0.85);
  } else {
    // Unknown voice name: fall back to Adam behavior.
    mulByName("cb1", 1.3);
    mulByName("pa6", 1.3);
    mulByName("fricationAmplitude", 0.85);
  }

  // 2) Per-field multipliers (0..100, 50 => neutral)
  if (m_speech.frameParams.size() == kFieldCount()) {
    for (size_t i = 0; i < kFieldCount(); ++i) {
      const double ratio = static_cast<double>(clampInt(m_speech.frameParams[i], 0, 100)) / 50.0;
      if (ratio == 1.0) continue;
      applyMul(frame, kFieldMap[i].member, ratio);
    }
  }

  // 3) Volume scaling (match NVDA driver: preFormantGain *= volume/75)
  const double vol = static_cast<double>(clampInt(m_speech.volume, 0, 100)) / 75.0;
  frame.preFormantGain = static_cast<speechPlayer_frameParam_t>(static_cast<double>(frame.preFormantGain) * vol);
}

void NvspRuntime::unload() {
  if (m_feHandle && m_feDestroy) {
    m_feDestroy(m_feHandle);
    m_feHandle = nullptr;
  }

  m_spInitialize = nullptr;
  m_spQueueFrame = nullptr;
  m_spSynthesize = nullptr;
  m_spTerminate = nullptr;

  m_feCreate = nullptr;
  m_feDestroy = nullptr;
  m_feSetLanguage = nullptr;
  m_feQueueIPA = nullptr;
  m_feGetLastError = nullptr;

  if (m_frontend) {
    FreeLibrary(m_frontend);
    m_frontend = NULL;
  }
  if (m_speechPlayer) {
    FreeLibrary(m_speechPlayer);
    m_speechPlayer = NULL;
  }
}

bool NvspRuntime::setDllDirectory(const std::wstring& dllDir, std::string& outError) {
  outError.clear();
  unload();

  if (dllDir.empty()) {
    outError = "DLL directory is empty";
    return false;
  }

  std::wstring spPath = dllDir;
  if (!spPath.empty() && spPath.back() != L'\\' && spPath.back() != L'/') spPath += L'\\';
  std::wstring fePath = spPath;
  spPath += L"speechPlayer.dll";
  fePath += L"nvspFrontend.dll";

  m_speechPlayer = LoadLibraryW(spPath.c_str());
  if (!m_speechPlayer) {
    outError = "Could not load speechPlayer.dll";
    return false;
  }
  m_frontend = LoadLibraryW(fePath.c_str());
  if (!m_frontend) {
    outError = "Could not load nvspFrontend.dll";
    FreeLibrary(m_speechPlayer);
    m_speechPlayer = NULL;
    return false;
  }

  // speechPlayer
  m_spInitialize = reinterpret_cast<sp_initialize_fn>(GetProcAddress(m_speechPlayer, "speechPlayer_initialize"));
  m_spQueueFrame = reinterpret_cast<sp_queueFrame_fn>(GetProcAddress(m_speechPlayer, "speechPlayer_queueFrame"));
  m_spSynthesize = reinterpret_cast<sp_synthesize_fn>(GetProcAddress(m_speechPlayer, "speechPlayer_synthesize"));
  m_spTerminate = reinterpret_cast<sp_terminate_fn>(GetProcAddress(m_speechPlayer, "speechPlayer_terminate"));

  if (!m_spInitialize || !m_spQueueFrame || !m_spSynthesize || !m_spTerminate) {
    outError = "speechPlayer.dll is missing expected exports";
    unload();
    return false;
  }

  // nvspFrontend
  m_feCreate = reinterpret_cast<fe_create_fn>(GetProcAddress(m_frontend, "nvspFrontend_create"));
  m_feDestroy = reinterpret_cast<fe_destroy_fn>(GetProcAddress(m_frontend, "nvspFrontend_destroy"));
  m_feSetLanguage = reinterpret_cast<fe_setLanguage_fn>(GetProcAddress(m_frontend, "nvspFrontend_setLanguage"));
  m_feQueueIPA = reinterpret_cast<fe_queueIPA_fn>(GetProcAddress(m_frontend, "nvspFrontend_queueIPA"));
  m_feGetLastError = reinterpret_cast<fe_getLastError_fn>(GetProcAddress(m_frontend, "nvspFrontend_getLastError"));

  if (!m_feCreate || !m_feDestroy || !m_feSetLanguage || !m_feQueueIPA || !m_feGetLastError) {
    outError = "nvspFrontend.dll is missing expected exports";
    unload();
    return false;
  }

  return true;
}

bool NvspRuntime::setPackRoot(const std::wstring& packRootDir, std::string& outError) {
  outError.clear();
  m_packRoot = packRootDir;

  // Reset the frontend handle; it is tied to packDir.
  if (m_feHandle && m_feDestroy) {
    m_feDestroy(m_feHandle);
    m_feHandle = nullptr;
  }

  return true;
}

bool NvspRuntime::setLanguage(const std::string& langTagUtf8, std::string& outError) {
  outError.clear();
  m_langTag = langTagUtf8;

  if (!dllsLoaded()) {
    outError = "DLLs are not loaded";
    return false;
  }
  if (m_packRoot.empty()) {
    outError = "Pack root is not set";
    return false;
  }

  if (!m_feHandle) {
    std::string packUtf8 = wideToUtf8(m_packRoot);
    m_feHandle = m_feCreate(packUtf8.c_str());
    if (!m_feHandle) {
      outError = "nvspFrontend_create failed (check packs/phonemes.yaml)";
      return false;
    }
  }

  if (!langTagUtf8.empty()) {
    int ok = m_feSetLanguage(m_feHandle, langTagUtf8.c_str());
    if (!ok) {
      const char* msg = m_feGetLastError(m_feHandle);
      m_lastFrontendError = msg ? msg : "";
      outError = m_lastFrontendError.empty() ? "nvspFrontend_setLanguage failed" : m_lastFrontendError;
      return false;
    }
  }

  return true;
}

bool NvspRuntime::dllsLoaded() const {
  return m_speechPlayer && m_frontend && m_spInitialize && m_spQueueFrame && m_spSynthesize && m_spTerminate && m_feCreate;
}

static bool synthesizeAll(
  sp_synthesize_fn synthFn,
  speechPlayer_handle_t player,
  std::vector<sample>& outSamples
) {
  const unsigned int block = 2048;
  std::vector<sample> tmp(block);

  while (true) {
    int n = synthFn(player, block, tmp.data());
    if (n <= 0) break;
    outSamples.insert(outSamples.end(), tmp.begin(), tmp.begin() + n);
    if (static_cast<unsigned int>(n) < block) break;
  }
  return true;
}

bool NvspRuntime::synthPreviewPhoneme(
  const Node& phonemeMap,
  int sampleRate,
  std::vector<sample>& outSamples,
  std::string& outError
) {
  outSamples.clear();
  outError.clear();

  if (!dllsLoaded()) {
    outError = "DLLs are not loaded";
    return false;
  }

  speechPlayer_handle_t player = m_spInitialize(sampleRate);
  if (!player) {
    outError = "speechPlayer_initialize failed";
    return false;
  }

  speechPlayer_frame_t frame{};
  bool isVowel = false;
  applyPhonemeMapToFrame(phonemeMap, frame, isVowel);
  applySpeechSettingsToFrame(frame);

  const double preMs = 35.0;
  const double durMs = isVowel ? 180.0 : 120.0;
  const double postMs = 50.0;
  const double fadeMs = 8.0;

  const unsigned int preS = msToSamples(preMs, sampleRate);
  const unsigned int durS = msToSamples(durMs, sampleRate);
  const unsigned int postS = msToSamples(postMs, sampleRate);
  const unsigned int fadeS = msToSamples(fadeMs, sampleRate);

  // Purge on first queue.
  m_spQueueFrame(player, nullptr, preS, fadeS, -1, true);
  m_spQueueFrame(player, &frame, durS, fadeS, -1, false);
  m_spQueueFrame(player, nullptr, postS, fadeS, -1, false);

  synthesizeAll(m_spSynthesize, player, outSamples);
  m_spTerminate(player);
  return true;
}

// -------------------------
// Punctuation pauses (matches NVDA driver behavior)
// -------------------------

static std::string toLowerAscii(std::string s) {
  for (auto& ch : s) ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
  return s;
}

static double punctuationPauseMs(char punct, const std::string& pauseMode) {
  const std::string pm = toLowerAscii(pauseMode);
  if (pm == "off") return 0.0;

  auto strong = [&]() { return (pm == "long") ? 50.0 : 30.0; };
  auto comma = [&]() { return (pm == "long") ? 6.0 : 0.0; };

  switch (punct) {
    case '.':
    case '!':
    case '?':
      return strong();
    case ':':
    case ';':
      return strong();
    case ',':
      return comma();
    default:
      return 0.0;
  }
}

static bool isClauseMarkerToken(const std::string& tok, char& outPunct) {
  // Marker tokens are inserted by the phonemizer bridge and may also be typed
  // directly by users in IPA mode.
  //
  // Supported: ".", "!", "?", ",", ":", ";", "..." (ellipsis treated as '.').
  if (tok == "...") {
    outPunct = '.';
    return true;
  }
  if (tok.size() == 1) {
    const char c = tok[0];
    if (c == '.' || c == '!' || c == '?' || c == ',' || c == ':' || c == ';') {
      outPunct = c;
      return true;
    }
  }
  return false;
}

struct IpaClauseChunk {
  std::string ipa;   // IPA tokens (no marker punctuation tokens)
  char punct = 0;    // punctuation that ended this chunk (0 if none)
};

static void splitIpaByClauseMarkers(const std::string& ipaUtf8, std::vector<IpaClauseChunk>& out) {
  out.clear();

  // Tokenize on ASCII whitespace. (IPA itself can include non-ASCII bytes.)
  std::vector<std::string> tokens;
  tokens.reserve(256);
  std::string cur;
  cur.reserve(64);

  auto flushTok = [&]() {
    if (!cur.empty()) {
      tokens.push_back(std::move(cur));
      cur.clear();
    }
  };

  for (unsigned char b : ipaUtf8) {
    if (b == ' ' || b == '\t' || b == '\r' || b == '\n' || b == '\v' || b == '\f') {
      flushTok();
      continue;
    }
    cur.push_back(static_cast<char>(b));
  }
  flushTok();

  std::vector<std::string> buf;
  buf.reserve(tokens.size());

  auto flushClause = [&](char punct) {
    if (buf.empty()) return;
    std::string joined;
    // Rebuild with single spaces.
    size_t total = 0;
    for (const auto& t : buf) total += t.size() + 1;
    joined.reserve(total);

    for (size_t i = 0; i < buf.size(); ++i) {
      if (i) joined.push_back(' ');
      joined += buf[i];
    }

    IpaClauseChunk c;
    c.ipa = std::move(joined);
    c.punct = punct;
    out.push_back(std::move(c));
    buf.clear();
  };

  for (const auto& t : tokens) {
    char punct = 0;
    if (isClauseMarkerToken(t, punct)) {
      flushClause(punct);
      continue;
    }
    buf.push_back(t);
  }
  flushClause(0);

  // If nothing was split out (e.g. whitespace-only), keep a single empty chunk.
  if (out.empty()) {
    IpaClauseChunk c;
    c.ipa = "";
    c.punct = 0;
    out.push_back(std::move(c));
  }
}

struct QueueCtx {
  sp_queueFrame_fn queueFrame;
  speechPlayer_handle_t player;
  int sampleRate;
  bool first;
  const NvspRuntime* runtime;
};

static void __cdecl frameCallback(
  void* userData,
  const nvspFrontend_Frame* frameOrNull,
  double durationMs,
  double fadeMs,
  int userIndex
) {
  QueueCtx* ctx = reinterpret_cast<QueueCtx*>(userData);
  if (!ctx || !ctx->queueFrame) return;

  unsigned int durS = msToSamples(durationMs, ctx->sampleRate);
  unsigned int fadeS = msToSamples(fadeMs, ctx->sampleRate);

  if (frameOrNull) {
    speechPlayer_frame_t f{};
    // Copy field-by-field (no ABI/layout assumptions).
    f.voicePitch = frameOrNull->voicePitch;
    f.vibratoPitchOffset = frameOrNull->vibratoPitchOffset;
    f.vibratoSpeed = frameOrNull->vibratoSpeed;
    f.voiceTurbulenceAmplitude = frameOrNull->voiceTurbulenceAmplitude;
    f.glottalOpenQuotient = frameOrNull->glottalOpenQuotient;
    f.voiceAmplitude = frameOrNull->voiceAmplitude;
    f.aspirationAmplitude = frameOrNull->aspirationAmplitude;

    f.cf1 = frameOrNull->cf1;
    f.cf2 = frameOrNull->cf2;
    f.cf3 = frameOrNull->cf3;
    f.cf4 = frameOrNull->cf4;
    f.cf5 = frameOrNull->cf5;
    f.cf6 = frameOrNull->cf6;
    f.cfN0 = frameOrNull->cfN0;
    f.cfNP = frameOrNull->cfNP;

    f.cb1 = frameOrNull->cb1;
    f.cb2 = frameOrNull->cb2;
    f.cb3 = frameOrNull->cb3;
    f.cb4 = frameOrNull->cb4;
    f.cb5 = frameOrNull->cb5;
    f.cb6 = frameOrNull->cb6;
    f.cbN0 = frameOrNull->cbN0;
    f.cbNP = frameOrNull->cbNP;

    f.caNP = frameOrNull->caNP;

    f.fricationAmplitude = frameOrNull->fricationAmplitude;

    f.pf1 = frameOrNull->pf1;
    f.pf2 = frameOrNull->pf2;
    f.pf3 = frameOrNull->pf3;
    f.pf4 = frameOrNull->pf4;
    f.pf5 = frameOrNull->pf5;
    f.pf6 = frameOrNull->pf6;

    f.pb1 = frameOrNull->pb1;
    f.pb2 = frameOrNull->pb2;
    f.pb3 = frameOrNull->pb3;
    f.pb4 = frameOrNull->pb4;
    f.pb5 = frameOrNull->pb5;
    f.pb6 = frameOrNull->pb6;

    f.pa1 = frameOrNull->pa1;
    f.pa2 = frameOrNull->pa2;
    f.pa3 = frameOrNull->pa3;
    f.pa4 = frameOrNull->pa4;
    f.pa5 = frameOrNull->pa5;
    f.pa6 = frameOrNull->pa6;

    f.parallelBypass = frameOrNull->parallelBypass;
    f.preFormantGain = frameOrNull->preFormantGain;
    f.outputGain = frameOrNull->outputGain;
    f.endVoicePitch = frameOrNull->endVoicePitch;

    if (ctx->runtime) {
      ctx->runtime->applySpeechSettingsToFrame(f);
    }

    ctx->queueFrame(ctx->player, &f, durS, fadeS, userIndex, ctx->first);
  } else {
    ctx->queueFrame(ctx->player, nullptr, durS, fadeS, userIndex, ctx->first);
  }
  ctx->first = false;
}

bool NvspRuntime::synthIpa(
  const std::string& ipaUtf8,
  int sampleRate,
  std::vector<sample>& outSamples,
  std::string& outError
) {
  outSamples.clear();
  outError.clear();
  m_lastFrontendError.clear();

  if (!dllsLoaded()) {
    outError = "DLLs are not loaded";
    return false;
  }
  if (m_packRoot.empty()) {
    outError = "Pack root is not set";
    return false;
  }

  // Ensure frontend handle + language.
  if (!m_feHandle) {
    std::string packUtf8 = wideToUtf8(m_packRoot);
    m_feHandle = m_feCreate(packUtf8.c_str());
    if (!m_feHandle) {
      outError = "nvspFrontend_create failed (check packs/phonemes.yaml)";
      return false;
    }
  }
  if (!m_langTag.empty()) {
    int ok = m_feSetLanguage(m_feHandle, m_langTag.c_str());
    if (!ok) {
      const char* msg = m_feGetLastError(m_feHandle);
      m_lastFrontendError = msg ? msg : "";
      outError = m_lastFrontendError.empty() ? "nvspFrontend_setLanguage failed" : m_lastFrontendError;
      return false;
    }
  }

  speechPlayer_handle_t player = m_spInitialize(sampleRate);
  if (!player) {
    outError = "speechPlayer_initialize failed";
    return false;
  }

  QueueCtx ctx{};
  ctx.queueFrame = m_spQueueFrame;
  ctx.player = player;
  ctx.sampleRate = sampleRate;
  ctx.first = true;
  ctx.runtime = this;

  // Match NVDA driver's mapping:
  //   rate: 0..100 -> 0.25 * 2^(rate/25)
  //   pitch: 0..100 -> basePitch = 25 + 21.25*(pitch/12.5)
  //   inflection: 0..100 -> 0.0..1.0
  const int rate = clampInt(m_speech.rate, 0, 100);
  const int pitch = clampInt(m_speech.pitch, 0, 100);
  const int infl = clampInt(m_speech.inflection, 0, 100);
  const double speed = 0.25 * std::pow(2.0, static_cast<double>(rate) / 25.0);
  const double basePitch = 25.0 + (21.25 * (static_cast<double>(pitch) / 12.5));
  const double inflection = static_cast<double>(infl) / 100.0;

// Split IPA into clause chunks. Clause markers can be:
// - inserted by our phonemizer chunker (for text->IPA)
// - typed directly by users (in IPA mode)
//
// This lets us insert real silence frames between sentences/clauses so speech
// does not sound like one long run-on stream.
std::vector<IpaClauseChunk> clauses;
splitIpaByClauseMarkers(ipaUtf8, clauses);

bool ok = true;
for (size_t i = 0; i < clauses.size(); ++i) {
  const IpaClauseChunk& c = clauses[i];
  if (c.ipa.empty()) continue;

  // nvspFrontend only reads a single byte from clauseType.
  // Use '.' as a safe default if the chunk has no marker punctuation.
  const char punct = c.punct ? c.punct : '.';
  char clauseBuf[2] = { punct, 0 };

  int qok = m_feQueueIPA(
    m_feHandle,
    c.ipa.c_str(),
    speed,
    basePitch,
    inflection,
    clauseBuf,
    -1,
    frameCallback,
    &ctx
  );

  if (!qok) {
    ok = false;
    break;
  }

  // Optional punctuation pause (micro-silence) between clauses.
  // This is separate from "clauseType" prosody; it adds actual time separation.
  if (c.punct && (i + 1) < clauses.size()) {
    const double pMs = punctuationPauseMs(c.punct, m_speech.pauseMode);
    if (pMs > 0.0) {
      const unsigned int durS = msToSamples(pMs, sampleRate);
      const double fadeMs = (std::min)(pMs, 3.0);
      const unsigned int fadeS = msToSamples(fadeMs, sampleRate);
      ctx.queueFrame(ctx.player, nullptr, durS, fadeS, -1, ctx.first);
      ctx.first = false;
    }
  }
}

if (!ok) {
    const char* msg = m_feGetLastError(m_feHandle);
    m_lastFrontendError = msg ? msg : "";
    outError = m_lastFrontendError.empty() ? "nvspFrontend_queueIPA failed" : m_lastFrontendError;
    m_spTerminate(player);
    return false;
  }

  synthesizeAll(m_spSynthesize, player, outSamples);
  m_spTerminate(player);
  return true;
}

} // namespace nvsp_editor
