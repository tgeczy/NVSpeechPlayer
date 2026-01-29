#define UNICODE
#define _UNICODE

#include "VoiceProfileEditor.h"
#include "WinUtils.h"
#include "resource.h"

#include <commctrl.h>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <regex>
#include <iomanip>

namespace nvsp_editor {

// =============================================================================
// YAML Parsing helpers
// =============================================================================

static std::string trim(const std::string& s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

static int countIndent(const std::string& line) {
  int n = 0;
  for (char c : line) {
    if (c == ' ') n++;
    else if (c == '\t') n += 2;
    else break;
  }
  return n;
}

static bool parseDouble(const std::string& s, double& out) {
  try {
    size_t pos;
    out = std::stod(s, &pos);
    return pos > 0;
  } catch (...) {
    return false;
  }
}

static std::vector<double> parseDoubleArray(const std::string& s) {
  std::vector<double> result;
  std::string inner = s;
  
  // Remove brackets
  size_t start = inner.find('[');
  size_t end = inner.rfind(']');
  if (start != std::string::npos && end != std::string::npos && end > start) {
    inner = inner.substr(start + 1, end - start - 1);
  }
  
  // Split by comma
  std::stringstream ss(inner);
  std::string token;
  while (std::getline(ss, token, ',')) {
    double v;
    if (parseDouble(trim(token), v)) {
      result.push_back(v);
    }
  }
  return result;
}

static void setScaleField(VPClassScales& scales, const std::string& field, const std::string& value) {
  // Array fields
  if (field == "cf_mul") {
    auto arr = parseDoubleArray(value);
    for (size_t i = 0; i < arr.size() && i < 6; i++) {
      scales.cf_mul[i] = arr[i];
      scales.cf_mul_set[i] = true;
    }
    return;
  }
  if (field == "pf_mul") {
    auto arr = parseDoubleArray(value);
    for (size_t i = 0; i < arr.size() && i < 6; i++) {
      scales.pf_mul[i] = arr[i];
      scales.pf_mul_set[i] = true;
    }
    return;
  }
  if (field == "cb_mul") {
    auto arr = parseDoubleArray(value);
    for (size_t i = 0; i < arr.size() && i < 6; i++) {
      scales.cb_mul[i] = arr[i];
      scales.cb_mul_set[i] = true;
    }
    return;
  }
  if (field == "pb_mul") {
    auto arr = parseDoubleArray(value);
    for (size_t i = 0; i < arr.size() && i < 6; i++) {
      scales.pb_mul[i] = arr[i];
      scales.pb_mul_set[i] = true;
    }
    return;
  }
  if (field == "pa_mul") {
    auto arr = parseDoubleArray(value);
    for (size_t i = 0; i < arr.size() && i < 6; i++) {
      scales.pa_mul[i] = arr[i];
      scales.pa_mul_set[i] = true;
    }
    return;
  }
  
  // Scalar fields
  double v;
  if (!parseDouble(value, v)) return;
  
  if (field == "voicePitch_mul") { scales.voicePitch_mul = v; scales.voicePitch_mul_set = true; }
  else if (field == "endVoicePitch_mul") { scales.endVoicePitch_mul = v; scales.endVoicePitch_mul_set = true; }
  else if (field == "vibratoPitchOffset_mul") { scales.vibratoPitchOffset_mul = v; scales.vibratoPitchOffset_mul_set = true; }
  else if (field == "vibratoSpeed_mul") { scales.vibratoSpeed_mul = v; scales.vibratoSpeed_mul_set = true; }
  else if (field == "voiceTurbulenceAmplitude_mul") { scales.voiceTurbulenceAmplitude_mul = v; scales.voiceTurbulenceAmplitude_mul_set = true; }
  else if (field == "glottalOpenQuotient_mul") { scales.glottalOpenQuotient_mul = v; scales.glottalOpenQuotient_mul_set = true; }
  else if (field == "voiceAmplitude_mul") { scales.voiceAmplitude_mul = v; scales.voiceAmplitude_mul_set = true; }
  else if (field == "aspirationAmplitude_mul") { scales.aspirationAmplitude_mul = v; scales.aspirationAmplitude_mul_set = true; }
  else if (field == "fricationAmplitude_mul") { scales.fricationAmplitude_mul = v; scales.fricationAmplitude_mul_set = true; }
  else if (field == "preFormantGain_mul") { scales.preFormantGain_mul = v; scales.preFormantGain_mul_set = true; }
  else if (field == "outputGain_mul") { scales.outputGain_mul = v; scales.outputGain_mul_set = true; }
}

// Parse inline map like {cf1: 648, cf2: 1856, cf3: 2820}
static std::map<std::string, double> parseInlineMap(const std::string& s) {
  std::map<std::string, double> result;
  std::string inner = s;
  
  size_t start = inner.find('{');
  size_t end = inner.rfind('}');
  if (start != std::string::npos && end != std::string::npos && end > start) {
    inner = inner.substr(start + 1, end - start - 1);
  }
  
  // Split by comma, then by colon
  std::stringstream ss(inner);
  std::string token;
  while (std::getline(ss, token, ',')) {
    size_t colon = token.find(':');
    if (colon != std::string::npos) {
      std::string key = trim(token.substr(0, colon));
      std::string val = trim(token.substr(colon + 1));
      double v;
      if (parseDouble(val, v)) {
        result[key] = v;
      }
    }
  }
  return result;
}

bool loadVoiceProfilesFromYaml(const std::wstring& yamlPath, std::vector<VPVoiceProfile>& outProfiles, std::string& outError) {
  outProfiles.clear();
  outError.clear();
  
  std::ifstream f(yamlPath);
  if (!f.is_open()) {
    outError = "Could not open file";
    return false;
  }
  
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  
  // Find voiceProfiles: section
  int vpStart = -1;
  int vpEnd = -1;
  int vpBaseIndent = -1;
  
  for (size_t i = 0; i < lines.size(); i++) {
    std::string stripped = trim(lines[i]);
    if (stripped.empty() || stripped[0] == '#') continue;
    
    if (lines[i][0] != ' ' && lines[i][0] != '\t') {
      if (stripped.rfind("voiceProfiles:", 0) == 0) {
        vpStart = static_cast<int>(i);
        vpBaseIndent = -1;
      } else if (vpStart >= 0 && vpEnd < 0) {
        vpEnd = static_cast<int>(i);
      }
    }
  }
  if (vpStart >= 0 && vpEnd < 0) vpEnd = static_cast<int>(lines.size());
  
  if (vpStart < 0) {
    // No voiceProfiles section - that's OK, just return empty
    return true;
  }
  
  // Parse profiles
  VPVoiceProfile* currentProfile = nullptr;
  std::string currentClass;
  VPPhonemeOverride* currentOverride = nullptr;
  int profileIndent = -1;
  int classScalesIndent = -1;
  int classIndent = -1;
  int fieldIndent = -1;
  int overridesIndent = -1;
  int overrideIndent = -1;
  bool inClassScales = false;
  bool inPhonemeOverrides = false;
  
  for (int i = vpStart + 1; i < vpEnd; i++) {
    const std::string& ln = lines[i];
    std::string stripped = trim(ln);
    if (stripped.empty() || stripped[0] == '#') continue;
    
    int indent = countIndent(ln);
    
    // Detect base indent for profiles
    if (profileIndent < 0) {
      profileIndent = indent;
    }
    
    // Profile name (at profile indent level, ends with :)
    if (indent == profileIndent) {
      size_t colon = stripped.find(':');
      if (colon != std::string::npos) {
        std::string name = trim(stripped.substr(0, colon));
        if (!name.empty() && name.find('.') == std::string::npos) {
          outProfiles.push_back(VPVoiceProfile{});
          currentProfile = &outProfiles.back();
          currentProfile->name = name;
          currentClass.clear();
          inClassScales = false;
          inPhonemeOverrides = false;
          classScalesIndent = -1;
          overridesIndent = -1;
        }
      }
      continue;
    }
    
    if (!currentProfile) continue;
    
    // Detect classScales: or phonemeOverrides:
    if (indent > profileIndent && classScalesIndent < 0 && overridesIndent < 0) {
      if (stripped == "classScales:") {
        inClassScales = true;
        inPhonemeOverrides = false;
        classScalesIndent = indent;
        classIndent = -1;
        continue;
      }
      if (stripped == "phonemeOverrides:") {
        inPhonemeOverrides = true;
        inClassScales = false;
        overridesIndent = indent;
        overrideIndent = -1;
        continue;
      }
    }
    
    // Inside classScales
    if (inClassScales && indent > classScalesIndent) {
      if (classIndent < 0) classIndent = indent;
      
      // Class name
      if (indent == classIndent) {
        size_t colon = stripped.find(':');
        if (colon != std::string::npos) {
          currentClass = trim(stripped.substr(0, colon));
          std::string val = trim(stripped.substr(colon + 1));
          if (val.empty()) {
            // Nested class definition
            if (currentProfile->classScales.find(currentClass) == currentProfile->classScales.end()) {
              currentProfile->classScales[currentClass] = VPClassScales{};
            }
            fieldIndent = -1;
          }
        }
        continue;
      }
      
      // Field inside class
      if (!currentClass.empty() && indent > classIndent) {
        if (fieldIndent < 0) fieldIndent = indent;
        if (indent == fieldIndent) {
          size_t colon = stripped.find(':');
          if (colon != std::string::npos) {
            std::string field = trim(stripped.substr(0, colon));
            std::string value = trim(stripped.substr(colon + 1));
            if (!field.empty() && !value.empty()) {
              setScaleField(currentProfile->classScales[currentClass], field, value);
            }
          }
        }
      }
      continue;
    }
    
    // Inside phonemeOverrides
    if (inPhonemeOverrides && indent > overridesIndent) {
      if (overrideIndent < 0) overrideIndent = indent;
      
      if (indent == overrideIndent) {
        size_t colon = stripped.find(':');
        if (colon != std::string::npos) {
          std::string phoneme = trim(stripped.substr(0, colon));
          std::string value = trim(stripped.substr(colon + 1));
          
          // Remove quotes from phoneme
          if (phoneme.size() >= 2 && 
              ((phoneme.front() == '"' && phoneme.back() == '"') ||
               (phoneme.front() == '\'' && phoneme.back() == '\''))) {
            phoneme = phoneme.substr(1, phoneme.size() - 2);
          }
          
          VPPhonemeOverride ovr;
          ovr.phoneme = phoneme;
          
          // Check for inline map
          if (!value.empty() && value.front() == '{') {
            ovr.fields = parseInlineMap(value);
          }
          
          currentProfile->phonemeOverrides.push_back(ovr);
          currentOverride = &currentProfile->phonemeOverrides.back();
        }
      }
    }
    
    // Reset section tracking when we go back to profile level
    if (indent <= profileIndent) {
      inClassScales = false;
      inPhonemeOverrides = false;
      classScalesIndent = -1;
      overridesIndent = -1;
    }
  }
  
  return true;
}

// Format a double array as YAML [1.0, 1.1, 1.2]
static std::string formatArray(const std::array<double, 6>& arr, const std::array<bool, 6>& set) {
  // Check if any are set
  bool anySet = false;
  for (bool b : set) if (b) { anySet = true; break; }
  if (!anySet) return "";
  
  std::ostringstream ss;
  ss << "[";
  for (size_t i = 0; i < 6; i++) {
    if (i > 0) ss << ", ";
    ss << arr[i];
  }
  ss << "]";
  return ss.str();
}

static std::string formatDouble(double v) {
  std::ostringstream ss;
  ss << v;
  return ss.str();
}

bool saveVoiceProfilesToYaml(const std::wstring& yamlPath, const std::vector<VPVoiceProfile>& profiles, std::string& outError) {
  outError.clear();
  
  // Read existing file
  std::vector<std::string> lines;
  {
    std::ifstream f(yamlPath);
    if (f.is_open()) {
      std::string line;
      while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
      }
    }
  }
  
  // Find and remove existing voiceProfiles section
  int vpStart = -1;
  int vpEnd = -1;
  
  for (size_t i = 0; i < lines.size(); i++) {
    std::string stripped = trim(lines[i]);
    if (stripped.empty() || stripped[0] == '#') continue;
    
    if (lines[i][0] != ' ' && lines[i][0] != '\t') {
      if (stripped.rfind("voiceProfiles:", 0) == 0) {
        vpStart = static_cast<int>(i);
      } else if (vpStart >= 0 && vpEnd < 0) {
        vpEnd = static_cast<int>(i);
      }
    }
  }
  if (vpStart >= 0 && vpEnd < 0) vpEnd = static_cast<int>(lines.size());
  
  // Remove old section
  if (vpStart >= 0) {
    lines.erase(lines.begin() + vpStart, lines.begin() + vpEnd);
  }
  
  // Build new voiceProfiles section
  std::vector<std::string> vpLines;
  if (!profiles.empty()) {
    vpLines.push_back("voiceProfiles:");
    
    for (const auto& profile : profiles) {
      vpLines.push_back("  " + profile.name + ":");
      
      // Class scales
      if (!profile.classScales.empty()) {
        vpLines.push_back("    classScales:");
        
        for (const auto& [className, scales] : profile.classScales) {
          vpLines.push_back("      " + className + ":");
          
          // Scalar fields
          if (scales.voicePitch_mul_set)
            vpLines.push_back("        voicePitch_mul: " + formatDouble(scales.voicePitch_mul));
          if (scales.endVoicePitch_mul_set)
            vpLines.push_back("        endVoicePitch_mul: " + formatDouble(scales.endVoicePitch_mul));
          if (scales.vibratoPitchOffset_mul_set)
            vpLines.push_back("        vibratoPitchOffset_mul: " + formatDouble(scales.vibratoPitchOffset_mul));
          if (scales.vibratoSpeed_mul_set)
            vpLines.push_back("        vibratoSpeed_mul: " + formatDouble(scales.vibratoSpeed_mul));
          if (scales.voiceTurbulenceAmplitude_mul_set)
            vpLines.push_back("        voiceTurbulenceAmplitude_mul: " + formatDouble(scales.voiceTurbulenceAmplitude_mul));
          if (scales.glottalOpenQuotient_mul_set)
            vpLines.push_back("        glottalOpenQuotient_mul: " + formatDouble(scales.glottalOpenQuotient_mul));
          if (scales.voiceAmplitude_mul_set)
            vpLines.push_back("        voiceAmplitude_mul: " + formatDouble(scales.voiceAmplitude_mul));
          if (scales.aspirationAmplitude_mul_set)
            vpLines.push_back("        aspirationAmplitude_mul: " + formatDouble(scales.aspirationAmplitude_mul));
          if (scales.fricationAmplitude_mul_set)
            vpLines.push_back("        fricationAmplitude_mul: " + formatDouble(scales.fricationAmplitude_mul));
          if (scales.preFormantGain_mul_set)
            vpLines.push_back("        preFormantGain_mul: " + formatDouble(scales.preFormantGain_mul));
          if (scales.outputGain_mul_set)
            vpLines.push_back("        outputGain_mul: " + formatDouble(scales.outputGain_mul));
          
          // Array fields
          std::string arr;
          arr = formatArray(scales.cf_mul, scales.cf_mul_set);
          if (!arr.empty()) vpLines.push_back("        cf_mul: " + arr);
          arr = formatArray(scales.pf_mul, scales.pf_mul_set);
          if (!arr.empty()) vpLines.push_back("        pf_mul: " + arr);
          arr = formatArray(scales.cb_mul, scales.cb_mul_set);
          if (!arr.empty()) vpLines.push_back("        cb_mul: " + arr);
          arr = formatArray(scales.pb_mul, scales.pb_mul_set);
          if (!arr.empty()) vpLines.push_back("        pb_mul: " + arr);
          arr = formatArray(scales.pa_mul, scales.pa_mul_set);
          if (!arr.empty()) vpLines.push_back("        pa_mul: " + arr);
        }
      }
      
      // Phoneme overrides
      if (!profile.phonemeOverrides.empty()) {
        vpLines.push_back("    phonemeOverrides:");
        
        for (const auto& ovr : profile.phonemeOverrides) {
          if (ovr.fields.empty()) continue;
          
          std::ostringstream ss;
          ss << "      " << ovr.phoneme << ": {";
          bool first = true;
          for (const auto& [field, val] : ovr.fields) {
            if (!first) ss << ", ";
            ss << field << ": " << val;
            first = false;
          }
          ss << "}";
          vpLines.push_back(ss.str());
        }
      }
    }
  }
  
  // Insert new section at the end (or where old one was)
  int insertPos = (vpStart >= 0) ? vpStart : static_cast<int>(lines.size());
  lines.insert(lines.begin() + insertPos, vpLines.begin(), vpLines.end());
  
  // Write back
  std::ofstream out(yamlPath);
  if (!out.is_open()) {
    outError = "Could not write file";
    return false;
  }
  
  for (const auto& ln : lines) {
    out << ln << "\n";
  }
  
  return true;
}

// =============================================================================
// Dialog Procedures
// =============================================================================

static void populateProfileList(HWND hList, const std::vector<VPVoiceProfile>& profiles) {
  SendMessageW(hList, LB_RESETCONTENT, 0, 0);
  for (const auto& p : profiles) {
    std::wstring w = utf8ToWide(p.name);
    SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
  }
  if (!profiles.empty()) {
    SendMessageW(hList, LB_SETCURSEL, 0, 0);
  }
}

static INT_PTR CALLBACK VoiceProfilesDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
  auto* st = reinterpret_cast<VoiceProfilesDialogState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
  
  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<VoiceProfilesDialogState*>(lParam);
      SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
      
      HWND hList = GetDlgItem(hDlg, IDC_VP_LIST);
      populateProfileList(hList, st->profiles);
      
      return TRUE;
    }
    
    case WM_COMMAND: {
      if (!st) break;
      int id = LOWORD(wParam);
      int code = HIWORD(wParam);
      
      if (id == IDC_VP_ADD) {
        EditVoiceProfileDialogState eps;
        eps.profile.name = "NewVoice";
        if (ShowEditVoiceProfileDialog(GetModuleHandleW(nullptr), hDlg, eps) && eps.ok) {
          st->profiles.push_back(eps.profile);
          st->modified = true;
          populateProfileList(GetDlgItem(hDlg, IDC_VP_LIST), st->profiles);
          SendMessageW(GetDlgItem(hDlg, IDC_VP_LIST), LB_SETCURSEL, st->profiles.size() - 1, 0);
        }
        return TRUE;
      }
      
      if (id == IDC_VP_EDIT || (id == IDC_VP_LIST && code == LBN_DBLCLK)) {
        HWND hList = GetDlgItem(hDlg, IDC_VP_LIST);
        int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < static_cast<int>(st->profiles.size())) {
          EditVoiceProfileDialogState eps;
          eps.profile = st->profiles[sel];
          if (ShowEditVoiceProfileDialog(GetModuleHandleW(nullptr), hDlg, eps) && eps.ok) {
            st->profiles[sel] = eps.profile;
            st->modified = true;
            populateProfileList(hList, st->profiles);
            SendMessageW(hList, LB_SETCURSEL, sel, 0);
          }
        }
        return TRUE;
      }
      
      if (id == IDC_VP_DELETE) {
        HWND hList = GetDlgItem(hDlg, IDC_VP_LIST);
        int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < static_cast<int>(st->profiles.size())) {
          std::wstring msg = L"Delete voice profile \"" + utf8ToWide(st->profiles[sel].name) + L"\"?";
          if (MessageBoxW(hDlg, msg.c_str(), L"Confirm Delete", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            st->profiles.erase(st->profiles.begin() + sel);
            st->modified = true;
            populateProfileList(hList, st->profiles);
          }
        }
        return TRUE;
      }
      
      if (id == IDC_VP_DUPLICATE) {
        HWND hList = GetDlgItem(hDlg, IDC_VP_LIST);
        int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < static_cast<int>(st->profiles.size())) {
          VPVoiceProfile dup = st->profiles[sel];
          dup.name += "_copy";
          st->profiles.push_back(dup);
          st->modified = true;
          populateProfileList(hList, st->profiles);
          SendMessageW(hList, LB_SETCURSEL, st->profiles.size() - 1, 0);
        }
        return TRUE;
      }
      
      if (id == IDOK) {
        st->ok = true;
        EndDialog(hDlg, IDOK);
        return TRUE;
      }
      
      if (id == IDCANCEL) {
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
      }
      break;
    }
  }
  return FALSE;
}

// =============================================================================
// Edit Voice Profile Dialog
// =============================================================================

static void populateClassCombo(HWND hCombo, const std::string& selected) {
  SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
  int sel = 0;
  for (int i = 0; i < kVoiceProfileClassCount; i++) {
    std::wstring w = utf8ToWide(kVoiceProfileClasses[i]);
    int idx = static_cast<int>(SendMessageW(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str())));
    if (kVoiceProfileClasses[i] == selected) sel = idx;
  }
  SendMessageW(hCombo, CB_SETCURSEL, sel, 0);
}

static void populateScalesList(HWND hList, const VPClassScales& scales) {
  SendMessageW(hList, LB_RESETCONTENT, 0, 0);
  
  auto addScalar = [&](const char* name, double val, bool set) {
    if (set) {
      std::wstring w = utf8ToWide(name) + L": " + utf8ToWide(formatDouble(val));
      SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
    }
  };
  
  addScalar("voicePitch_mul", scales.voicePitch_mul, scales.voicePitch_mul_set);
  addScalar("endVoicePitch_mul", scales.endVoicePitch_mul, scales.endVoicePitch_mul_set);
  addScalar("vibratoPitchOffset_mul", scales.vibratoPitchOffset_mul, scales.vibratoPitchOffset_mul_set);
  addScalar("vibratoSpeed_mul", scales.vibratoSpeed_mul, scales.vibratoSpeed_mul_set);
  addScalar("voiceTurbulenceAmplitude_mul", scales.voiceTurbulenceAmplitude_mul, scales.voiceTurbulenceAmplitude_mul_set);
  addScalar("glottalOpenQuotient_mul", scales.glottalOpenQuotient_mul, scales.glottalOpenQuotient_mul_set);
  addScalar("voiceAmplitude_mul", scales.voiceAmplitude_mul, scales.voiceAmplitude_mul_set);
  addScalar("aspirationAmplitude_mul", scales.aspirationAmplitude_mul, scales.aspirationAmplitude_mul_set);
  addScalar("fricationAmplitude_mul", scales.fricationAmplitude_mul, scales.fricationAmplitude_mul_set);
  addScalar("preFormantGain_mul", scales.preFormantGain_mul, scales.preFormantGain_mul_set);
  addScalar("outputGain_mul", scales.outputGain_mul, scales.outputGain_mul_set);
  
  // Arrays
  auto addArray = [&](const char* name, const std::array<double, 6>& arr, const std::array<bool, 6>& set) {
    std::string s = formatArray(arr, set);
    if (!s.empty()) {
      std::wstring w = utf8ToWide(name) + L": " + utf8ToWide(s);
      SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
    }
  };
  
  addArray("cf_mul", scales.cf_mul, scales.cf_mul_set);
  addArray("pf_mul", scales.pf_mul, scales.pf_mul_set);
  addArray("cb_mul", scales.cb_mul, scales.cb_mul_set);
  addArray("pb_mul", scales.pb_mul, scales.pb_mul_set);
  addArray("pa_mul", scales.pa_mul, scales.pa_mul_set);
}

static void populateFieldCombo(HWND hCombo) {
  SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
  for (int i = 0; i < kScaleFieldCount; i++) {
    std::wstring w = utf8ToWide(kScaleFieldNames[i]);
    SendMessageW(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
  }
  SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
}

static void populateOverridesList(HWND hList, const std::vector<VPPhonemeOverride>& overrides) {
  SendMessageW(hList, LB_RESETCONTENT, 0, 0);
  for (const auto& ovr : overrides) {
    std::wstring w = utf8ToWide(ovr.phoneme) + L" (" + std::to_wstring(ovr.fields.size()) + L" fields)";
    SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
  }
}

static INT_PTR CALLBACK EditVoiceProfileDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
  auto* st = reinterpret_cast<EditVoiceProfileDialogState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
  
  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<EditVoiceProfileDialogState*>(lParam);
      SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
      
      // Profile name
      SetDlgItemTextW(hDlg, IDC_EVP_NAME, utf8ToWide(st->profile.name).c_str());
      
      // Class combo
      HWND hClassCombo = GetDlgItem(hDlg, IDC_EVP_CLASS_COMBO);
      populateClassCombo(hClassCombo, st->currentClass.empty() ? "vowel" : st->currentClass);
      if (st->currentClass.empty()) st->currentClass = "vowel";
      
      // Field combo
      populateFieldCombo(GetDlgItem(hDlg, IDC_EVP_SCALE_FIELD));
      
      // Scales list for current class
      auto it = st->profile.classScales.find(st->currentClass);
      if (it != st->profile.classScales.end()) {
        populateScalesList(GetDlgItem(hDlg, IDC_EVP_SCALES_LIST), it->second);
      }
      
      // Overrides list
      populateOverridesList(GetDlgItem(hDlg, IDC_EVP_OVERRIDES_LIST), st->profile.phonemeOverrides);
      
      return TRUE;
    }
    
    case WM_COMMAND: {
      if (!st) break;
      int id = LOWORD(wParam);
      int code = HIWORD(wParam);
      
      // Class combo changed
      if (id == IDC_EVP_CLASS_COMBO && code == CBN_SELCHANGE) {
        HWND hCombo = GetDlgItem(hDlg, IDC_EVP_CLASS_COMBO);
        int sel = static_cast<int>(SendMessageW(hCombo, CB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < kVoiceProfileClassCount) {
          st->currentClass = kVoiceProfileClasses[sel];
          auto it = st->profile.classScales.find(st->currentClass);
          if (it != st->profile.classScales.end()) {
            populateScalesList(GetDlgItem(hDlg, IDC_EVP_SCALES_LIST), it->second);
          } else {
            SendMessageW(GetDlgItem(hDlg, IDC_EVP_SCALES_LIST), LB_RESETCONTENT, 0, 0);
          }
        }
        return TRUE;
      }
      
      // Remove class
      if (id == IDC_EVP_CLASS_REMOVE) {
        if (!st->currentClass.empty()) {
          auto it = st->profile.classScales.find(st->currentClass);
          if (it != st->profile.classScales.end()) {
            st->profile.classScales.erase(it);
            SendMessageW(GetDlgItem(hDlg, IDC_EVP_SCALES_LIST), LB_RESETCONTENT, 0, 0);
            std::wstring msg = L"All fields from class \"" + utf8ToWide(st->currentClass) + L"\" removed.";
            msgBox(hDlg, msg.c_str(), L"Class Removed", MB_OK | MB_ICONINFORMATION);
          } else {
            msgBox(hDlg, L"This class has no fields to remove.", L"Class Empty", MB_OK | MB_ICONINFORMATION);
          }
        }
        return TRUE;
      }
      
      // Set scale value (auto-creates class if needed)
      if (id == IDC_EVP_SCALE_SET) {
        if (st->currentClass.empty()) {
          msgBox(hDlg, L"Select a class first.", L"Voice Profile", MB_ICONINFORMATION);
          return TRUE;
        }
        
        // Get field name
        HWND hFieldCombo = GetDlgItem(hDlg, IDC_EVP_SCALE_FIELD);
        int fieldSel = static_cast<int>(SendMessageW(hFieldCombo, CB_GETCURSEL, 0, 0));
        if (fieldSel < 0 || fieldSel >= kScaleFieldCount) return TRUE;
        std::string fieldName = kScaleFieldNames[fieldSel];
        
        // Get value
        wchar_t buf[256];
        GetDlgItemTextW(hDlg, IDC_EVP_SCALE_VALUE, buf, 256);
        std::string valueStr = wideToUtf8(buf);
        
        // Ensure class exists
        if (st->profile.classScales.find(st->currentClass) == st->profile.classScales.end()) {
          st->profile.classScales[st->currentClass] = VPClassScales{};
        }
        
        setScaleField(st->profile.classScales[st->currentClass], fieldName, valueStr);
        populateScalesList(GetDlgItem(hDlg, IDC_EVP_SCALES_LIST), st->profile.classScales[st->currentClass]);
        return TRUE;
      }
      
      // Add phoneme override
      if (id == IDC_EVP_OVERRIDE_ADD) {
        EditPhonemeOverrideDialogState ops;
        if (ShowEditPhonemeOverrideDialog(GetModuleHandleW(nullptr), hDlg, ops) && ops.ok) {
          st->profile.phonemeOverrides.push_back(ops.override);
          populateOverridesList(GetDlgItem(hDlg, IDC_EVP_OVERRIDES_LIST), st->profile.phonemeOverrides);
        }
        return TRUE;
      }
      
      // Edit phoneme override
      if (id == IDC_EVP_OVERRIDE_EDIT || (id == IDC_EVP_OVERRIDES_LIST && code == LBN_DBLCLK)) {
        HWND hList = GetDlgItem(hDlg, IDC_EVP_OVERRIDES_LIST);
        int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < static_cast<int>(st->profile.phonemeOverrides.size())) {
          EditPhonemeOverrideDialogState ops;
          ops.override = st->profile.phonemeOverrides[sel];
          if (ShowEditPhonemeOverrideDialog(GetModuleHandleW(nullptr), hDlg, ops) && ops.ok) {
            st->profile.phonemeOverrides[sel] = ops.override;
            populateOverridesList(hList, st->profile.phonemeOverrides);
          }
        }
        return TRUE;
      }
      
      // Remove phoneme override
      if (id == IDC_EVP_OVERRIDE_REMOVE) {
        HWND hList = GetDlgItem(hDlg, IDC_EVP_OVERRIDES_LIST);
        int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < static_cast<int>(st->profile.phonemeOverrides.size())) {
          st->profile.phonemeOverrides.erase(st->profile.phonemeOverrides.begin() + sel);
          populateOverridesList(hList, st->profile.phonemeOverrides);
        }
        return TRUE;
      }
      
      if (id == IDOK) {
        // Get profile name
        wchar_t buf[256];
        GetDlgItemTextW(hDlg, IDC_EVP_NAME, buf, 256);
        st->profile.name = wideToUtf8(buf);
        
        if (st->profile.name.empty()) {
          msgBox(hDlg, L"Profile name is required.", L"Voice Profile", MB_ICONERROR);
          return TRUE;
        }
        
        st->ok = true;
        EndDialog(hDlg, IDOK);
        return TRUE;
      }
      
      if (id == IDCANCEL) {
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
      }
      break;
    }
  }
  return FALSE;
}

// =============================================================================
// Edit Phoneme Override Dialog
// =============================================================================

static void populateOverrideFieldCombo(HWND hCombo) {
  SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
  for (int i = 0; i < kOverrideFieldCount; i++) {
    std::wstring w = utf8ToWide(kOverrideFieldNames[i]);
    SendMessageW(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
  }
  SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
}

static void populateOverrideFieldsList(HWND hList, const std::map<std::string, double>& fields) {
  SendMessageW(hList, LB_RESETCONTENT, 0, 0);
  for (const auto& [name, val] : fields) {
    std::wstring w = utf8ToWide(name) + L": " + utf8ToWide(formatDouble(val));
    SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
  }
}

static INT_PTR CALLBACK EditPhonemeOverrideDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
  auto* st = reinterpret_cast<EditPhonemeOverrideDialogState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
  
  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<EditPhonemeOverrideDialogState*>(lParam);
      SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
      
      SetDlgItemTextW(hDlg, IDC_EPO_PHONEME, utf8ToWide(st->override.phoneme).c_str());
      populateOverrideFieldCombo(GetDlgItem(hDlg, IDC_EPO_FIELD_COMBO));
      populateOverrideFieldsList(GetDlgItem(hDlg, IDC_EPO_FIELDS_LIST), st->override.fields);
      
      return TRUE;
    }
    
    case WM_COMMAND: {
      if (!st) break;
      int id = LOWORD(wParam);
      
      // Set field
      if (id == IDC_EPO_FIELD_SET) {
        HWND hCombo = GetDlgItem(hDlg, IDC_EPO_FIELD_COMBO);
        int sel = static_cast<int>(SendMessageW(hCombo, CB_GETCURSEL, 0, 0));
        if (sel < 0 || sel >= kOverrideFieldCount) return TRUE;
        
        wchar_t buf[256];
        GetDlgItemTextW(hDlg, IDC_EPO_FIELD_VALUE, buf, 256);
        double val;
        if (parseDouble(wideToUtf8(buf), val)) {
          st->override.fields[kOverrideFieldNames[sel]] = val;
          populateOverrideFieldsList(GetDlgItem(hDlg, IDC_EPO_FIELDS_LIST), st->override.fields);
        } else {
          msgBox(hDlg, L"Invalid number.", L"Override Field", MB_ICONERROR);
        }
        return TRUE;
      }
      
      // Remove field
      if (id == IDC_EPO_FIELD_REMOVE) {
        HWND hList = GetDlgItem(hDlg, IDC_EPO_FIELDS_LIST);
        int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
        if (sel >= 0) {
          // Find which field this is
          int idx = 0;
          for (auto it = st->override.fields.begin(); it != st->override.fields.end(); ++it, ++idx) {
            if (idx == sel) {
              st->override.fields.erase(it);
              break;
            }
          }
          populateOverrideFieldsList(hList, st->override.fields);
        }
        return TRUE;
      }
      
      if (id == IDOK) {
        wchar_t buf[256];
        GetDlgItemTextW(hDlg, IDC_EPO_PHONEME, buf, 256);
        st->override.phoneme = wideToUtf8(buf);
        
        if (st->override.phoneme.empty()) {
          msgBox(hDlg, L"Phoneme is required.", L"Override", MB_ICONERROR);
          return TRUE;
        }
        
        st->ok = true;
        EndDialog(hDlg, IDOK);
        return TRUE;
      }
      
      if (id == IDCANCEL) {
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
      }
      break;
    }
  }
  return FALSE;
}

// =============================================================================
// Public entry points
// =============================================================================

bool ShowVoiceProfilesDialog(HINSTANCE hInst, HWND parent, VoiceProfilesDialogState& st) {
  st.ok = false;
  DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_VOICE_PROFILES), parent, VoiceProfilesDlgProc, reinterpret_cast<LPARAM>(&st));
  return st.ok;
}

bool ShowEditVoiceProfileDialog(HINSTANCE hInst, HWND parent, EditVoiceProfileDialogState& st) {
  st.ok = false;
  DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_EDIT_VOICE_PROFILE), parent, EditVoiceProfileDlgProc, reinterpret_cast<LPARAM>(&st));
  return st.ok;
}

bool ShowEditPhonemeOverrideDialog(HINSTANCE hInst, HWND parent, EditPhonemeOverrideDialogState& st) {
  st.ok = false;
  DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_EDIT_PHONEME_OVERRIDE), parent, EditPhonemeOverrideDlgProc, reinterpret_cast<LPARAM>(&st));
  return st.ok;
}

} // namespace nvsp_editor