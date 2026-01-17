#include "nvspFrontend.h"

#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "ipa_engine.h"
#include "pack.h"

namespace nvsp_frontend {

struct Handle {
  std::string packDir;
  PackSet pack;
  bool packLoaded = false;
  std::string langTag;
  std::string lastError;
  // True after at least one successful queueIPA call emitted frames.
  // Used to insert an optional inter-segment gap between consecutive calls.
  bool streamHasSpeech = false;
  std::mutex mu;
};

static Handle* asHandle(nvspFrontend_handle_t h) {
  return reinterpret_cast<Handle*>(h);
}

static void setError(Handle* h, const std::string& msg) {
  if (!h) return;
  h->lastError = msg;
}

} // namespace nvsp_frontend

extern "C" {

NVSP_FRONTEND_API nvspFrontend_handle_t nvspFrontend_create(const char* packDirUtf8) {
  using namespace nvsp_frontend;
  try {
    auto* h = new Handle();
    h->packDir = packDirUtf8 ? std::string(packDirUtf8) : std::string();
    h->lastError.clear();
    return reinterpret_cast<nvspFrontend_handle_t>(h);
  } catch (...) {
    return nullptr;
  }
}

NVSP_FRONTEND_API void nvspFrontend_destroy(nvspFrontend_handle_t handle) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  delete h;
}

NVSP_FRONTEND_API int nvspFrontend_setLanguage(nvspFrontend_handle_t handle, const char* langTagUtf8) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h) return 0;

  std::lock_guard<std::mutex> lock(h->mu);

  h->lastError.clear();
  const std::string lang = langTagUtf8 ? std::string(langTagUtf8) : std::string();

  PackSet pack;
  std::string err;
  if (!loadPackSet(h->packDir, lang, pack, err)) {
    setError(h, err.empty() ? "Failed to load pack set" : err);
    return 0;
  }

  h->pack = std::move(pack);
  h->packLoaded = true;
  h->langTag = normalizeLangTag(lang);
  // Treat language change as a stream reset.
  h->streamHasSpeech = false;
  return 1;
}

NVSP_FRONTEND_API int nvspFrontend_queueIPA(
  nvspFrontend_handle_t handle,
  const char* ipaUtf8,
  double speed,
  double basePitch,
  double inflection,
  const char* clauseTypeUtf8,
  int userIndexBase,
  nvspFrontend_FrameCallback cb,
  void* userData
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h) return 0;

  std::lock_guard<std::mutex> lock(h->mu);
  h->lastError.clear();

  if (!h->packLoaded) {
    // Default to "default" language if the caller didn't call setLanguage.
    PackSet pack;
    std::string err;
    if (!loadPackSet(h->packDir, "default", pack, err)) {
      setError(h, err.empty() ? "No language loaded and default load failed" : err);
      return 0;
    }
    h->pack = std::move(pack);
    h->packLoaded = true;
    h->langTag = "default";
  }

  if (!ipaUtf8) ipaUtf8 = "";

  char clauseType = '.';
  if (clauseTypeUtf8 && clauseTypeUtf8[0]) {
    clauseType = clauseTypeUtf8[0];
  }

  std::vector<Token> tokens;
  std::string err;
  if (!convertIpaToTokens(h->pack, ipaUtf8, speed, basePitch, inflection, clauseType, tokens, err)) {
    setError(h, err.empty() ? "IPA conversion failed" : err);
    return 0;
  }

  // Optional: Insert a tiny silence between consecutive queueIPA calls.
  // This helps with UI speech where NVDA supplies separate chunks (label/role/value)
  // and the synthesizer would otherwise transition abruptly with no boundary.
  //
  // Units in YAML are ms at speed=1.0; we divide by speed.
  const double effSpeed = (speed <= 0.0) ? 1.0 : speed;
  if (cb && h->streamHasSpeech && !tokens.empty()) {
    const double gap = h->pack.lang.segmentBoundaryGapMs;
    const double fade = h->pack.lang.segmentBoundaryFadeMs;
    if (gap > 0.0) {
      cb(userData, nullptr, gap / effSpeed, (fade > 0.0 ? fade / effSpeed : 0.0), -1);
    }
  }

  emitFrames(h->pack, tokens, userIndexBase, cb, userData);
  if (!tokens.empty()) h->streamHasSpeech = true;
  return 1;
}

NVSP_FRONTEND_API const char* nvspFrontend_getLastError(nvspFrontend_handle_t handle) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h) return "invalid handle";
  std::lock_guard<std::mutex> lock(h->mu);
  return h->lastError.c_str();
}

} // extern "C"
