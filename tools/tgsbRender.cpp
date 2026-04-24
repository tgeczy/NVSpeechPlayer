/*
TGSpeechBox — Command-line IPA-to-PCM renderer for Speech Dispatcher.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

/*
  TGSBRender (formerly nvspRender)
  -----------------------------------
  Small command-line helper that turns an IPA stream into audio using:
    - nvspFrontend (IPA -> formant frames)
    - speechPlayer (frames -> 16-bit PCM)

  Intended use:
    - Speech Dispatcher via sd_generic (see extras/speech-dispatcher/)

  Notes:
    - This tool reads UTF-8 from stdin and writes raw 16-bit signed
      little-endian PCM to stdout at a configurable sample rate (default 16000 Hz).
    - We deliberately keep the interface small and self-contained.
    
  DSP V5 Features:
    - VoicingTone V3 support (13 parameters)
    - FrameEx support (creakiness, breathiness, jitter, shimmer, sharpness)
    - Per-phoneme FrameEx from YAML (e.g. Danish stød creakiness) via queueIPA_Ex
    - Voice profile support via nvspFrontend_setVoiceProfile
    - --list-voices to show available profiles for speech-dispatcher config
    - Automatic voicing tone loading from YAML when --voice is specified

  DSP V6 Features:
    - Formant end targets for within-frame ramping (DECTalk-style transitions)
    - Fujisaki-Bartman pitch model for Eloquence-style prosody contours
    - FrameEx extended to 22 fields (176 bytes)

  DSP V8 Features:
    - VoicingTone V4/V5 support (nasalBwScale, f4FreqScale, nasalGainScale,
      chorusDepth, chorusDetuneHz — 22 parameters total)
    - FrameEx extended to 27 fields (216 bytes) with higher cascade F7/F8
*/

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <iostream>
#include <string>
#include <vector>

#include "speechPlayer.h"
#include "nvspFrontend.h"

#if defined(_WIN32)
  #include <fcntl.h>
  #include <io.h>
#else
  #include <dlfcn.h>  // dlopen/dlsym for runtime espeak-ng loading
#endif

namespace {

// ============================================================================
// VoicingTone structure (must match voicingTone.h — V3+V4+V5 fields)
// ============================================================================

#ifndef SPEECHPLAYER_VOICINGTONE_MAGIC
#define SPEECHPLAYER_VOICINGTONE_MAGIC 0x32544F56u   // "VOT2"
#endif

#ifndef SPEECHPLAYER_VOICINGTONE_VERSION
#define SPEECHPLAYER_VOICINGTONE_VERSION 4u
#endif

#ifndef SPEECHPLAYER_DSP_VERSION
#define SPEECHPLAYER_DSP_VERSION 8u
#endif

struct VoicingToneV3 {
  uint32_t magic;
  uint32_t structSize;
  uint32_t structVersion;
  uint32_t dspVersion;
  // V1/V2 params
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
  // V4 additions: vocal tract shape
  double nasalBwScale;
  double f4FreqScale;
  double nasalGainScale;
  // V5 additions: dual-oscillator chorus
  double chorusDepth;
  double chorusDetuneHz;
};

// ============================================================================
// FrameEx structure (must match frame.h - 29 doubles = 232 bytes)
// ============================================================================

// >>> AUTO-GENERATED FROM src/frame.h - DO NOT EDIT MANUALLY (regenerate: python tools/gen_frame_ex.py) >>>
struct FrameEx {
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
  // Frication spectral tilt (DSP v9). 0=flat, negative=darken high-freq parallels.
  double fricationTiltDb;
};
// <<< END AUTO-GENERATED <<<

// ============================================================================
// Built-in voice presets (mirrors NVDA constants.py / mobile hardcoded voices)
//
// These are applied as frame multipliers in the render callback when the
// voice name matches but no YAML profile exists.  This keeps them out of
// the YAML profile system (no double-listing, no export conflicts).
// ============================================================================

struct BuiltinVoice {
  const char* name;
  // Multipliers (1.0 = no change).  Absolute overrides use negative sentinel.
  double voicePitch_mul, endVoicePitch_mul;
  double cf1_mul, cf2_mul, cf3_mul, cf4_mul, cf5_mul, cf6_mul;
  double cb1_mul, cb2_mul, cb3_mul, cb4_mul, cb5_mul, cb6_mul;
  double pb1_mul, pb2_mul, pb3_mul, pb4_mul, pb5_mul, pb6_mul;
  double pf3_mul, pf4_mul, pf5_mul, pf6_mul;
  double pa3_mul, pa4_mul, pa5_mul, pa6_mul;
  double fricationAmplitude_mul;
  double parallelBypass_mul;
  double voiceTurbulenceAmplitude_mul;
  // Absolute overrides (NAN = don't override).
  double voiceAmplitude_abs;
  double aspirationAmplitude_abs;
  double glottalOpenQuotient_abs;
  double vibratoPitchOffset_abs;
  double vibratoSpeed_abs;
  double cf4_abs, cf5_abs, cf6_abs;   // cfNP_mul handled via cfNP_mul field
  double cfNP_mul;
  double voicedTiltDbPerOct;           // For VoicingTone override
  bool   hasVoicedTilt;
};

static const BuiltinVoice kBuiltinVoices[] = {
  // Adam: slightly wider cb1, boosted pa6, reduced frication, lowered pitch (-8%)
  {"Adam",
    0.92, 0.92,                                       // pitch (lowered for more natural male baseline on Linux)
    1.0, 1.0, 1.0, 1.0, 1.0, 1.0,                   // cf
    1.3, 1.0, 1.0, 1.0, 1.0, 1.0,                   // cb
    1.0, 1.0, 1.0, 1.0, 1.0, 1.0,                   // pb
    1.0, 1.0, 1.0, 1.0,                               // pf3-6
    1.0, 1.0, 1.0, 1.3,                               // pa3-6
    0.85, 1.0, 1.0,                                    // fric, bypass, turbulence
    NAN, NAN, NAN, NAN, NAN,                           // abs overrides
    NAN, NAN, NAN, 1.0,                                // cf4/5/6 abs, cfNP_mul
    0.0, true},                                        // voicedTilt

  // Benjamin: brighter upper formants, wider cb1
  {"Benjamin",
    1.0, 1.0,
    1.01, 1.02, 1.0, 1.0, 1.0, 1.0,
    1.3, 1.0, 1.0, 1.0, 1.0, 1.0,
    1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
    1.0, 1.0, 1.0, 1.0,
    1.0, 1.0, 1.0, 1.3,
    0.7, 1.0, 1.0,
    NAN, NAN, NAN, NAN, NAN,
    3770.0, 4100.0, 5000.0, 0.9,
    0.0, true},

  // Caleb: whisper (no voicing, full aspiration)
  {"Caleb",
    1.0, 1.0,
    1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
    1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
    1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
    1.0, 1.0, 1.0, 1.0,
    1.0, 1.0, 1.0, 1.0,
    1.0, 1.0, 1.0,
    0.0, 1.0, NAN, NAN, NAN,                          // voiceAmp=0, aspirAmp=1
    NAN, NAN, NAN, 1.0,
    0.0, true},

  // David: deep voice — lower pitch, narrower formants
  {"David",
    0.75, 0.75,
    0.90, 0.93, 0.95, 1.0, 1.0, 1.0,
    1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
    1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
    1.0, 1.0, 1.0, 1.0,
    1.0, 1.0, 1.0, 1.0,
    1.0, 1.0, 1.0,
    NAN, NAN, NAN, NAN, NAN,
    NAN, NAN, NAN, 1.0,
    0.0, true},

  // Robert: bright, crisp, synthetic — pressed glottis, narrow bandwidths, negative tilt
  {"Robert",
    1.10, 1.10,
    1.02, 1.06, 1.08, 1.08, 1.10, 1.05,             // cf
    0.65, 0.68, 0.72, 0.75, 0.78, 0.80,             // cb
    0.72, 0.75, 0.78, 0.80, 0.82, 0.85,             // pb
    1.06, 1.08, 1.10, 1.00,                           // pf3-6
    1.08, 1.15, 1.20, 1.25,                           // pa3-6
    0.75, 0.70, 0.20,                                  // fric, bypass, turbulence
    NAN, NAN, 0.30, 0.0, 0.0,                         // glottalOQ=0.30, vibrato off
    NAN, NAN, NAN, 1.0,
    -6.0, true},
};
static const int kBuiltinVoiceCount = sizeof(kBuiltinVoices) / sizeof(kBuiltinVoices[0]);

static const BuiltinVoice* findBuiltinVoice(const std::string& name) {
  for (int i = 0; i < kBuiltinVoiceCount; ++i) {
    // Case-insensitive compare
    std::string a = name, b = kBuiltinVoices[i].name;
    for (auto& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (a == b) return &kBuiltinVoices[i];
  }
  return nullptr;
}

// Apply built-in voice multipliers to a frame (in-place).
static void applyBuiltinVoice(speechPlayer_frame_t& f, const BuiltinVoice& v) {
  f.voicePitch       *= v.voicePitch_mul;
  f.endVoicePitch    *= v.endVoicePitch_mul;
  f.cf1 *= v.cf1_mul;  f.cf2 *= v.cf2_mul;  f.cf3 *= v.cf3_mul;
  f.cf4 *= v.cf4_mul;  f.cf5 *= v.cf5_mul;  f.cf6 *= v.cf6_mul;
  f.cb1 *= v.cb1_mul;  f.cb2 *= v.cb2_mul;  f.cb3 *= v.cb3_mul;
  f.cb4 *= v.cb4_mul;  f.cb5 *= v.cb5_mul;  f.cb6 *= v.cb6_mul;
  f.pa3 *= v.pa3_mul;  f.pa4 *= v.pa4_mul;  f.pa5 *= v.pa5_mul;  f.pa6 *= v.pa6_mul;
  f.pb1 *= v.pb1_mul;  f.pb2 *= v.pb2_mul;  f.pb3 *= v.pb3_mul;
  f.pb4 *= v.pb4_mul;  f.pb5 *= v.pb5_mul;  f.pb6 *= v.pb6_mul;
  f.pf3 *= v.pf3_mul;  f.pf4 *= v.pf4_mul;  f.pf5 *= v.pf5_mul;  f.pf6 *= v.pf6_mul;
  f.fricationAmplitude     *= v.fricationAmplitude_mul;
  f.parallelBypass         *= v.parallelBypass_mul;
  f.voiceTurbulenceAmplitude *= v.voiceTurbulenceAmplitude_mul;
  f.cfNP *= v.cfNP_mul;
  // Absolute overrides
  if (!std::isnan(v.voiceAmplitude_abs))      f.voiceAmplitude = v.voiceAmplitude_abs;
  if (!std::isnan(v.aspirationAmplitude_abs)) f.aspirationAmplitude = v.aspirationAmplitude_abs;
  if (!std::isnan(v.glottalOpenQuotient_abs)) f.glottalOpenQuotient = v.glottalOpenQuotient_abs;
  if (!std::isnan(v.vibratoPitchOffset_abs))  f.vibratoPitchOffset = v.vibratoPitchOffset_abs;
  if (!std::isnan(v.vibratoSpeed_abs))        f.vibratoSpeed = v.vibratoSpeed_abs;
  if (!std::isnan(v.cf4_abs)) f.cf4 = v.cf4_abs;
  if (!std::isnan(v.cf5_abs)) f.cf5 = v.cf5_abs;
  if (!std::isnan(v.cf6_abs)) f.cf6 = v.cf6_abs;
}

// ============================================================================
// Emoji padding (port from tgsb_bridge.cpp — pads emoji with spaces for eSpeak)
// ============================================================================

static std::string padEmojiWithSpaces(const char *text) {
  std::string out;
  out.reserve(std::strlen(text) * 2);
  const unsigned char *p = (const unsigned char *)text;
  while (*p) {
    // 4-byte UTF-8: emoji in U+1F000..U+1FFFF (F0 9F xx xx)
    if (p[0] == 0xF0 && p[1] >= 0x9F && p[1] <= 0x9F &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
      if (!out.empty() && out.back() != ' ') out += ' ';
      out += (char)p[0]; out += (char)p[1];
      out += (char)p[2]; out += (char)p[3];
      p += 4;
      while (p[0] == 0xEF && p[1] == 0xB8 && (p[2] == 0x8E || p[2] == 0x8F)) {
        out += (char)p[0]; out += (char)p[1]; out += (char)p[2];
        p += 3;
      }
      if (*p && *p != ' ') out += ' ';
      continue;
    }
    // 3-byte UTF-8: U+2600..U+27BF (E2 98 80..E2 9E BF)
    if (p[0] == 0xE2 && p[1] >= 0x98 && p[1] <= 0x9E &&
        (p[2] & 0xC0) == 0x80) {
      if (!out.empty() && out.back() != ' ') out += ' ';
      out += (char)p[0]; out += (char)p[1]; out += (char)p[2];
      p += 3;
      while (p[0] == 0xEF && p[1] == 0xB8 && (p[2] == 0x8E || p[2] == 0x8F)) {
        out += (char)p[0]; out += (char)p[1]; out += (char)p[2];
        p += 3;
      }
      if (*p && *p != ' ') out += ' ';
      continue;
    }
    out += (char)*p++;
  }
  return out;
}

// ============================================================================
// eSpeak-NG runtime loading via dlopen (Linux) / LoadLibrary (Windows)
//
// We load libespeak-ng at runtime rather than link-time so that:
//   - No build dependency on libespeak-ng-dev (CI stays simple)
//   - No cross-compilation headaches for aarch64
//   - tgsbRender still works without espeak (IPA-from-stdin mode)
// ============================================================================

#if !defined(_WIN32)

// Minimal espeak types — stable across all espeak-ng versions.
// We only need 4 functions for phonemization (no audio synthesis).
enum { ESPEAK_AUDIO_OUTPUT_SYNCH = 0x02 };
enum { ESPEAK_CHARS_UTF8 = 1 };
enum { ESPEAK_INITIALIZE_DONT_EXIT = 0x8000 };
enum { ESPEAK_EE_OK = 0 };

struct espeak_VOICE_local {
  const char *name;
  const char *languages;
  const char *identifier;
  unsigned char gender;
  unsigned char age;
  unsigned char variant;
  unsigned char xx1;
  int score;
  void *spare;
};

// Function pointer types
typedef int  (*fn_espeak_Initialize)(int, int, const char*, int);
typedef int  (*fn_espeak_SetVoiceByProperties)(espeak_VOICE_local*);
typedef const char* (*fn_espeak_TextToPhonemes)(const void**, int, int);
typedef int  (*fn_espeak_Terminate)(void);

struct EspeakLib {
  void* handle = nullptr;
  fn_espeak_Initialize       Initialize = nullptr;
  fn_espeak_SetVoiceByProperties SetVoiceByProperties = nullptr;
  fn_espeak_TextToPhonemes   TextToPhonemes = nullptr;
  fn_espeak_Terminate        Terminate = nullptr;

  bool load(const char* espeakDataPath) {
    // Try versioned name first, then unversioned
    handle = dlopen("libespeak-ng.so.1", RTLD_LAZY);
    if (!handle) handle = dlopen("libespeak-ng.so", RTLD_LAZY);
    if (!handle) {
      std::cerr << "--espeak: cannot load libespeak-ng.so: " << dlerror() << "\n";
      return false;
    }

    Initialize = (fn_espeak_Initialize)dlsym(handle, "espeak_Initialize");
    SetVoiceByProperties = (fn_espeak_SetVoiceByProperties)dlsym(handle, "espeak_SetVoiceByProperties");
    TextToPhonemes = (fn_espeak_TextToPhonemes)dlsym(handle, "espeak_TextToPhonemes");
    Terminate = (fn_espeak_Terminate)dlsym(handle, "espeak_Terminate");

    if (!Initialize || !SetVoiceByProperties || !TextToPhonemes || !Terminate) {
      std::cerr << "--espeak: missing espeak-ng symbols in library\n";
      dlclose(handle);
      handle = nullptr;
      return false;
    }

    // Initialize espeak for phonemization only (synchronous, no audio)
    int sr = Initialize(ESPEAK_AUDIO_OUTPUT_SYNCH, 0,
                        (espeakDataPath && espeakDataPath[0]) ? espeakDataPath : nullptr,
                        ESPEAK_INITIALIZE_DONT_EXIT);
    if (sr <= 0) {
      std::cerr << "--espeak: espeak_Initialize failed\n";
      dlclose(handle);
      handle = nullptr;
      return false;
    }

    return true;
  }

  bool setVoice(const char* lang) {
    if (!SetVoiceByProperties) return false;
    espeak_VOICE_local spec{};
    spec.languages = lang;
    return SetVoiceByProperties(&spec) == ESPEAK_EE_OK;
  }

  std::string phonemize(const char* text) {
    if (!TextToPhonemes) return {};
    std::string combined;
    const void *ptr = text;
    while (ptr && *(const char*)ptr) {
      const char *ipa = TextToPhonemes(&ptr, ESPEAK_CHARS_UTF8, 0x02 /*IPA*/);
      if (!ipa || !*ipa) continue;
      if (!combined.empty()) combined += ' ';
      combined += ipa;
    }
    return combined;
  }

  void close() {
    if (handle) {
      if (Terminate) Terminate();
      dlclose(handle);
      handle = nullptr;
    }
  }
};

#endif // !_WIN32

// ============================================================================
// Options
// ============================================================================

struct Options {
  std::string packDir = ".";      // Directory containing "packs/" or the packs folder itself.
  std::string language = "en";    // e.g. "en", "en-us", "fr".
  std::string voiceProfile = "";  // Voice profile name (empty = default)
  std::string text = "";          // Original text for stress correction (optional)
  std::string clauseOverride = "";  // Explicit clause type override (e.g. "?" "!" "," ".")

  // Speech Dispatcher (SSIP) conventions are typically -100..+100 for rate.
  // We accept that and map it to a speed multiplier.
  int rate = 0;

  // We accept 0..100 (like eSpeak pitch after GenericPitch mapping).
  int pitch = 50;

  // We accept a linear gain multiplier. In Speech Dispatcher generic configs,
  // volume is often mapped to 0.0..2.0 (default 1.0).
  double volume = 1.0;

  // Output sample rate in Hz. Must match whatever plays the raw stream.
  int sampleRate = 16000;

  // Inflection range (octaves across +/-50% pitch path).
  double inflection = 0.5;

  // -------------------------------------------------------------------------
  // VoicingTone V3 parameters (0-100 sliders, mapped to actual values)
  // -------------------------------------------------------------------------
  int voicingPeakPos = 50;        // 0.85-0.95, default 0.91
  int voicedPreEmphA = 50;        // 0.0-0.97, default ~0.485
  int voicedPreEmphMix = 50;      // 0.0-1.0, default 0.5
  int highShelfGainDb = 50;       // -12 to +12 dB, default 0
  int highShelfFcHz = 50;         // 500-8000 Hz, default 4250
  int highShelfQ = 50;            // 0.3-2.0, default 1.15
  int voicedTiltDbPerOct = 50;    // -24 to +24, default 0
  int noiseGlottalModDepth = 0;   // 0.0-1.0, default 0
  int pitchSyncF1DeltaHz = 50;    // -60 to +60, default 0
  int pitchSyncB1DeltaHz = 50;    // -50 to +50, default 0
  int speedQuotient = 50;         // 0.5-4.0, default 2.0
  int aspirationTiltDbPerOct = 50; // -12 to +12, default 0
  int cascadeBwScale = 50;        // 0.4-1.4, default 1.0
  int tremor = 0;                 // 0.0-0.4, default 0 (no tremor)
  int chorusDepth = 0;            // 0.0-1.0, default 0 (off)
  int chorusDetune = 33;          // 0.5-5.0 Hz, default ~2.0 Hz

  // -------------------------------------------------------------------------
  // FrameEx parameters (0-100 sliders)
  // -------------------------------------------------------------------------
  int creakiness = 0;             // 0.0-1.0, default 0
  int breathiness = 0;            // 0.0-1.0, default 0
  int jitter = 0;                 // 0.0-1.0, default 0
  int shimmer = 0;                // 0.0-1.0, default 0
  int sharpness = 50;             // 0.5-2.0 multiplier, default 1.0

  bool help = false;
  bool listVoices = false;        // --list-voices: print available voice profiles and exit
  bool rateBoost = false;         // --rate-boost: double speed, DSP time-stretch handles excess
  bool prepareTextMode = false;   // --prepare-text: output normalized text to stdout and exit
  bool espeakMode = false;        // --espeak: in-process espeak (text from --text, no stdin IPA)
  std::string espeakDataPath = "";  // --espeak-data: path to espeak-ng-data (empty = system default)
};

// ============================================================================
// Helpers
// ============================================================================

static void printHelp(const char* argv0) {
  std::cerr
    << "Usage: " << (argv0 ? argv0 : "nvspRender") << " [options]\n\n"
    << "Reads IPA text from stdin (UTF-8) and writes raw 16-bit PCM to stdout.\n\n"
    << "Basic options:\n"
    << "  --packdir <path>      Path to repo root or packs dir (default: .)\n"
    << "  --lang <tag>          Language tag for pack selection (default: en)\n"
    << "  --voice <name>        Voice profile name (loads voicingTone from YAML)\n"
    << "  --list-voices         List available voice profiles and exit\n"
    << "  --text <string>       Original text for stress correction (optional)\n"
    << "  --clause <char>       Clause type override: . ? ! , (default: auto-detect)\n"
    << "  --rate <int>          SSIP-style rate -100..100 (default: 0)\n"
    << "  --pitch <int>         Pitch 0..100 (default: 50)\n"
    << "  --volume <float>      Output gain multiplier (default: 1.0)\n"
    << "  --samplerate <int>    Output sample rate in Hz (default: 16000)\n"
    << "  --inflection <float>  Inflection amount (default: 0.5)\n"
    << "\n"
    << "VoicingTone parameters (0-100 sliders):\n"
    << "  --voicing-peak-pos <int>       Glottal pulse peak position (default: 50)\n"
    << "  --voiced-preemph-a <int>       Pre-emphasis coefficient (default: 50)\n"
    << "  --voiced-preemph-mix <int>     Pre-emphasis mix (default: 50)\n"
    << "  --high-shelf-gain <int>        High shelf gain dB (default: 50)\n"
    << "  --high-shelf-fc <int>          High shelf frequency (default: 50)\n"
    << "  --high-shelf-q <int>           High shelf Q (default: 50)\n"
    << "  --voiced-tilt <int>            Voiced spectral tilt dB/oct (default: 50)\n"
    << "  --noise-glottal-mod <int>      Noise glottal modulation depth (default: 0)\n"
    << "  --pitch-sync-f1 <int>          Pitch-sync F1 delta Hz (default: 50)\n"
    << "  --pitch-sync-b1 <int>          Pitch-sync B1 delta Hz (default: 50)\n"
    << "  --speed-quotient <int>         Glottal pulse asymmetry (default: 50)\n"
    << "  --aspiration-tilt <int>        Aspiration spectral tilt (default: 50)\n"
    << "  --cascade-bw-scale <int>       Formant sharpness (cascade bandwidth) (default: 50)\n"
    << "  --formant-sharpness <int>      Formant sharpness (cascade bandwidth, default: 50)\n"
    << "  --tremor <int>                 Voice tremor / shakiness (default: 0)\n"
    << "  --chorus-depth <int>           Dual-oscillator chorus depth (default: 0)\n"
    << "  --chorus-detune <int>          Chorus detune Hz (default: 33, maps 0.5-5.0 Hz)\n"
    << "\n"
    << "FrameEx voice quality parameters (0-100 sliders):\n"
    << "  --creakiness <int>    Laryngealization / creaky voice (default: 0)\n"
    << "  --breathiness <int>   Breath noise in voicing (default: 0)\n"
    << "  --jitter <int>        Pitch period variation (default: 0)\n"
    << "  --shimmer <int>       Amplitude variation (default: 0)\n"
    << "  --sharpness <int>     Glottal closure sharpness (default: 50)\n"
    << "\n"
    << "Rate boost:\n"
    << "  --rate-boost          Double effective speed with DSP time-stretch\n"
    << "\n"
    << "In-process eSpeak (eliminates pipe chain shimmer):\n"
    << "  --espeak              Phonemize text from --text using system espeak-ng\n"
    << "                        instead of reading IPA from stdin. Requires\n"
    << "                        libespeak-ng.so on the system (Linux only).\n"
    << "  --espeak-data <path>  Path to espeak-ng-data (default: system default)\n"
    << "\n"
    << "Text normalization:\n"
    << "  --prepare-text        Read text from stdin, apply dict/compound\n"
    << "                        normalization, write result to stdout, and exit.\n"
    << "                        Use in wrapper scripts before piping to eSpeak.\n"
    << "\n"
    << "  -h, --help            Show this help\n";
}

static bool parseInt(const char* s, int& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (!end || *end != '\0') return false;
  out = static_cast<int>(v);
  return true;
}

static bool parseDouble(const char* s, double& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  double v = std::strtod(s, &end);
  if (!end || *end != '\0') return false;
  out = v;
  return true;
}

static int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static Options parseArgs(int argc, char** argv) {
  Options opt;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i] ? argv[i] : "";

    if (a == "-h" || a == "--help") {
      opt.help = true;
      continue;
    }
    if (a == "--list-voices") {
      opt.listVoices = true;
      continue;
    }

    auto requireValue = [&](const char* name) -> const char* {
      if (i + 1 >= argc || !argv[i + 1]) {
        std::cerr << "Missing value for " << name << "\n";
        opt.help = true;
        return nullptr;
      }
      return argv[++i];
    };

    auto parseIntArg = [&](const char* name, int& target) {
      if (const char* v = requireValue(name)) {
        int tmp = 0;
        if (!parseInt(v, tmp)) {
          std::cerr << "Bad " << name << " value: " << v << "\n";
          opt.help = true;
        } else {
          target = tmp;
        }
      }
    };

    auto parseDoubleArg = [&](const char* name, double& target) {
      if (const char* v = requireValue(name)) {
        double tmp = 0;
        if (!parseDouble(v, tmp)) {
          std::cerr << "Bad " << name << " value: " << v << "\n";
          opt.help = true;
        } else {
          target = tmp;
        }
      }
    };

    // Basic options
    if (a == "--packdir") {
      if (const char* v = requireValue("--packdir")) opt.packDir = v;
      continue;
    }
    if (a == "--lang") {
      if (const char* v = requireValue("--lang")) opt.language = v;
      continue;
    }
    if (a == "--voice") {
      if (const char* v = requireValue("--voice")) opt.voiceProfile = v;
      continue;
    }
    if (a == "--text") {
      if (const char* v = requireValue("--text")) opt.text = v;
      continue;
    }
    if (a == "--clause") {
      if (const char* v = requireValue("--clause")) opt.clauseOverride = v;
      continue;
    }
    if (a == "--rate") { parseIntArg("--rate", opt.rate); continue; }
    if (a == "--pitch") { parseIntArg("--pitch", opt.pitch); continue; }
    if (a == "--volume") { parseDoubleArg("--volume", opt.volume); continue; }
    if (a == "--samplerate" || a == "--sample-rate") {
      if (const char* v = requireValue(a.c_str())) {
        int tmp = 16000;
        if (!parseInt(v, tmp) || tmp < 8000 || tmp > 192000) {
          std::cerr << "Bad --samplerate value: " << v << " (expected 8000..192000)\n";
          opt.help = true;
        } else {
          opt.sampleRate = tmp;
        }
      }
      continue;
    }
    if (a == "--inflection") { parseDoubleArg("--inflection", opt.inflection); continue; }
    if (a == "--rate-boost") { opt.rateBoost = true; continue; }
    if (a == "--prepare-text") { opt.prepareTextMode = true; continue; }
    if (a == "--espeak") { opt.espeakMode = true; continue; }
    if (a == "--espeak-data") {
      if (const char* v = requireValue("--espeak-data")) opt.espeakDataPath = v;
      continue;
    }

    // VoicingTone parameters
    if (a == "--voicing-peak-pos") { parseIntArg(a.c_str(), opt.voicingPeakPos); continue; }
    if (a == "--voiced-preemph-a") { parseIntArg(a.c_str(), opt.voicedPreEmphA); continue; }
    if (a == "--voiced-preemph-mix") { parseIntArg(a.c_str(), opt.voicedPreEmphMix); continue; }
    if (a == "--high-shelf-gain") { parseIntArg(a.c_str(), opt.highShelfGainDb); continue; }
    if (a == "--high-shelf-fc") { parseIntArg(a.c_str(), opt.highShelfFcHz); continue; }
    if (a == "--high-shelf-q") { parseIntArg(a.c_str(), opt.highShelfQ); continue; }
    if (a == "--voiced-tilt") { parseIntArg(a.c_str(), opt.voicedTiltDbPerOct); continue; }
    if (a == "--noise-glottal-mod") { parseIntArg(a.c_str(), opt.noiseGlottalModDepth); continue; }
    if (a == "--pitch-sync-f1") { parseIntArg(a.c_str(), opt.pitchSyncF1DeltaHz); continue; }
    if (a == "--pitch-sync-b1") { parseIntArg(a.c_str(), opt.pitchSyncB1DeltaHz); continue; }
    if (a == "--speed-quotient") { parseIntArg(a.c_str(), opt.speedQuotient); continue; }
    if (a == "--aspiration-tilt") { parseIntArg(a.c_str(), opt.aspirationTiltDbPerOct); continue; }
    if (a == "--cascade-bw-scale" || a == "--formant-sharpness") { parseIntArg(a.c_str(), opt.cascadeBwScale); continue; }
    if (a == "--tremor") { parseIntArg(a.c_str(), opt.tremor); continue; }
    if (a == "--chorus-depth") { parseIntArg(a.c_str(), opt.chorusDepth); continue; }
    if (a == "--chorus-detune") { parseIntArg(a.c_str(), opt.chorusDetune); continue; }

    // FrameEx parameters
    if (a == "--creakiness") { parseIntArg(a.c_str(), opt.creakiness); continue; }
    if (a == "--breathiness") { parseIntArg(a.c_str(), opt.breathiness); continue; }
    if (a == "--jitter") { parseIntArg(a.c_str(), opt.jitter); continue; }
    if (a == "--shimmer") { parseIntArg(a.c_str(), opt.shimmer); continue; }
    if (a == "--sharpness") { parseIntArg(a.c_str(), opt.sharpness); continue; }

    std::cerr << "Unknown arg: " << a << "\n";
    opt.help = true;
  }

  return opt;
}

static std::string readAllStdin() {
  std::string out;
  char buf[4096];
  while (true) {
    size_t n = std::fread(buf, 1, sizeof(buf), stdin);
    if (n > 0) out.append(buf, n);
    if (n < sizeof(buf)) {
      if (std::feof(stdin)) break;
      if (std::ferror(stdin)) break;
    }
  }
  return out;
}

// ============================================================================
// VoicingTone builder (slider 0-100 -> actual values)
// ============================================================================

static VoicingToneV3 buildVoicingTone(const Options& opt) {
  VoicingToneV3 tone{};
  
  tone.magic = SPEECHPLAYER_VOICINGTONE_MAGIC;
  tone.structSize = sizeof(VoicingToneV3);
  tone.structVersion = SPEECHPLAYER_VOICINGTONE_VERSION;
  tone.dspVersion = SPEECHPLAYER_DSP_VERSION;

  auto slider = [](int v) { return static_cast<double>(clampInt(v, 0, 100)) / 100.0; };

  // Map sliders to actual values (same mapping as NVDA driver and phoneme editor)
  tone.voicingPeakPos = 0.85 + slider(opt.voicingPeakPos) * 0.10;           // 0.85-0.95
  tone.voicedPreEmphA = slider(opt.voicedPreEmphA) * 0.97;                  // 0.0-0.97
  tone.voicedPreEmphMix = slider(opt.voicedPreEmphMix);                     // 0.0-1.0
  tone.highShelfGainDb = -12.0 + slider(opt.highShelfGainDb) * 24.0;        // -12 to +12
  tone.highShelfFcHz = 500.0 + slider(opt.highShelfFcHz) * 7500.0;          // 500-8000
  tone.highShelfQ = 0.3 + slider(opt.highShelfQ) * 1.7;                     // 0.3-2.0
  tone.voicedTiltDbPerOct = -24.0 + slider(opt.voicedTiltDbPerOct) * 48.0;  // -24 to +24
  tone.noiseGlottalModDepth = slider(opt.noiseGlottalModDepth);             // 0.0-1.0
  tone.pitchSyncF1DeltaHz = -60.0 + slider(opt.pitchSyncF1DeltaHz) * 120.0; // -60 to +60
  tone.pitchSyncB1DeltaHz = -50.0 + slider(opt.pitchSyncB1DeltaHz) * 100.0; // -50 to +50
  tone.speedQuotient = 0.5 + slider(opt.speedQuotient) * 3.5;               // 0.5-4.0
  tone.aspirationTiltDbPerOct = -12.0 + slider(opt.aspirationTiltDbPerOct) * 24.0; // -12 to +12
  // cascadeBwScale: piecewise so that 50 => 1.0
  {
    const int s = clampInt(opt.cascadeBwScale, 0, 100);
    if (s <= 50) tone.cascadeBwScale = 2.0 - (static_cast<double>(s) / 50.0) * 1.0;
    else tone.cascadeBwScale = 1.0 - (static_cast<double>(s - 50) / 50.0) * 0.7;
  }
  // tremorDepth: 0-100 maps to 0.0-0.4
  tone.tremorDepth = slider(opt.tremor) * 0.4;
  // V4: vocal tract shape defaults (neutral)
  tone.nasalBwScale = 1.0;
  tone.f4FreqScale = 1.0;
  tone.nasalGainScale = 1.0;
  // V5: chorus
  tone.chorusDepth = slider(opt.chorusDepth);                      // 0-100 -> 0.0-1.0
  tone.chorusDetuneHz = 0.5 + slider(opt.chorusDetune) * 4.5;     // 0-100 -> 0.5-5.0 Hz

  return tone;
}

// ============================================================================
// FrameEx builder (slider 0-100 -> actual values)
// ============================================================================

static FrameEx buildFrameEx(const Options& opt, bool& outHasEffect) {
  FrameEx ex{};
  outHasEffect = false;

  auto slider = [](int v) { return static_cast<double>(clampInt(v, 0, 100)) / 100.0; };

  ex.creakiness = slider(opt.creakiness);
  ex.breathiness = slider(opt.breathiness);
  ex.jitter = slider(opt.jitter);
  ex.shimmer = slider(opt.shimmer);
  // sharpness: 0-100 -> 0.5-2.0 multiplier (50 = 1.0 = neutral)
  ex.sharpness = 0.5 + slider(opt.sharpness) * 1.5;

  // Check if any effect is active
  outHasEffect = (opt.creakiness > 0 || opt.breathiness > 0 || 
                  opt.jitter > 0 || opt.shimmer > 0 || opt.sharpness != 50);

  return ex;
}

// ============================================================================
// Callback context
// ============================================================================

struct CallbackCtx {
  speechPlayer_handle_t player = nullptr;
  int sampleRate = 16000;
  double volume = 1.0;
  FrameEx userFrameEx{};      // User-level defaults from CLI (additive)
  bool hasUserFrameEx = false;
  const BuiltinVoice* builtinVoice = nullptr;  // Non-null when using a hardcoded voice
};

static void onFrontendFrameEx(
    void* userData,
    const nvspFrontend_Frame* frameOrNull,
    const nvspFrontend_FrameEx* frameExOrNull,  // Per-phoneme FrameEx (e.g. Danish stød)
    double durationMs,
    double fadeMs,
    int userIndex
) {
  auto* ctx = static_cast<CallbackCtx*>(userData);
  if (!ctx || !ctx->player) return;

  auto msToSamples = [&](double ms) -> unsigned int {
    if (ms <= 0.0) return 0;
    const double s = (ms * static_cast<double>(ctx->sampleRate)) / 1000.0;
    if (s <= 0.0) return 0;
    return static_cast<unsigned int>(s + 0.5);
  };

  const unsigned int minSamples = msToSamples(durationMs);
  const unsigned int fadeSamples = msToSamples(fadeMs);

  if (frameOrNull) {
    static_assert(sizeof(nvspFrontend_Frame) == sizeof(speechPlayer_frame_t), "Frame ABI mismatch");
    speechPlayer_frame_t f{};
    std::memcpy(&f, frameOrNull, sizeof(f));
    f.outputGain *= ctx->volume;

    // Apply built-in voice preset multipliers (Adam, Benjamin, etc.)
    if (ctx->builtinVoice) {
      applyBuiltinVoice(f, *ctx->builtinVoice);
    }

    // Use FrameEx if we have per-phoneme values OR user CLI overrides
    if (frameExOrNull || ctx->hasUserFrameEx) {
      FrameEx merged{};
      
      // Start with per-phoneme values from frontend (includes Fujisaki pitch model)
      if (frameExOrNull) {
        // Copy all 27 fields - frontend provides formant ramping, Fujisaki, transition, and F7/F8 data
        std::memcpy(&merged, frameExOrNull, sizeof(FrameEx));
      } else {
        merged.sharpness = 1.0;  // Neutral default for sharpness
        // Formant end targets: 0.0 is fine (DSP treats as "no target")
        // Fujisaki fields: 0.0 means disabled
      }
      
      // Add user CLI overrides for voice quality params only
      // (additive for 0-1 params, multiplicative for sharpness)
      if (ctx->hasUserFrameEx) {
        merged.creakiness = std::min(1.0, merged.creakiness + ctx->userFrameEx.creakiness);
        merged.breathiness = std::min(1.0, merged.breathiness + ctx->userFrameEx.breathiness);
        merged.jitter = std::min(1.0, merged.jitter + ctx->userFrameEx.jitter);
        merged.shimmer = std::min(1.0, merged.shimmer + ctx->userFrameEx.shimmer);
        merged.sharpness *= ctx->userFrameEx.sharpness;
        // Note: formant end targets and Fujisaki params come from frontend only,
        // no CLI overrides for those (they're per-phoneme/per-utterance)
      }
      
      speechPlayer_queueFrameEx(ctx->player, &f,
                                reinterpret_cast<const speechPlayer_frameEx_t*>(&merged),
                                static_cast<unsigned int>(sizeof(FrameEx)),
                                minSamples, fadeSamples, userIndex, false);
    } else {
      speechPlayer_queueFrame(ctx->player, &f, minSamples, fadeSamples, userIndex, false);
    }
  } else {
    // Silence frame - no FrameEx needed
    speechPlayer_queueFrame(ctx->player, nullptr, minSamples, fadeSamples, userIndex, false);
  }
}

// Map Speech Dispatcher SSIP rate (-100..+100) to a speed multiplier.
static double ssipRateToSpeed(int ssipRate) {
  if (ssipRate < -100) ssipRate = -100;
  if (ssipRate > 100) ssipRate = 100;
  return std::pow(2.0, static_cast<double>(ssipRate) / 100.0);
}

// Map a 0..100 pitch slider to a base pitch in Hz.
static double sliderPitchToBaseHz(int pitch0to100) {
  if (pitch0to100 < 0) pitch0to100 = 0;
  if (pitch0to100 > 100) pitch0to100 = 100;
  return 25.0 + (21.25 * (static_cast<double>(pitch0to100) / 12.5));
}

// Check if any VoicingTone parameter is non-default
static bool hasVoicingToneEffect(const Options& opt) {
  return (opt.voicingPeakPos != 50 || opt.voicedPreEmphA != 50 ||
          opt.voicedPreEmphMix != 50 || opt.highShelfGainDb != 50 ||
          opt.highShelfFcHz != 50 || opt.highShelfQ != 50 ||
          opt.voicedTiltDbPerOct != 50 || opt.noiseGlottalModDepth != 0 ||
          opt.pitchSyncF1DeltaHz != 50 || opt.pitchSyncB1DeltaHz != 50 ||
          opt.speedQuotient != 50 || opt.aspirationTiltDbPerOct != 50 ||
          opt.cascadeBwScale != 50 || opt.tremor != 0 ||
          opt.chorusDepth != 0 || opt.chorusDetune != 33);
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parseArgs(argc, argv);
  if (opt.help) {
    printHelp(argv && argv[0] ? argv[0] : "nvspRender");
    return 2;
  }

  // Handle --list-voices: print available profiles and exit
  if (opt.listVoices) {
    nvspFrontend_handle_t fe = nvspFrontend_create(opt.packDir.c_str());
    if (!fe) {
      std::cerr << "nvspFrontend_create failed (packDir='" << opt.packDir << "')\n";
      return 1;
    }
    if (!nvspFrontend_setLanguage(fe, opt.language.c_str())) {
      std::cerr << "nvspFrontend_setLanguage failed (lang='" << opt.language << "')\n";
      nvspFrontend_destroy(fe);
      return 1;
    }
    
    // List built-in voices first
    std::cerr << "Built-in voices:\n";
    for (int i = 0; i < kBuiltinVoiceCount; ++i) {
      std::cerr << "  " << kBuiltinVoices[i].name << "\n";
    }

    const char* names = nvspFrontend_getVoiceProfileNames(fe);
    if (names && *names) {
      std::cerr << "\nYAML voice profiles:\n";
      std::string nameStr = names;
      std::string::size_type pos = 0, prev = 0;
      while ((pos = nameStr.find('\n', prev)) != std::string::npos) {
        std::string name = nameStr.substr(prev, pos - prev);
        if (!name.empty()) {
          std::cerr << "  " << name << "\n";
        }
        prev = pos + 1;
      }
      if (prev < nameStr.size()) {
        std::cerr << "  " << nameStr.substr(prev) << "\n";
      }
      std::cerr << "\nExample speech-dispatcher AddVoice lines:\n";
      prev = 0;
      while ((pos = nameStr.find('\n', prev)) != std::string::npos) {
        std::string name = nameStr.substr(prev, pos - prev);
        if (!name.empty()) {
          std::cerr << "  AddVoice \"en\" \"male1\" \"" << name << "\"\n";
        }
        prev = pos + 1;
      }
      if (prev < nameStr.size()) {
        std::cerr << "  AddVoice \"en\" \"male1\" \"" << nameStr.substr(prev) << "\"\n";
      }
    } else {
      std::cerr << "No voice profiles found.\n";
    }
    nvspFrontend_destroy(fe);
    return 0;
  }

  // --prepare-text mode: read text from stdin, normalize, write to stdout.
  // Used by wrapper scripts to apply dict/compound transforms before eSpeak.
  if (opt.prepareTextMode) {
    const std::string text = readAllStdin();
    if (text.empty()) return 0;

    nvspFrontend_handle_t fe = nvspFrontend_create(opt.packDir.c_str());
    if (!fe) {
      std::cerr << "nvspFrontend_create failed\n";
      return 1;
    }
    if (!opt.language.empty()) {
      nvspFrontend_setLanguage(fe, opt.language.c_str());
    }

    char* prepared = nvspFrontend_prepareText(fe, text.c_str());
    const char* output = prepared ? prepared : text.c_str();
    std::fwrite(output, 1, std::strlen(output), stdout);
    if (prepared) nvspFrontend_freeString(prepared);
    nvspFrontend_destroy(fe);
    return 0;
  }

#if defined(_WIN32)
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  // In --espeak mode, text comes from --text argument (no stdin).
  // Otherwise, read IPA from stdin as before.
  std::string ipa;
  if (opt.espeakMode) {
    if (opt.text.empty()) {
      std::cerr << "--espeak requires --text <text>\n";
      return 2;
    }
  } else {
    ipa = readAllStdin();
    if (ipa.empty()) return 0;
  }

  // Initialize speechPlayer with the requested sample rate
  const int sampleRate = opt.sampleRate;
  speechPlayer_handle_t player = speechPlayer_initialize(sampleRate);
  if (!player) {
    std::cerr << "speechPlayer_initialize failed\n";
    return 1;
  }

  // Initialize frontend
  nvspFrontend_handle_t fe = nvspFrontend_create(opt.packDir.c_str());
  if (!fe) {
    std::cerr << "nvspFrontend_create failed (packDir='" << opt.packDir << "')\n";
    speechPlayer_terminate(player);
    return 1;
  }

  if (!nvspFrontend_setLanguage(fe, opt.language.c_str())) {
    std::cerr << "nvspFrontend_setLanguage failed (lang='" << opt.language << "')\n";
    const char* err = nvspFrontend_getLastError(fe);
    if (err && *err) std::cerr << "  " << err << "\n";
    nvspFrontend_destroy(fe);
    speechPlayer_terminate(player);
    return 1;
  }

  // Set voice profile if specified.
  // First try YAML profiles (Beth, Bobby, user-defined).
  // If no YAML profile matches, check built-in hardcoded voices (Adam, Benjamin, etc.).
  const BuiltinVoice* activeBuiltin = nullptr;
  if (!opt.voiceProfile.empty()) {
    // Try YAML profile first
    nvspFrontend_setVoiceProfile(fe, opt.voiceProfile.c_str());

    // Check if a built-in voice matches (these aren't in YAML)
    activeBuiltin = findBuiltinVoice(opt.voiceProfile);
  }

  // Apply VoicingTone: first try to load from YAML via frontend, then apply CLI overrides
  {
    VoicingToneV3 tone{};
    tone.magic = SPEECHPLAYER_VOICINGTONE_MAGIC;
    tone.structSize = sizeof(VoicingToneV3);
    tone.structVersion = SPEECHPLAYER_VOICINGTONE_VERSION;
    tone.dspVersion = SPEECHPLAYER_DSP_VERSION;
    
    // Start with defaults
    tone.voicingPeakPos = 0.91;
    tone.voicedPreEmphA = 0.92;
    tone.voicedPreEmphMix = 0.35;
    tone.highShelfGainDb = 4.0;
    tone.highShelfFcHz = 2000.0;
    tone.highShelfQ = 0.7;
    tone.voicedTiltDbPerOct = 0.0;
    tone.noiseGlottalModDepth = 0.0;
    tone.pitchSyncF1DeltaHz = 0.0;
    tone.pitchSyncB1DeltaHz = 0.0;
    tone.speedQuotient = 2.0;
    tone.aspirationTiltDbPerOct = 0.0;
    tone.cascadeBwScale = 1.0;
    tone.tremorDepth = 0.0;
    tone.nasalBwScale = 1.0;
    tone.f4FreqScale = 1.0;
    tone.nasalGainScale = 1.0;
    tone.chorusDepth = 0.0;
    tone.chorusDetuneHz = 2.0;

    // Try to get voicing tone from YAML (if voice profile has one)
    nvspFrontend_VoicingTone yamlTone{};
    if (nvspFrontend_getVoicingTone(fe, &yamlTone)) {
      // Copy YAML values
      tone.voicingPeakPos = yamlTone.voicingPeakPos;
      tone.voicedPreEmphA = yamlTone.voicedPreEmphA;
      tone.voicedPreEmphMix = yamlTone.voicedPreEmphMix;
      tone.highShelfGainDb = yamlTone.highShelfGainDb;
      tone.highShelfFcHz = yamlTone.highShelfFcHz;
      tone.highShelfQ = yamlTone.highShelfQ;
      tone.voicedTiltDbPerOct = yamlTone.voicedTiltDbPerOct;
      tone.noiseGlottalModDepth = yamlTone.noiseGlottalModDepth;
      tone.pitchSyncF1DeltaHz = yamlTone.pitchSyncF1DeltaHz;
      tone.pitchSyncB1DeltaHz = yamlTone.pitchSyncB1DeltaHz;
      tone.speedQuotient = yamlTone.speedQuotient;
      tone.aspirationTiltDbPerOct = yamlTone.aspirationTiltDbPerOct;
      tone.cascadeBwScale = yamlTone.cascadeBwScale;
      tone.tremorDepth = yamlTone.tremorDepth;
    }
    
    // Apply built-in voice voicedTilt (between YAML and CLI so CLI can still override)
    if (activeBuiltin && activeBuiltin->hasVoicedTilt) {
      tone.voicedTiltDbPerOct = activeBuiltin->voicedTiltDbPerOct;
    }

    // Apply CLI overrides (only if non-default)
    if (hasVoicingToneEffect(opt)) {
      VoicingToneV3 cliTone = buildVoicingTone(opt);
      // CLI args override YAML values
      if (opt.voicingPeakPos != 50) tone.voicingPeakPos = cliTone.voicingPeakPos;
      if (opt.voicedPreEmphA != 50) tone.voicedPreEmphA = cliTone.voicedPreEmphA;
      if (opt.voicedPreEmphMix != 50) tone.voicedPreEmphMix = cliTone.voicedPreEmphMix;
      if (opt.highShelfGainDb != 50) tone.highShelfGainDb = cliTone.highShelfGainDb;
      if (opt.highShelfFcHz != 50) tone.highShelfFcHz = cliTone.highShelfFcHz;
      if (opt.highShelfQ != 50) tone.highShelfQ = cliTone.highShelfQ;
      if (opt.voicedTiltDbPerOct != 50) tone.voicedTiltDbPerOct = cliTone.voicedTiltDbPerOct;
      if (opt.noiseGlottalModDepth != 0) tone.noiseGlottalModDepth = cliTone.noiseGlottalModDepth;
      if (opt.pitchSyncF1DeltaHz != 50) tone.pitchSyncF1DeltaHz = cliTone.pitchSyncF1DeltaHz;
      if (opt.pitchSyncB1DeltaHz != 50) tone.pitchSyncB1DeltaHz = cliTone.pitchSyncB1DeltaHz;
      if (opt.speedQuotient != 50) tone.speedQuotient = cliTone.speedQuotient;
      if (opt.aspirationTiltDbPerOct != 50) tone.aspirationTiltDbPerOct = cliTone.aspirationTiltDbPerOct;
      if (opt.cascadeBwScale != 50) tone.cascadeBwScale = cliTone.cascadeBwScale;
      if (opt.tremor != 0) tone.tremorDepth = cliTone.tremorDepth;
      if (opt.chorusDepth != 0) tone.chorusDepth = cliTone.chorusDepth;
      if (opt.chorusDetune != 33) tone.chorusDetuneHz = cliTone.chorusDetuneHz;
    }

    speechPlayer_setVoicingTone(player, reinterpret_cast<const speechPlayer_voicingTone_t*>(&tone));
  }

  // Build user-level FrameEx defaults from CLI args
  bool hasUserFrameEx = false;
  FrameEx userFrameEx = buildFrameEx(opt, hasUserFrameEx);

  CallbackCtx cbCtx;
  cbCtx.player = player;
  cbCtx.sampleRate = sampleRate;
  cbCtx.volume = opt.volume;
  cbCtx.userFrameEx = userFrameEx;
  cbCtx.hasUserFrameEx = hasUserFrameEx;
  cbCtx.builtinVoice = activeBuiltin;

  double speed = ssipRateToSpeed(opt.rate);
  if (opt.rateBoost) speed *= 2.0;
  // Cap synthesis at 2.0x, put excess into DSP time-stretch.
  double timeStretch = 1.0;
  if (speed > 2.0) {
    timeStretch = speed / 2.0;
    speed = 2.0;
  }
  speechPlayer_setTimeStretch(player, timeStretch);
  const double basePitchHz = sliderPitchToBaseHz(opt.pitch);
  const double inflection = opt.inflection;

  // ========================================================================
  // --espeak mode: in-process text-to-IPA via libespeak-ng.so (Linux only).
  // Adapted from tgsb_bridge.cpp — same clause splitting as iOS/Android.
  // Eliminates the espeak CLI → pipe → tgsbRender pipe chain.
  // ========================================================================
#if !defined(_WIN32)
  if (opt.espeakMode) {
    EspeakLib espeak;
    if (!espeak.load(opt.espeakDataPath.empty() ? nullptr : opt.espeakDataPath.c_str())) {
      nvspFrontend_destroy(fe);
      speechPlayer_terminate(player);
      return 1;
    }

    // Map our language tag to an espeak voice.
    // eSpeak uses bare tags like "en-us", "de", "fr" — same as ours.
    if (!espeak.setVoice(opt.language.c_str())) {
      std::cerr << "--espeak: failed to set voice for '" << opt.language << "'\n";
      espeak.close();
      nvspFrontend_destroy(fe);
      speechPlayer_terminate(player);
      return 1;
    }

    // Clause-splitting loop (same algorithm as tgsb_bridge.cpp / tgsb_jni.cpp).
    // Split text at sentence boundaries, feed each clause to espeak individually,
    // tag with correct clause type for prosody.
    const char *p = opt.text.c_str();
    while (*p) {
      // Skip leading whitespace
      while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
      if (!*p) break;

      const char *clauseStart = p;
      char clauseType = '.';

      while (*p) {
        char c = *p;
        if (c == '?' || c == '!') {
          clauseType = c;
          p++;
          break;
        }
        // comma/period between digits = thousands separator / decimal
        if (c == ',' || c == '.') {
          bool prevDigit = (p > clauseStart) &&
              (unsigned char)(*(p - 1) - '0') <= 9;
          bool nextDigit = *(p + 1) &&
              (unsigned char)(*(p + 1) - '0') <= 9;
          if (prevDigit && nextDigit) { p++; continue; }
          // Ordinal dot: "3. Mai" (German/Swedish/Czech/Finnish)
          if (c == '.' && prevDigit && *(p+1) == ' ' && *(p+2) &&
              ((unsigned char)(*(p+2) - 'A') <= 25 || (unsigned char)(*(p+2) - 'a') <= 25)) {
            p++; continue;
          }
          clauseType = c;
          p++;
          break;
        }
        // U+2026 ellipsis (UTF-8: E2 80 A6)
        if ((unsigned char)c == 0xE2 &&
            (unsigned char)*(p+1) == 0x80 &&
            (unsigned char)*(p+2) == 0xA6) {
          clauseType = '.';
          p += 3;
          break;
        }
        // colon/semicolon only split when followed by whitespace
        if (c == ';' || c == ':') {
          char next = *(p + 1);
          if (next == ' ' || next == '\t' || next == '\r' ||
              next == '\n' || next == '\0') {
            clauseType = ',';
            p++;
            break;
          }
        }
        p++;
      }

      size_t len = (size_t)(p - clauseStart);
      if (len == 0) continue;

      std::string clause(clauseStart, len);

      // Text normalization: compound splitting, date ordinals, etc.
      char *prepared = nvspFrontend_prepareText(fe, clause.c_str());
      if (prepared) {
        clause = prepared;
        nvspFrontend_freeString(prepared);
      }

      // Pad emoji with spaces so eSpeak treats them as separate words
      std::string padded = padEmojiWithSpaces(clause.c_str());
      if (padded.size() != clause.size()) clause = std::move(padded);

      // In-process phonemization: text → IPA
      std::string clauseIpa = espeak.phonemize(clause.c_str());
      if (clauseIpa.empty()) continue;

      char clauseStr[2] = { clauseType, '\0' };
      int ok = nvspFrontend_queueIPA_ExWithText(
        fe, clause.c_str(), clauseIpa.c_str(),
        speed, basePitchHz, inflection, clauseStr,
        0, &onFrontendFrameEx, &cbCtx
      );
      if (!ok) {
        std::cerr << "nvspFrontend_queueIPA failed for clause\n";
        const char* err = nvspFrontend_getLastError(fe);
        if (err && *err) std::cerr << "  " << err << "\n";
      }
    }

    espeak.close();
  } else
#endif // !_WIN32
  {
    // Original path: read IPA from stdin, split at clause boundaries.

    // Detect clause type from original text or explicit override.
    char textClauseType = 0;
    if (!opt.clauseOverride.empty()) {
      textClauseType = opt.clauseOverride[0];
    } else if (!opt.text.empty()) {
      for (auto it = opt.text.rbegin(); it != opt.text.rend(); ++it) {
        char c = *it;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '"' || c == '\'' || c == ')' || c == ']') continue;
        if ((unsigned char)c == 0xA6) {
          auto it2 = it; ++it2;
          if (it2 != opt.text.rend() && (unsigned char)*it2 == 0x80) {
            auto it3 = it2; ++it3;
            if (it3 != opt.text.rend() && (unsigned char)*it3 == 0xE2) {
              textClauseType = '.';
              break;
            }
          }
        }
        if ((unsigned char)c >= 0x80) continue;
        if (c == '.' || c == '?' || c == '!' || c == ',') {
          textClauseType = c;
          break;
        }
        break;
      }
      if (!textClauseType) textClauseType = '.';
    }

    const char* ip = ipa.c_str();
    bool anyFailed = false;

    while (*ip) {
      while (*ip == ' ' || *ip == '\t' || *ip == '\r' || *ip == '\n') ip++;
      if (!*ip) break;

      const char* clauseStart = ip;
      char clauseType = '.';

      while (*ip) {
        char c = *ip;
        if (c == '.' || c == '?' || c == '!' || c == ',') {
          clauseType = c;
          ip++;
          break;
        }
        if ((unsigned char)c == 0xE2 &&
            (unsigned char)*(ip+1) == 0x80 &&
            (unsigned char)*(ip+2) == 0xA6) {
          clauseType = '.';
          ip += 3;
          break;
        }
        if (c == ';' || c == ':') {
          char next = *(ip + 1);
          if (next == ' ' || next == '\t' || next == '\r' ||
              next == '\n' || next == '\0') {
            clauseType = ',';
            ip++;
            break;
          }
        }
        ip++;
      }

      if (textClauseType) clauseType = textClauseType;

      size_t len = static_cast<size_t>(ip - clauseStart);
      if (len == 0) continue;

      std::string clause(clauseStart, len);
      char clauseStr[2] = { clauseType, '\0' };

      int queueOk;
      if (!opt.text.empty()) {
        queueOk = nvspFrontend_queueIPA_ExWithText(
          fe, opt.text.c_str(), clause.c_str(),
          speed, basePitchHz, inflection, clauseStr,
          0, &onFrontendFrameEx, &cbCtx
        );
      } else {
        queueOk = nvspFrontend_queueIPA_Ex(
          fe, clause.c_str(),
          speed, basePitchHz, inflection, clauseStr,
          0, &onFrontendFrameEx, &cbCtx
        );
      }

      if (!queueOk) {
        std::cerr << "nvspFrontend_queueIPA failed for clause\n";
        const char* err = nvspFrontend_getLastError(fe);
        if (err && *err) std::cerr << "  " << err << "\n";
        anyFailed = true;
      }
    }

    if (anyFailed && ipa.find_first_not_of(" \t\r\n") != std::string::npos) {
      // At least one clause failed, but others may have succeeded.
    }
  }

  // Synthesize to stdout as raw PCM
  static_assert(sizeof(sample) == sizeof(sampleVal), "sample struct should be a packed 16-bit value");
  std::vector<sample> pcm;
  pcm.resize(2048);

  while (true) {
    const int n = speechPlayer_synthesize(player, static_cast<unsigned int>(pcm.size()), pcm.data());
    if (n <= 0) break;

    const size_t bytes = static_cast<size_t>(n) * sizeof(sample);
    size_t written = 0;
    while (written < bytes) {
      const size_t w = std::fwrite(reinterpret_cast<const char*>(pcm.data()) + written, 1, bytes - written, stdout);
      if (w == 0) {
        nvspFrontend_destroy(fe);
        speechPlayer_terminate(player);
        return 0;
      }
      written += w;
    }

    if (n < static_cast<int>(pcm.size())) break;
  }

  nvspFrontend_destroy(fe);
  speechPlayer_terminate(player);
  return 0;
}