#pragma once

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <array>

namespace nvsp_editor {

// Forward declaration
class NvspRuntime;

// Multiplier field types for class scales
struct VPClassScales {
  // Formant frequency multipliers (cf1-cf6, pf1-pf6)
  std::array<double, 6> cf_mul = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
  std::array<double, 6> pf_mul = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
  
  // Formant bandwidth multipliers (cb1-cb6, pb1-pb6)
  std::array<double, 6> cb_mul = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
  std::array<double, 6> pb_mul = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
  
  // Parallel amplitude multipliers (pa1-pa6)
  std::array<double, 6> pa_mul = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
  
  // Scalar multipliers (with _set flags to track if explicitly set)
  double voicePitch_mul = 1.0;           bool voicePitch_mul_set = false;
  double endVoicePitch_mul = 1.0;        bool endVoicePitch_mul_set = false;
  double vibratoPitchOffset_mul = 1.0;   bool vibratoPitchOffset_mul_set = false;
  double vibratoSpeed_mul = 1.0;         bool vibratoSpeed_mul_set = false;
  double voiceTurbulenceAmplitude_mul = 1.0; bool voiceTurbulenceAmplitude_mul_set = false;
  double glottalOpenQuotient_mul = 1.0;  bool glottalOpenQuotient_mul_set = false;
  double voiceAmplitude_mul = 1.0;       bool voiceAmplitude_mul_set = false;
  double aspirationAmplitude_mul = 1.0;  bool aspirationAmplitude_mul_set = false;
  double fricationAmplitude_mul = 1.0;   bool fricationAmplitude_mul_set = false;
  double preFormantGain_mul = 1.0;       bool preFormantGain_mul_set = false;
  double outputGain_mul = 1.0;           bool outputGain_mul_set = false;
  
  // Track which array elements are explicitly set (vs default)
  std::array<bool, 6> cf_mul_set = {false, false, false, false, false, false};
  std::array<bool, 6> pf_mul_set = {false, false, false, false, false, false};
  std::array<bool, 6> cb_mul_set = {false, false, false, false, false, false};
  std::array<bool, 6> pb_mul_set = {false, false, false, false, false, false};
  std::array<bool, 6> pa_mul_set = {false, false, false, false, false, false};
};

// Phoneme override: map field name -> absolute value
struct VPPhonemeOverride {
  std::string phoneme;
  std::map<std::string, double> fields;  // field name -> value
};

// A complete voice profile
struct VPVoiceProfile {
  std::string name;
  std::map<std::string, VPClassScales> classScales;  // class name -> scales
  std::vector<VPPhonemeOverride> phonemeOverrides;
};

// Dialog state for voice profile list
struct VoiceProfilesDialogState {
  std::vector<VPVoiceProfile> profiles;
  std::wstring phonemesYamlPath;
  bool modified = false;
  bool ok = false;
};

// Dialog state for editing a single profile
struct EditVoiceProfileDialogState {
  VPVoiceProfile profile;
  std::string currentClass;  // Currently selected class in combo
  bool ok = false;
};

// Dialog state for editing phoneme override
struct EditPhonemeOverrideDialogState {
  VPPhonemeOverride override;
  bool ok = false;
};

// Available class names for the combo box
const char* const kVoiceProfileClasses[] = {
  "vowel",
  "consonant",
  "voicedConsonant",
  "voicedFricative",
  "unvoicedFricative",
  "nasal",
  "liquid",
  "semivowel",
  "stop",
  "affricate"
};
constexpr int kVoiceProfileClassCount = sizeof(kVoiceProfileClasses) / sizeof(kVoiceProfileClasses[0]);

// Available multiplier field names
const char* const kScaleFieldNames[] = {
  "voicePitch_mul",
  "endVoicePitch_mul",
  "vibratoPitchOffset_mul",
  "vibratoSpeed_mul",
  "voiceTurbulenceAmplitude_mul",
  "glottalOpenQuotient_mul",
  "voiceAmplitude_mul",
  "aspirationAmplitude_mul",
  "fricationAmplitude_mul",
  "preFormantGain_mul",
  "outputGain_mul",
  "cf_mul",
  "pf_mul",
  "cb_mul",
  "pb_mul",
  "pa_mul"
};
constexpr int kScaleFieldCount = sizeof(kScaleFieldNames) / sizeof(kScaleFieldNames[0]);

// Available phoneme override field names (absolute values, not multipliers)
const char* const kOverrideFieldNames[] = {
  "cf1", "cf2", "cf3", "cf4", "cf5", "cf6",
  "pf1", "pf2", "pf3", "pf4", "pf5", "pf6",
  "cb1", "cb2", "cb3", "cb4", "cb5", "cb6",
  "pb1", "pb2", "pb3", "pb4", "pb5", "pb6",
  "pa1", "pa2", "pa3", "pa4", "pa5", "pa6",
  "voicePitch", "endVoicePitch",
  "voiceAmplitude", "aspirationAmplitude", "fricationAmplitude",
  "voiceTurbulenceAmplitude", "glottalOpenQuotient",
  "vibratoPitchOffset", "vibratoSpeed",
  "preFormantGain", "outputGain", "parallelBypass"
};
constexpr int kOverrideFieldCount = sizeof(kOverrideFieldNames) / sizeof(kOverrideFieldNames[0]);

// Load voice profiles from phonemes.yaml
bool loadVoiceProfilesFromYaml(const std::wstring& yamlPath, std::vector<VPVoiceProfile>& outProfiles, std::string& outError);

// Save voice profiles back to phonemes.yaml (preserves other content)
bool saveVoiceProfilesToYaml(const std::wstring& yamlPath, const std::vector<VPVoiceProfile>& profiles, std::string& outError);

// Dialog entry points
bool ShowVoiceProfilesDialog(HINSTANCE hInst, HWND parent, VoiceProfilesDialogState& st);
bool ShowEditVoiceProfileDialog(HINSTANCE hInst, HWND parent, EditVoiceProfileDialogState& st);
bool ShowEditPhonemeOverrideDialog(HINSTANCE hInst, HWND parent, EditPhonemeOverrideDialogState& st);

} // namespace nvsp_editor
