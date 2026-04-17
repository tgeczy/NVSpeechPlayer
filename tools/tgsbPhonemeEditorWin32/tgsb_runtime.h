/*
TGSpeechBox — DLL runtime loader interface.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include <windows.h>

#include "speechPlayer.h"
#include "yaml_edit.h"

// NOTE: We dynamically load speechPlayer.dll and nvspFrontend.dll.
// These types are used only for layout compatibility.
#include "nvspFrontend.h"

namespace tgsb_editor {

// Mirrors the NVDA driver's public-facing speech settings.
// - Voice: preset that applies multipliers/overrides to the generated frames.
// - Rate/Pitch/Volume/Inflection: passed to nvspFrontend.dll (and/or applied to frames).
// - frameParams: 0..100 sliders that act as multipliers on each speechPlayer frame field,
//   with 50 meaning "neutral" (x1.0).
// - voicingParams: 0..100 sliders for VoicingTone parameters.
struct SpeechSettings {
  std::string voiceName = "Adam";
  int rate = 50;       // 0..100
  int pitch = 50;      // 0..100
  int volume = 90;     // 0..100
  int inflection = 60; // 0..100
  std::string pauseMode = "short"; // off | short | long
  std::vector<int> frameParams;   // size == frameParamNames().size()
  std::vector<int> voicingParams; // size == voicingParamNames().size()
  std::vector<int> frameExParams; // size == frameExParamNames().size() - FrameEx voice quality
};

// -------------------------
// Dynamic DLL function types
// -------------------------

// VoicingTone v3 struct - must match voicingTone.h in speechPlayer (v3+ DLLs)
#ifndef SPEECHPLAYER_VOICINGTONE_MAGIC
#define SPEECHPLAYER_VOICINGTONE_MAGIC 0x32544F56u   // "VOT2"
#endif

#ifndef SPEECHPLAYER_VOICINGTONE_VERSION
#define SPEECHPLAYER_VOICINGTONE_VERSION 4u
#endif

#ifndef SPEECHPLAYER_DSP_VERSION
#define SPEECHPLAYER_DSP_VERSION 6u
#endif

struct EditorVoicingToneV3 {
  // ABI header
  uint32_t magic;
  uint32_t structSize;
  uint32_t structVersion;
  uint32_t dspVersion;
  // Parameters
  double voicingPeakPos;
  double voicedPreEmphA;
  double voicedPreEmphMix;
  double highShelfGainDb;
  double highShelfFcHz;
  double highShelfQ;
  double voicedTiltDbPerOct;
  double noiseGlottalModDepth;
  double pitchSyncF1DeltaHz;
  double pitchSyncB1DeltaHz;
  // V3 additions
  double speedQuotient;
  double aspirationTiltDbPerOct;
  double cascadeBwScale;
  double tremorDepth;
  // V4 additions — vocal tract shape
  double nasalBwScale;
  double f4FreqScale;
  double nasalGainScale;
  // V5 additions — dual-oscillator chorus
  double chorusDepth;
  double chorusDetuneHz;
};

// Alias for backward compatibility
using EditorVoicingToneV2 = EditorVoicingToneV3;

// VoicingTone v1 struct - legacy 7-double layout (no header)
struct EditorVoicingToneV1 {
  double voicingPeakPos;
  double voicedPreEmphA;
  double voicedPreEmphMix;
  double highShelfGainDb;
  double highShelfFcHz;
  double highShelfQ;
  double voicedTiltDbPerOct;
};

// FrameEx struct - per-frame voice quality extensions (DSP v5+)
// Must match nvspFrontend_FrameEx / speechPlayer_frameEx_t exactly (29 doubles = 232 bytes)
// >>> AUTO-GENERATED FROM src/frame.h - DO NOT EDIT MANUALLY (regenerate: python tools/gen_frame_ex.py) >>>
struct EditorFrameEx {
  // Voice quality parameters (DSP v5)
  double creakiness;  // laryngealization / creaky voice (e.g. Danish stød)
  double breathiness;  // breath noise mixed into voicing
  double jitter;  // pitch period variation (irregular F0)
  double shimmer;  // amplitude variation (irregular loudness)
  double sharpness;  // glottal closure sharpness MULTIPLIER (0=use SR default, 0.5-2.0 typical)
  // Formant end targets for within-frame ramping (NAN = no ramp)
  double endCf1;  // Cascade F1 end target (Hz), NAN = no ramp
  double endCf2;  // Cascade F2 end target (Hz), NAN = no ramp
  double endCf3;  // Cascade F3 end target (Hz), NAN = no ramp
  double endPf1;  // Parallel F1 end target (Hz), NAN = no ramp
  double endPf2;  // Parallel F2 end target (Hz), NAN = no ramp
  double endPf3;  // Parallel F3 end target (Hz), NAN = no ramp
  // Fujisaki-Bartman pitch contour model (DSP v6+)
  double fujisakiEnabled;  // 0.0 = off (legacy behavior), >0.5 = on
  double fujisakiReset;  // rising edge resets model filter state
  double fujisakiPhraseAmp;  // phrase command amplitude (e.g. 1.3)
  double fujisakiPhraseLen;  // phrase filter L (samples to peak). 0 = use default
  double fujisakiAccentAmp;  // accent command amplitude (e.g. 0.4)
  double fujisakiAccentDur;  // accent duration D (samples). 0 = use default
  double fujisakiAccentLen;  // accent filter L (samples to peak). 0 = use default
  // Per-parameter transition speed scales (DSP v7). 0.0 = no override.
  double transF1Scale;  // cf1, pf1, cb1, pb1
  double transF2Scale;  // cf2, pf2, cb2, pb2
  double transF3Scale;  // cf3, pf3, cb3, pb3
  double transNasalScale;  // cfN0, cfNP, cbN0, cbNP, caNP
  // Amplitude crossfade mode (DSP v7.1). 0=linear, 1=equal-power.
  double transAmplitudeMode;
  // Higher cascade formants F7/F8 (DSP v8, Rabiner 1968 defaults)
  double cf7;  // F7 frequency (Hz).  Default 6500.0
  double cb7;  // F7 bandwidth (Hz).  Default 720.0
  double cf8;  // F8 frequency (Hz).  Default 7500.0
  double cb8;  // F8 bandwidth (Hz).  Default 1250.0
  // Source amplitude timing (DSP v8). 0.0 = legacy, no hold.
  double transSourceHoldRatio;
  // Voicing onset hold (DSP v8). 0.0 = legacy, no hold.
  double transVoicingHoldRatio;
};
// <<< END AUTO-GENERATED <<<

// speechPlayer.dll API
using sp_initialize_fn = speechPlayer_handle_t(*)(int);
using sp_queueFrame_fn = void(*)(speechPlayer_handle_t, speechPlayer_frame_t*, unsigned int, unsigned int, int, bool);
using sp_queueFrameEx_fn = void(*)(speechPlayer_handle_t, speechPlayer_frame_t*, const EditorFrameEx*, unsigned int, unsigned int, unsigned int, int, bool);
using sp_synthesize_fn = int(*)(speechPlayer_handle_t, unsigned int, sample*);
using sp_terminate_fn = void(*)(speechPlayer_handle_t);
using sp_setVoicingTone_fn = void(*)(speechPlayer_handle_t, const void*);  // void* to accept either v1 or v2
using sp_getDspVersion_fn = unsigned int(*)();  // Returns DSP version if present

// nvspFrontend.dll API
using fe_create_fn = nvspFrontend_handle_t(*)(const char*);
using fe_destroy_fn = void(*)(nvspFrontend_handle_t);
using fe_setLanguage_fn = int(*)(nvspFrontend_handle_t, const char*);
using fe_queueIPA_fn = int(*)(
  nvspFrontend_handle_t,
  const char*,
  double,
  double,
  double,
  const char*,
  int,
  nvspFrontend_FrameCallback,
  void*
);
using fe_getLastError_fn = const char*(*)(nvspFrontend_handle_t);
using fe_setVoiceProfile_fn = int(*)(nvspFrontend_handle_t, const char*);
using fe_getVoiceProfile_fn = const char*(*)(nvspFrontend_handle_t);
using fe_getPackWarnings_fn = const char*(*)(nvspFrontend_handle_t);

// FrameExCallback - receives the MIXED FrameEx (phoneme + user defaults)
using nvspFrontend_FrameExCallback = void(*)(
  void* userData,
  const nvspFrontend_Frame* frameOrNull,
  const nvspFrontend_FrameEx* frameExOrNull,
  double durationMs,
  double fadeMs,
  int userIndex
);

using fe_setFrameExDefaults_fn = void(*)(
  nvspFrontend_handle_t,
  double creakiness,
  double breathiness,
  double jitter,
  double shimmer,
  double sharpness
);

using fe_queueIPA_Ex_fn = int(*)(
  nvspFrontend_handle_t,
  const char*,
  double,
  double,
  double,
  const char*,
  int,
  nvspFrontend_FrameExCallback,
  void*
);

using fe_queueIPA_ExWithText_fn = int(*)(
  nvspFrontend_handle_t,
  const char*,  // textUtf8
  const char*,  // ipaUtf8
  double,
  double,
  double,
  const char*,
  int,
  nvspFrontend_FrameExCallback,
  void*
);

class TgsbRuntime {
public:
  TgsbRuntime();
  ~TgsbRuntime();

  // Speech settings (voice, sliders). Safe to call before DLLs are loaded.
  void setSpeechSettings(const SpeechSettings& s);
  SpeechSettings getSpeechSettings() const;

  // Names of the 47 frame parameters exposed in the NVDA driver.
  static const std::vector<std::string>& frameParamNames();
  
  // Names of the 14 voicing tone parameters.
  static const std::vector<std::string>& voicingParamNames();

  // Names of the 5 FrameEx voice quality parameters.
  static const std::vector<std::string>& frameExParamNames();

  // Directory containing speechPlayer.dll and nvspFrontend.dll.
  bool setDllDirectory(const std::wstring& dllDir, std::string& outError);

  // Directory that contains a "packs" folder.
  bool setPackRoot(const std::wstring& packRootDir, std::string& outError);

  // Language tag like "en-us", "hu", ...
  bool setLanguage(const std::string& langTagUtf8, std::string& outError);

  bool dllsLoaded() const;

  // Synthesize just a single phoneme (from phonemes.yaml) to PCM samples.
  bool synthPreviewPhoneme(
    const Node& phonemeMap,
    int sampleRate,
    std::vector<sample>& outSamples,
    std::string& outError
  );

  // Synthesize an IPA string via nvspFrontend.dll to PCM samples.
  // If textUtf8 is non-empty, uses queueIPA_ExWithText for stress correction.
  bool synthIpa(
    const std::string& ipaUtf8,
    int sampleRate,
    std::vector<sample>& outSamples,
    std::string& outError,
    const std::string& textUtf8 = ""
  );

  // Last frontend error (if available).
  std::string lastFrontendError() const { return m_lastFrontendError; }

  // Voice profile support.
  // Discover profile names from phonemes.yaml (call after setPackRoot).
  std::vector<std::string> discoverVoiceProfiles() const;
  
  // Set the active voice profile (empty string = no profile).
  bool setVoiceProfile(const std::string& profileName, std::string& outError);
  
  // Get the currently active voice profile name.
  std::string getVoiceProfile() const;
  
  // Check if a voice name is a C++ profile (vs Python preset).
  static bool isVoiceProfile(const std::string& voiceName);
  
  // Get profile name from voice name (strips "profile:" prefix).
  static std::string getProfileNameFromVoice(const std::string& voiceName);
  
  // Voice profile prefix used to distinguish profiles from Python presets.
  static constexpr const char* kVoiceProfilePrefix = "profile:";

  // Save voicing + FrameEx slider values to YAML for specified profile.
  // voicingSliders: 12 values (0-100), frameExSliders: 5 values (0-100)
  bool saveVoiceProfileSliders(const std::string& profileName,
                               const std::vector<int>& voicingSliders,
                               const std::vector<int>& frameExSliders,
                               std::string& outError);

  // Apply voice preset + per-field multipliers + volume scaling.
  // Exposed so the free callback helper can reuse the same logic.
  void applySpeechSettingsToFrame(speechPlayer_frame_t& frame) const;

private:
  void unload();
  
  // VoicingTone version detection
  enum class VoicingToneSupport { None, V1, V2 };
  VoicingToneSupport m_voicingToneSupport = VoicingToneSupport::None;

  // DLL modules.
  HMODULE m_speechPlayer = NULL;
  HMODULE m_frontend = NULL;

  sp_initialize_fn m_spInitialize = nullptr;
  sp_queueFrame_fn m_spQueueFrame = nullptr;
  sp_queueFrameEx_fn m_spQueueFrameEx = nullptr;
  sp_synthesize_fn m_spSynthesize = nullptr;
  sp_terminate_fn m_spTerminate = nullptr;
  sp_setVoicingTone_fn m_spSetVoicingTone = nullptr;
  sp_getDspVersion_fn m_spGetDspVersion = nullptr;

  fe_create_fn m_feCreate = nullptr;
  fe_destroy_fn m_feDestroy = nullptr;
  fe_setLanguage_fn m_feSetLanguage = nullptr;
  fe_queueIPA_fn m_feQueueIPA = nullptr;
  fe_getLastError_fn m_feGetLastError = nullptr;
  fe_setVoiceProfile_fn m_feSetVoiceProfile = nullptr;
  fe_getVoiceProfile_fn m_feGetVoiceProfile = nullptr;
  fe_getPackWarnings_fn m_feGetPackWarnings = nullptr;
  fe_setFrameExDefaults_fn m_feSetFrameExDefaults = nullptr;
  fe_queueIPA_Ex_fn m_feQueueIPA_Ex = nullptr;
  fe_queueIPA_ExWithText_fn m_feQueueIPA_ExWithText = nullptr;

  // Runtime state
  nvspFrontend_handle_t m_feHandle = nullptr;
  std::string m_lastFrontendError;
  std::wstring m_packRoot;
  std::string m_langTag;

  SpeechSettings m_speech;
};

} // namespace tgsb_editor