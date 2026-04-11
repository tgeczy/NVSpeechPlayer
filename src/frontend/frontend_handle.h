/*
TGSpeechBox — Private Handle struct for the frontend C API.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSB_FRONTEND_HANDLE_H
#define TGSB_FRONTEND_HANDLE_H

#include "nvspFrontend.h"
#include "data_query.h"
#include "ipa_engine.h"
#include "pack.h"

#include <locale>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace nvsp_frontend {

struct Handle {
  std::string packDir;
  std::string overrideDir;  // Optional dir checked first for lang YAML files
  PackSet pack;
  bool packLoaded = false;
  // True once we have emitted at least one chunk of speech on this handle.
  // Used to optionally insert a tiny silence between consecutive queueIPA calls.
  bool streamHasSpeech = false;
  // True if the last emitted *real phoneme* in the previous chunk was vowel-like
  // (vowel or semivowel). Used to avoid inserting boundary pauses inside
  // vowel-to-vowel transitions (e.g. diphthongs split across chunks).
  bool lastEndsVowelLike = false;
  std::string langTag;
  std::string lastError;
  std::mutex mu;

  // Per-language disabled dictionary types (e.g., {"en-us": {"stress", "compound"}}).
  // Set via setData(DICTIONARY, "config:langTag", type, "false"/"true").
  std::unordered_map<std::string, std::unordered_set<std::string>> disabledDictTypes;

  // Temporary dict data for cross-language browsing (when lang != langTag).
  // Loaded on demand via loadDictFiles(), cached until lang changes.
  struct CrossLangDicts {
    std::string lang;  // which language this was loaded for
    std::unordered_map<std::string, std::vector<int>> stressDict;
    std::unordered_map<std::string, std::vector<std::string>> compoundMap;
    PronDict pronDict;
    std::unordered_map<std::string, std::string> letterDict;
  };
  std::unique_ptr<CrossLangDicts> crossLangDicts;

  // Per-handle trajectory limiting state for formant smoothing.
  // This is NOT static - each handle has its own state to avoid data races
  // when multiple engine instances speak concurrently.
  TrajectoryState trajectoryState;

  // User-level FrameEx defaults (ABI v2+).
  // These are mixed with per-phoneme values when emitting frames.
  double frameExCreakiness = 0.0;
  double frameExBreathiness = 0.0;
  double frameExJitter = 0.0;
  double frameExShimmer = 0.0;
  double frameExSharpness = 1.0;  // multiplier, 1.0 = neutral

  // IPA overrides collected during prepareText — words whose dict entry
  // has toIpa set.  Keyed on lowercase word text.  Consumed by the next
  // runTextParser call, then cleared.
  std::unordered_map<std::string, std::string> ipaOverrides;

  // Buffer for getVoiceProfileNames return value
  std::string profileNamesBuffer;

  // Deferred settings: if setPitchMode / setInflectionScale are called
  // before setLanguage, we stash the values here and apply them once
  // the pack is loaded.  Prevents crashes from writing to uninitialized
  // h->pack memory (Android crash when Speak tab selects klatt_style
  // before language init).
  std::string pendingPitchMode;
  double pendingInflectionScale = 0.0;
  bool hasPendingInflectionScale = false;

  // Generic data query cache (ABI v5+).
  tgsb_data::DataCache dataCache;
};

inline Handle* asHandle(nvspFrontend_handle_t h) {
  return reinterpret_cast<Handle*>(h);
}

inline void setError(Handle* h, const std::string& msg) {
  if (!h) return;
  h->lastError = msg;
}

// Locale-independent double parse -- NVDA may set process locale to one that
// uses ',' as decimal separator (Hungarian, Polish, etc.), which would cause
// std::strtod/atof to misread YAML values like "0.15" as 0.0.
inline double parseDouble(const std::string& s) {
  std::istringstream iss(s);
  iss.imbue(std::locale::classic());
  double v = 0.0;
  iss >> v;
  return v;
}

} // namespace nvsp_frontend

#endif // TGSB_FRONTEND_HANDLE_H
