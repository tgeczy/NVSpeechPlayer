/*
TGSpeechBox — Phoneme editor dialog implementations.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#define UNICODE
#define _UNICODE

#include "Dialogs.h"

#include "AccessibilityUtils.h"
#include "WinUtils.h"

#include "resource.h"
#include "wav_writer.h"

#include <commctrl.h>
#include <mmsystem.h>

#include <algorithm>
#include <cwchar>
#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

static constexpr int kSampleRate = 22050;

using tgsb_editor::TgsbRuntime;

static std::wstring paramHintW(const std::string& key) {
  // Short hints. These are not meant to be textbook-perfect, just a useful nudge while tuning.
  if (key == "cf1" || key == "pf1") return L"F1 freq (mouth openness)";
  if (key == "cf2" || key == "pf2") return L"F2 freq (tongue front/back)";
  if (key == "cf3" || key == "pf3") return L"F3 freq (r-color/brightness)";
  if (key == "cf4" || key == "pf4") return L"high formant (brightness)";
  if (key == "cf5" || key == "pf5") return L"high formant (brightness)";
  if (key == "cf6" || key == "pf6") return L"high formant (brightness)";

  if (key == "cb1" || key == "pb1") return L"F1 width (boxiness)";
  if (key == "cb2" || key == "pb2") return L"F2 width (boxiness)";
  if (key == "cb3" || key == "pb3") return L"F3 width (buzz/edge)";
  if (key == "cb4" || key == "pb4") return L"high width (brightness)";
  if (key == "cb5" || key == "pb5") return L"high width";
  if (key == "cb6" || key == "pb6") return L"high width";

  if (key == "caNP") return L"nasal coupling";
  if (key == "cfN0") return L"nasal resonance";
  if (key == "cfNP") return L"nasal pole";
  if (key == "cbN0") return L"nasal width";
  if (key == "cbNP") return L"nasal pole width";

  if (key == "pa1" || key == "pa2" || key == "pa3" || key == "pa4" || key == "pa5" || key == "pa6") return L"noise band level";
  if (key == "parallelBypass") return L"noise bypass mix";

  if (key == "voicePitch") return L"pitch";
  if (key == "endVoicePitch") return L"pitch end";
  if (key == "voiceAmplitude") return L"voiced loudness";
  if (key == "aspirationAmplitude") return L"breath noise";
  if (key == "fricationAmplitude") return L"hiss/noise";
  if (key == "voiceTurbulenceAmplitude") return L"roughness";
  if (key == "vibratoPitchOffset") return L"vibrato depth";
  if (key == "vibratoSpeed") return L"vibrato speed";
  if (key == "glottalOpenQuotient") return L"breathiness";

  if (key == "preFormantGain") return L"preamp gain";
  if (key == "outputGain") return L"overall gain";

  if (key == "_isVowel") return L"vowel timing";
  if (key == "_isVoiced") return L"voicing";
  if (key == "_isStop") return L"stop timing";
  if (key == "_isNasal") return L"nasal timing";
  if (key == "_isLiquid") return L"liquid timing";
  if (key == "_isSemivowel") return L"glide timing";
  if (key == "_isTap") return L"tap timing";
  if (key == "_isTrill") return L"trill timing";
  if (key == "_isAfricate") return L"affricate timing";
  if (key == "_copyAdjacent") return L"copy adjacent formants";

  // Micro-event fields
  if (key == "burstDurationMs") return L"burst length (ms)";
  if (key == "burstDecayRate") return L"burst decay (0\u20131)";
  if (key == "burstSpectralTilt") return L"burst tilt (neg=bright)";
  if (key == "voiceBarAmplitude") return L"closure voicing level";
  if (key == "voiceBarF1") return L"closure F1 (Hz)";
  if (key == "releaseSpreadMs") return L"aspiration ramp-in (ms)";
  if (key == "fricAttackMs") return L"frication onset (ms)";
  if (key == "fricDecayMs") return L"frication offset (ms)";
  if (key == "durationScale") return L"duration multiplier";
  if (key == "closureGapMs") return L"closure gap (ms, stops)";
  if (key == "frameEx.fricationTiltDb") return L"parallel tilt (dB, neg=darker)";

  return L"";
}

static std::wstring formatFieldLabelW(const std::string& key) {
  std::wstring w = utf8ToWide(key);
  std::wstring hint = paramHintW(key);
  if (!hint.empty()) {
    w += L" (";
    w += hint;
    w += L")";
  }
  return w;
}

static std::string extractFieldKeyFromLabel(const std::string& labelUtf8) {
  // Field labels may look like: "cf1 (F1 freq ...)".
  // We only want the raw key ("cf1").
  auto pos = labelUtf8.find(" (");
  if (pos == std::string::npos) return labelUtf8;
  return labelUtf8.substr(0, pos);
}

static std::wstring formatSpeechParamRowW(const std::string& name, int value) {
  std::wstring w = utf8ToWide(name);
  std::wstring hint = paramHintW(name);
  if (!hint.empty()) {
    w += L" (";
    w += hint;
    w += L")";
  }
  w += L": ";
  w += std::to_wstring(value);
  return w;
}

static void comboAddNone(HWND hCombo) {
  SendMessageW(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(none)"));
  SendMessageW(hCombo, CB_SETITEMDATA, 0, 0);
}

static void comboFillKnownKeys(HWND hCombo, const std::vector<std::string>& keys) {
  if (!hCombo) return;
  SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);

  // De-dup + sort to keep the drop-down predictable.
  std::vector<std::string> sorted = keys;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

  for (const auto& k : sorted) {
    std::wstring w = utf8ToWide(k);
    SendMessageW(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
  }
}

static INT_PTR CALLBACK AddMappingDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
  AddMappingDialogState* st = reinterpret_cast<AddMappingDialogState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));

  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<AddMappingDialogState*>(lParam);
      SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

      SetDlgItemTextW(hDlg, IDC_MAP_FROM, utf8ToWide(st->rule.from).c_str());
      SetDlgItemTextW(hDlg, IDC_MAP_TO, utf8ToWide(st->rule.to).c_str());

      CheckDlgButton(hDlg, IDC_MAP_WORDSTART, st->rule.when.atWordStart ? BST_CHECKED : BST_UNCHECKED);
      CheckDlgButton(hDlg, IDC_MAP_WORDEND, st->rule.when.atWordEnd ? BST_CHECKED : BST_UNCHECKED);

      HWND before = GetDlgItem(hDlg, IDC_MAP_BEFORECLASS);
      HWND after = GetDlgItem(hDlg, IDC_MAP_AFTERCLASS);
      HWND notBefore = GetDlgItem(hDlg, IDC_MAP_NOTBEFORECLASS);
      HWND notAfter = GetDlgItem(hDlg, IDC_MAP_NOTAFTERCLASS);

      comboAddNone(before);
      comboAddNone(after);
      comboAddNone(notBefore);
      comboAddNone(notAfter);

      int idxBefore = 0;
      int idxAfter = 0;
      int idxNotBefore = 0;
      int idxNotAfter = 0;

      for (size_t i = 0; i < st->classNames.size(); ++i) {
        std::wstring w = utf8ToWide(st->classNames[i]);
        int posB = static_cast<int>(SendMessageW(before, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str())));
        int posA = static_cast<int>(SendMessageW(after, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str())));
        int posNB = static_cast<int>(SendMessageW(notBefore, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str())));
        int posNA = static_cast<int>(SendMessageW(notAfter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str())));
        if (!st->rule.when.beforeClass.empty() && st->classNames[i] == st->rule.when.beforeClass) idxBefore = posB;
        if (!st->rule.when.afterClass.empty() && st->classNames[i] == st->rule.when.afterClass) idxAfter = posA;
        if (!st->rule.when.notBeforeClass.empty() && st->classNames[i] == st->rule.when.notBeforeClass) idxNotBefore = posNB;
        if (!st->rule.when.notAfterClass.empty() && st->classNames[i] == st->rule.when.notAfterClass) idxNotAfter = posNA;
      }

      SendMessageW(before, CB_SETCURSEL, idxBefore, 0);
      SendMessageW(after, CB_SETCURSEL, idxAfter, 0);
      SendMessageW(notBefore, CB_SETCURSEL, idxNotBefore, 0);
      SendMessageW(notAfter, CB_SETCURSEL, idxNotAfter, 0);

      return TRUE;
    }

    case WM_COMMAND: {
      if (LOWORD(wParam) == IDC_MAP_EDIT_CLASSES && st && st->language) {
        // Open the class editor dialog
        ClassEditorDialogState ceSt;
        ceSt.classes = st->language->classes();

        HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hDlg, GWLP_HINSTANCE));
        if (ShowClassEditorDialog(hInst, hDlg, ceSt)) {
          // Write back to YAML and refresh comboboxes
          st->language->setClasses(ceSt.classes);
          st->classNames = st->language->classNamesSorted();

          // Refresh all class comboboxes
          auto refreshCombo = [&](int id, const std::string& selected) {
            HWND h = GetDlgItem(hDlg, id);
            SendMessageW(h, CB_RESETCONTENT, 0, 0);
            comboAddNone(h);
            int selIdx = 0;
            for (size_t i = 0; i < st->classNames.size(); ++i) {
              std::wstring w = utf8ToWide(st->classNames[i]);
              int pos = static_cast<int>(SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str())));
              if (!selected.empty() && st->classNames[i] == selected) selIdx = pos;
            }
            SendMessageW(h, CB_SETCURSEL, selIdx, 0);
          };

          // Preserve current selections where possible
          auto getComboSel = [&](int id) -> std::string {
            HWND h = GetDlgItem(hDlg, id);
            int sel = static_cast<int>(SendMessageW(h, CB_GETCURSEL, 0, 0));
            if (sel <= 0) return "";
            wchar_t item[512];
            SendMessageW(h, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(item));
            return wideToUtf8(item);
          };

          std::string selBefore = getComboSel(IDC_MAP_BEFORECLASS);
          std::string selAfter = getComboSel(IDC_MAP_AFTERCLASS);
          std::string selNotBefore = getComboSel(IDC_MAP_NOTBEFORECLASS);
          std::string selNotAfter = getComboSel(IDC_MAP_NOTAFTERCLASS);

          refreshCombo(IDC_MAP_BEFORECLASS, selBefore);
          refreshCombo(IDC_MAP_AFTERCLASS, selAfter);
          refreshCombo(IDC_MAP_NOTBEFORECLASS, selNotBefore);
          refreshCombo(IDC_MAP_NOTAFTERCLASS, selNotAfter);
        }
        return TRUE;
      }

      if (LOWORD(wParam) == IDOK && st) {
        wchar_t buf[1024];
        GetDlgItemTextW(hDlg, IDC_MAP_FROM, buf, 1024);
        st->rule.from = wideToUtf8(buf);
        GetDlgItemTextW(hDlg, IDC_MAP_TO, buf, 1024);
        st->rule.to = wideToUtf8(buf);

        st->rule.when.atWordStart = (IsDlgButtonChecked(hDlg, IDC_MAP_WORDSTART) == BST_CHECKED);
        st->rule.when.atWordEnd = (IsDlgButtonChecked(hDlg, IDC_MAP_WORDEND) == BST_CHECKED);

        auto readCombo = [&](int id, std::string& out) {
          HWND h = GetDlgItem(hDlg, id);
          int sel = static_cast<int>(SendMessageW(h, CB_GETCURSEL, 0, 0));
          if (sel <= 0) { out.clear(); return; }
          wchar_t item[512];
          SendMessageW(h, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(item));
          out = wideToUtf8(item);
        };

        readCombo(IDC_MAP_BEFORECLASS, st->rule.when.beforeClass);
        readCombo(IDC_MAP_AFTERCLASS, st->rule.when.afterClass);
        readCombo(IDC_MAP_NOTBEFORECLASS, st->rule.when.notBeforeClass);
        readCombo(IDC_MAP_NOTAFTERCLASS, st->rule.when.notAfterClass);

        if (st->rule.from.empty() || st->rule.to.empty()) {
          msgBox(hDlg, L"Both 'From' and 'To' are required.", L"Add mapping", MB_ICONERROR);
          return TRUE;
        }

        st->ok = true;
        EndDialog(hDlg, IDOK);
        return TRUE;
      }

      if (LOWORD(wParam) == IDCANCEL) {
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
      }
      break;
    }

  }

  return FALSE;
}


static INT_PTR CALLBACK ClonePhonemeDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
  ClonePhonemeDialogState* st = reinterpret_cast<ClonePhonemeDialogState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));

  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<ClonePhonemeDialogState*>(lParam);
      SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

      HWND combo = GetDlgItem(hDlg, IDC_CLONE_FROM);
      int selIndex = 0;
      for (size_t i = 0; i < st->keys.size(); ++i) {
        std::wstring w = utf8ToWide(st->keys[i]);
        int pos = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str())));
        if (!st->fromKey.empty() && st->keys[i] == st->fromKey) selIndex = pos;
      }
      SendMessageW(combo, CB_SETCURSEL, selIndex, 0);
      SetDlgItemTextW(hDlg, IDC_CLONE_NEWKEY, L"");
      return TRUE;
    }

    case WM_COMMAND: {
      if (LOWORD(wParam) == IDOK && st) {
        wchar_t buf[512];
        GetDlgItemTextW(hDlg, IDC_CLONE_NEWKEY, buf, 512);
        st->newKey = wideToUtf8(buf);

        HWND combo = GetDlgItem(hDlg, IDC_CLONE_FROM);
        int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
        if (sel < 0 || sel >= static_cast<int>(st->keys.size())) {
          msgBox(hDlg, L"Choose a source phoneme.", L"Clone phoneme", MB_ICONERROR);
          return TRUE;
        }
        st->fromKey = st->keys[static_cast<size_t>(sel)];

        if (st->newKey.empty()) {
          msgBox(hDlg, L"New phoneme key is required.", L"Clone phoneme", MB_ICONERROR);
          return TRUE;
        }

        st->ok = true;
        EndDialog(hDlg, IDOK);
        return TRUE;
      }
      if (LOWORD(wParam) == IDCANCEL) {
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
      }
      break;
    }
  }

  return FALSE;
}


static bool tryParseDoubleStrict(const std::wstring& s, double& out) {
  const wchar_t* p = s.c_str();
  wchar_t* end = nullptr;
  out = wcstod(p, &end);
  if (end == p) return false;
  // Allow trailing whitespace only.
  while (end && (*end == L' ' || *end == L'\t' || *end == L'\r' || *end == L'\n')) ++end;
  return end && *end == 0;
}

static std::wstring formatDoubleSmart(double v) {
  // Prefer integer formatting when the value is very close to an integer.
  const double r = std::round(v);
  if (std::fabs(v - r) < 1e-9) {
    wchar_t buf[64];
    swprintf_s(buf, L"%.0f", r);
    return buf;
  }

  // Otherwise format with a few decimals and trim trailing zeros.
  wchar_t buf[64];
  swprintf_s(buf, L"%.6f", v);
  std::wstring out = buf;
  // Trim trailing zeros
  while (!out.empty() && out.back() == L'0') out.pop_back();
  // Trim trailing dot
  if (!out.empty() && out.back() == L'.') out.pop_back();
  return out;
}

static LRESULT CALLBACK numericSpinEditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                   UINT_PTR uIdSubclass, DWORD_PTR) {
  if (msg == WM_NCDESTROY) {
    RemoveWindowSubclass(hwnd, numericSpinEditSubclassProc, uIdSubclass);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
  }
  if (msg == WM_KEYDOWN) {
    if (wParam == VK_UP || wParam == VK_DOWN || wParam == VK_PRIOR || wParam == VK_NEXT) {
      wchar_t buf[256];
      GetWindowTextW(hwnd, buf, 256);
      double v = 0.0;
      if (!tryParseDoubleStrict(buf, v)) {
        return DefSubclassProc(hwnd, msg, wParam, lParam);
      }
      const double step = (wParam == VK_PRIOR || wParam == VK_NEXT) ? 50.0 : 1.0;
      v += (wParam == VK_UP || wParam == VK_PRIOR) ? step : -step;
      std::wstring out = formatDoubleSmart(v);
      SetWindowTextW(hwnd, out.c_str());
      SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(out.size()), static_cast<LPARAM>(out.size()));
      return 0;
    }
  }
  return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static INT_PTR CALLBACK EditValueDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
  EditValueDialogState* st = reinterpret_cast<EditValueDialogState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));

  auto schedulePreview = [&]() {
    if (!st || !st->livePreview || !st->armed) return;
    if (st->previewTimer) KillTimer(hDlg, st->previewTimer);
    st->previewTimer = SetTimer(hDlg, 1, 250, nullptr);
  };

  auto doPreview = [&]() {
    if (!st || !st->livePreview || !st->armed) return;
    if (!st->runtime) return;
    if (!st->runtime->dllsLoaded()) return;
    if (!st->baseMap.isMap()) return;

    // Grab current text from the edit control.
    wchar_t buf[1024];
    GetDlgItemTextW(hDlg, IDC_VAL_VALUE, buf, 1024);
    st->value = wideToUtf8(buf);

    tgsb_editor::Node tmp = st->baseMap;
    auto it = tmp.map.find(st->field);
    if (it == tmp.map.end()) {
      // If missing, create it.
      tmp.map[st->field] = tgsb_editor::Node{};
      it = tmp.map.find(st->field);
    }
    it->second.type = tgsb_editor::Node::Type::Scalar;
    it->second.scalar = st->value;

    std::vector<sample> samples;
    std::string err;
    if (!st->runtime->synthPreviewPhoneme(tmp, kSampleRate, samples, err)) {
      return; // silent on preview errors
    }
    if (samples.empty()) return;

    std::wstring wavPath = tgsb_editor::makeTempWavPath(L"nvpe");
    if (!tgsb_editor::writeWav16Mono(wavPath, kSampleRate, samples, err)) {
      return;
    }
    PlaySoundW(nullptr, NULL, SND_ASYNC);
    PlaySoundW(wavPath.c_str(), NULL, SND_FILENAME | SND_ASYNC);
  };

  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<EditValueDialogState*>(lParam);
      SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
      SetDlgItemTextW(hDlg, IDC_VAL_FIELD, formatFieldLabelW(st->field).c_str());
      SetDlgItemTextW(hDlg, IDC_VAL_VALUE, utf8ToWide(st->value).c_str());
      CheckDlgButton(hDlg, IDC_VAL_LIVE_PREVIEW, st->livePreview ? BST_CHECKED : BST_UNCHECKED);
      // Make the numeric field behave like a spinbox: Up/Down adjusts by 1, typing still works.
      if (HWND valEdit = GetDlgItem(hDlg, IDC_VAL_VALUE)) {
        SetWindowSubclass(valEdit, numericSpinEditSubclassProc, 1, 0);
        // Select all so numeric edits are quick.
        SendMessageW(valEdit, EM_SETSEL, 0, -1);
      }
      st->armed = true;
      return TRUE;
    }

    case WM_TIMER: {
      if (!st) break;
      if (wParam == 1) {
        KillTimer(hDlg, 1);
        st->previewTimer = 0;
        doPreview();
        return TRUE;
      }
      break;
    }

    case WM_COMMAND: {
      if (!st) break;

      if (LOWORD(wParam) == IDC_VAL_LIVE_PREVIEW) {
        st->livePreview = (IsDlgButtonChecked(hDlg, IDC_VAL_LIVE_PREVIEW) == BST_CHECKED);
        if (st->livePreview) schedulePreview();
        return TRUE;
      }

      if (LOWORD(wParam) == IDC_VAL_VALUE && HIWORD(wParam) == EN_CHANGE) {
        schedulePreview();
        return TRUE;
      }

      if (LOWORD(wParam) == IDOK) {
        if (st->previewTimer) {
          KillTimer(hDlg, st->previewTimer);
          st->previewTimer = 0;
        }
        wchar_t buf[1024];
        GetDlgItemTextW(hDlg, IDC_VAL_VALUE, buf, 1024);
        st->value = wideToUtf8(buf);
        st->ok = true;
        EndDialog(hDlg, IDOK);
        return TRUE;
      }
      if (LOWORD(wParam) == IDCANCEL) {
        if (st && st->previewTimer) {
          KillTimer(hDlg, st->previewTimer);
          st->previewTimer = 0;
        }
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
      }
      break;
    }
  }

  return FALSE;
}



// -------------------------
// Dialogs: Language settings
// -------------------------

static INT_PTR CALLBACK EditSettingDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
  EditSettingDialogState* st = reinterpret_cast<EditSettingDialogState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));

  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<EditSettingDialogState*>(lParam);
      SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

      HWND combo = GetDlgItem(hDlg, IDC_SETTING_KEY);
      if (combo) {
        comboFillKnownKeys(combo, st->knownKeys);
        SetWindowTextW(combo, utf8ToWide(st->key).c_str());
      }
      SetDlgItemTextW(hDlg, IDC_SETTING_VALUE, utf8ToWide(st->value).c_str());

      HWND valEdit = GetDlgItem(hDlg, IDC_SETTING_VALUE);
      if (valEdit) {
        SetWindowSubclass(valEdit, numericSpinEditSubclassProc, 1, 0);
      }
      return TRUE;
    }

    case WM_COMMAND: {
      if (!st) break;

      if (LOWORD(wParam) == IDOK) {
        wchar_t keyBuf[512];
        wchar_t valBuf[1024];
        GetDlgItemTextW(hDlg, IDC_SETTING_KEY, keyBuf, 512);
        GetDlgItemTextW(hDlg, IDC_SETTING_VALUE, valBuf, 1024);

        st->key = wideToUtf8(keyBuf);
        st->value = wideToUtf8(valBuf);

        // Basic trimming of surrounding whitespace for key.
        while (!st->key.empty() && (st->key.front() == ' ' || st->key.front() == '\t')) st->key.erase(st->key.begin());
        while (!st->key.empty() && (st->key.back() == ' ' || st->key.back() == '\t')) st->key.pop_back();

        if (st->key.empty()) {
          msgBox(hDlg, L"Key is required.", L"Edit setting", MB_ICONERROR);
          return TRUE;
        }

        st->ok = true;
        EndDialog(hDlg, IDOK);
        return TRUE;
      }

      if (LOWORD(wParam) == IDCANCEL) {
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
      }
      break;
    }
  }

  return FALSE;
}


static void settingsListAddColumns(HWND lv) {
  LVCOLUMNW col{};
  col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

  col.pszText = const_cast<wchar_t*>(L"Key");
  col.cx = 140;
  col.iSubItem = 0;
  ListView_InsertColumn(lv, 0, &col);

  col.pszText = const_cast<wchar_t*>(L"Value");
  col.cx = 120;
  col.iSubItem = 1;
  ListView_InsertColumn(lv, 1, &col);
}

static void settingsListPopulate(HWND lv, const std::vector<std::pair<std::string, std::string>>& settings) {
  ListView_DeleteAllItems(lv);
  int row = 0;
  for (const auto& kv : settings) {
    std::wstring k = utf8ToWide(kv.first);
    std::wstring v = utf8ToWide(kv.second);

    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.iSubItem = 0;
    it.pszText = const_cast<wchar_t*>(k.c_str());
    ListView_InsertItem(lv, &it);
    ListView_SetItemText(lv, row, 1, const_cast<wchar_t*>(v.c_str()));
    row++;
  }
}

static int settingsListSelectedIndex(HWND lv) {
  return ListView_GetNextItem(lv, -1, LVNI_SELECTED);
}

static void upsertSetting(std::vector<std::pair<std::string, std::string>>& vec, const std::string& key, const std::string& value) {
  // If key exists, update. Else insert.
  for (auto& kv : vec) {
    if (kv.first == key) {
      kv.second = value;
      return;
    }
  }
  vec.emplace_back(key, value);
}

static void sortSettings(std::vector<std::pair<std::string, std::string>>& vec) {
  std::sort(vec.begin(), vec.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
}

static INT_PTR CALLBACK EditSettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
  EditSettingsDialogState* st = reinterpret_cast<EditSettingsDialogState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));

  auto refresh = [&]() {
    HWND lv = GetDlgItem(hDlg, IDC_SETTINGS_LIST);
    if (!lv || !st) return;
    sortSettings(st->settings);
    settingsListPopulate(lv, st->settings);
    EnsureListViewHasSelection(lv);
  };

  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<EditSettingsDialogState*>(lParam);
      SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

      HWND lv = GetDlgItem(hDlg, IDC_SETTINGS_LIST);
      if (lv) {
        installAccessibleNameForListView(lv, L"Language settings");
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        settingsListAddColumns(lv);
      }

      refresh();
      return TRUE;
    }

    case WM_NOTIFY: {
      NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);
      if (hdr && hdr->code == NM_SETFOCUS && hdr->idFrom == IDC_SETTINGS_LIST) {
        EnsureListViewHasSelection(hdr->hwndFrom);
        return TRUE;
      }
      if (hdr && hdr->code == LVN_KEYDOWN && hdr->idFrom == IDC_SETTINGS_LIST) {
        auto* kd = (NMLVKEYDOWN*)lParam;
        if (kd->wVKey == VK_SPACE || kd->wVKey == VK_RETURN) {
          SendMessageW(
            hDlg,
            WM_COMMAND,
            MAKEWPARAM(IDC_SETTINGS_EDIT, BN_CLICKED),
            (LPARAM)GetDlgItem(hDlg, IDC_SETTINGS_EDIT)
          );
          return TRUE;
        }
      }
      break;
    }

    case WM_COMMAND: {
      if (!st) break;

      if (LOWORD(wParam) == IDC_SETTINGS_ADD) {
        EditSettingDialogState ed;
        ed.key.clear();
        ed.value.clear();
        ed.knownKeys = st->knownKeys;
        DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_EDIT_SETTING), hDlg, EditSettingDlgProc, reinterpret_cast<LPARAM>(&ed));
        if (ed.ok) {
          upsertSetting(st->settings, ed.key, ed.value);
          refresh();
        }
        return TRUE;
      }

      if (LOWORD(wParam) == IDC_SETTINGS_EDIT) {
        HWND lv = GetDlgItem(hDlg, IDC_SETTINGS_LIST);
        int sel = lv ? settingsListSelectedIndex(lv) : -1;
        if (sel < 0 || sel >= static_cast<int>(st->settings.size())) {
          msgBox(hDlg, L"Select a setting first.", L"Language settings", MB_ICONINFORMATION);
          return TRUE;
        }

        EditSettingDialogState ed;
        ed.key = st->settings[static_cast<size_t>(sel)].first;
        ed.value = st->settings[static_cast<size_t>(sel)].second;
        ed.knownKeys = st->knownKeys;

        DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_EDIT_SETTING), hDlg, EditSettingDlgProc, reinterpret_cast<LPARAM>(&ed));
        if (ed.ok) {
          // Remove old entry (even if key changed), then upsert.
          st->settings.erase(st->settings.begin() + sel);
          upsertSetting(st->settings, ed.key, ed.value);
          refresh();
        }
        return TRUE;
      }

      if (LOWORD(wParam) == IDC_SETTINGS_REMOVE) {
        HWND lv = GetDlgItem(hDlg, IDC_SETTINGS_LIST);
        int sel = lv ? settingsListSelectedIndex(lv) : -1;
        if (sel < 0 || sel >= static_cast<int>(st->settings.size())) {
          msgBox(hDlg, L"Select a setting first.", L"Language settings", MB_ICONINFORMATION);
          return TRUE;
        }
        st->settings.erase(st->settings.begin() + sel);
        refresh();
        return TRUE;
      }

      if (LOWORD(wParam) == IDOK) {
        st->ok = true;
        EndDialog(hDlg, IDOK);
        return TRUE;
      }
      if (LOWORD(wParam) == IDCANCEL) {
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
      }
      break;
    }
  }

  return FALSE;
}

static void listviewAddColumns(HWND lv) {
  LVCOLUMNW col{};
  col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

  col.pszText = const_cast<wchar_t*>(L"Field");
  col.cx = 140;
  col.iSubItem = 0;
  ListView_InsertColumn(lv, 0, &col);

  col.pszText = const_cast<wchar_t*>(L"Value");
  col.cx = 120;
  col.iSubItem = 1;
  ListView_InsertColumn(lv, 1, &col);
}

static std::vector<std::string> sortedNodeKeys(const tgsb_editor::Node& n) {
  std::vector<std::string> keys;
  if (!n.isMap()) return keys;
  keys.reserve(n.map.size());
  for (const auto& kv : n.map) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  return keys;
}

// Standard phoneme type flags that should always be shown in the editor,
// even if they are not defined for a given phoneme. These are metadata flags
// used by timing rules and special-case handling in the engine.
static const std::vector<std::string>& getStandardPhonemeTypeFlags() {
  static const std::vector<std::string> flags = {
    "_copyAdjacent",
    "_isAffricate",
    "_isLiquid",
    "_isNasal",
    "_isSemivowel",
    "_isStop",
    "_isTap",
    "_isTrill",
    "_isVoiced",
    "_isVowel",
    // FrameEx voice quality (stored under frameEx: in YAML)
    "frameEx.creakiness",
    "frameEx.breathiness",
    "frameEx.jitter",
    "frameEx.shimmer",
    "frameEx.sharpness",
    // Formant end targets for within-frame ramping (DECTalk-style coarticulation)
    "frameEx.endCf1",
    "frameEx.endCf2",
    "frameEx.endCf3",
    "frameEx.endPf1",
    "frameEx.endPf2",
    "frameEx.endPf3",
    // Frication spectral tilt (DSP v9): 0=flat, negative=darken high-freq parallels.
    // Rate-modulated automatically by engine; per-phoneme value here is a baseline.
    "frameEx.fricationTiltDb",
    // Micro-event shaping (stop burst, voice bar, fricative envelope, release)
    "burstDurationMs",
    "burstDecayRate",
    "burstSpectralTilt",
    "voiceBarAmplitude",
    "voiceBarF1",
    "releaseSpreadMs",
    "fricAttackMs",
    "fricDecayMs",
    "durationScale",
    // Per-phoneme closure gap override (b2, issue #84/#95).
    // Decouples closure silence from stop body duration. Default: language-level stopClosureVowelGapMs.
    "closureGapMs"
  };
  return flags;
}

static void populatePhonemeFieldsList(HWND lv, const tgsb_editor::Node& phonemeMap) {
  ListView_DeleteAllItems(lv);
  
  // First, collect all keys from the phoneme map
  auto existingKeys = sortedNodeKeys(phonemeMap);
  
  // Build a set of keys to show: existing keys + standard type flags
  std::vector<std::string> allKeys;
  std::unordered_set<std::string> seen;
  
  // Add existing keys first (skip "frameEx" itself since we'll show its children)
  for (const auto& k : existingKeys) {
    if (k == "frameEx") continue;  // Handle specially below
    allKeys.push_back(k);
    seen.insert(k);
  }
  
  // Add frameEx.* keys if frameEx exists in the map
  auto frameExIt = phonemeMap.map.find("frameEx");
  if (frameExIt != phonemeMap.map.end() && frameExIt->second.isMap()) {
    for (const auto& kv : frameExIt->second.map) {
      std::string flatKey = "frameEx." + kv.first;
      if (seen.find(flatKey) == seen.end()) {
        allKeys.push_back(flatKey);
        seen.insert(flatKey);
      }
    }
  }
  
  // Add standard type flags that aren't already present
  for (const auto& flag : getStandardPhonemeTypeFlags()) {
    if (seen.find(flag) == seen.end()) {
      allKeys.push_back(flag);
      seen.insert(flag);
    }
  }
  
  // Sort all keys
  std::sort(allKeys.begin(), allKeys.end());

  int row = 0;
  for (const auto& k : allKeys) {
    bool exists = false;
    std::string valueStr;
    
    // Handle frameEx.* keys specially
    if (k.rfind("frameEx.", 0) == 0) {
      std::string subKey = k.substr(8);  // Remove "frameEx." prefix
      auto fxIt = phonemeMap.map.find("frameEx");
      if (fxIt != phonemeMap.map.end() && fxIt->second.isMap()) {
        auto subIt = fxIt->second.map.find(subKey);
        if (subIt != fxIt->second.map.end() && subIt->second.isScalar()) {
          exists = true;
          valueStr = subIt->second.scalar;
        }
      }
    } else {
      auto it = phonemeMap.map.find(k);
      exists = (it != phonemeMap.map.end() && it->second.isScalar());
      
      // Skip non-scalar values that exist (like nested maps)
      if (it != phonemeMap.map.end() && !it->second.isScalar()) continue;
      
      if (exists) valueStr = it->second.scalar;
    }

    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.iSubItem = 0;
    std::wstring wk = formatFieldLabelW(k);
    item.pszText = wk.data();
    ListView_InsertItem(lv, &item);

    std::wstring wv = exists ? utf8ToWide(valueStr) : L"(not set)";
    ListView_SetItemText(lv, row, 1, const_cast<wchar_t*>(wv.c_str()));

    row++;
  }
}

static std::string getSelectedField(HWND lv, const tgsb_editor::Node& phonemeMap) {
  int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
  if (sel < 0) return {};

  wchar_t buf[512];
  ListView_GetItemText(lv, sel, 0, buf, 512);
  return extractFieldKeyFromLabel(wideToUtf8(buf));
}

static INT_PTR CALLBACK EditPhonemeDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
  EditPhonemeDialogState* st = reinterpret_cast<EditPhonemeDialogState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));

  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<EditPhonemeDialogState*>(lParam);
      SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

      SetDlgItemTextW(hDlg, IDC_PHONEME_KEY_LABEL, (L"Phoneme: " + utf8ToWide(st->phonemeKey)).c_str());

      HWND lv = GetDlgItem(hDlg, IDC_PHONEME_FIELDS);
      if (lv) installAccessibleNameForListView(lv, L"Phoneme fields");
      ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
      listviewAddColumns(lv);
      populatePhonemeFieldsList(lv, st->working);
      EnsureListViewHasSelection(lv);

      return TRUE;
    }

    case WM_COMMAND: {
      if (!st) break;

      if (LOWORD(wParam) == IDC_PHONEME_EDIT_VALUE) {
        HWND lv = GetDlgItem(hDlg, IDC_PHONEME_FIELDS);
        std::string field = getSelectedField(lv, st->working);
        if (field.empty()) {
          msgBox(hDlg, L"Select a field first.", L"Edit phoneme", MB_ICONINFORMATION);
          return TRUE;
        }

        // Handle frameEx.* fields specially
        bool isFrameExField = (field.rfind("frameEx.", 0) == 0);
        std::string subKey;
        bool fieldExists = false;
        std::string currentValue;
        
        if (isFrameExField) {
          subKey = field.substr(8);  // Remove "frameEx." prefix
          auto fxIt = st->working.map.find("frameEx");
          if (fxIt != st->working.map.end() && fxIt->second.isMap()) {
            auto subIt = fxIt->second.map.find(subKey);
            if (subIt != fxIt->second.map.end() && subIt->second.isScalar()) {
              fieldExists = true;
              currentValue = subIt->second.scalar;
            }
          }
        } else {
          auto it = st->working.map.find(field);
          fieldExists = (it != st->working.map.end());
          
          // If field exists but is not scalar, reject it
          if (fieldExists && !it->second.isScalar()) {
            msgBox(hDlg, L"That field isn't a scalar value.", L"Edit phoneme", MB_ICONERROR);
            return TRUE;
          }
          if (fieldExists) currentValue = it->second.scalar;
        }

        EditValueDialogState vs;
        vs.field = field;
        vs.value = currentValue;
        vs.baseMap = st->working;
        vs.runtime = st->runtime;
        vs.livePreview = true;

        DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_EDIT_VALUE), hDlg, EditValueDlgProc, reinterpret_cast<LPARAM>(&vs));
        if (vs.ok) {
          if (isFrameExField) {
            // Create frameEx map if it doesn't exist
            auto fxIt = st->working.map.find("frameEx");
            if (fxIt == st->working.map.end()) {
              st->working.map["frameEx"] = tgsb_editor::Node{};
              st->working.map["frameEx"].type = tgsb_editor::Node::Type::Map;
              fxIt = st->working.map.find("frameEx");
            }
            // Create or update the subkey
            fxIt->second.map[subKey] = tgsb_editor::Node{};
            fxIt->second.map[subKey].type = tgsb_editor::Node::Type::Scalar;
            fxIt->second.map[subKey].scalar = vs.value;
          } else {
            // Create or update the field at top level
            auto it = st->working.map.find(field);
            if (it == st->working.map.end()) {
              st->working.map[field] = tgsb_editor::Node{};
              it = st->working.map.find(field);
            }
            it->second.type = tgsb_editor::Node::Type::Scalar;
            it->second.scalar = vs.value;
          }
          populatePhonemeFieldsList(lv, st->working);
          EnsureListViewHasSelection(lv);
        }
        return TRUE;
      }

      if (LOWORD(wParam) == IDOK) {
        st->ok = true;
        EndDialog(hDlg, IDOK);
        return TRUE;
      }
      if (LOWORD(wParam) == IDCANCEL) {
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
      }
      break;
    }

    case WM_NOTIFY: {
      NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);
      if (!hdr) break;

      if (hdr->code == NM_SETFOCUS && hdr->idFrom == IDC_PHONEME_FIELDS) {
        EnsureListViewHasSelection(hdr->hwndFrom);
        return TRUE;
      }

      if (hdr->code == LVN_KEYDOWN && hdr->idFrom == IDC_PHONEME_FIELDS) {
        auto* kd = reinterpret_cast<NMLVKEYDOWN*>(lParam);
        if (kd->wVKey == VK_SPACE || kd->wVKey == VK_RETURN) {
          SendMessageW(
            hDlg,
            WM_COMMAND,
            MAKEWPARAM(IDC_PHONEME_EDIT_VALUE, BN_CLICKED),
            (LPARAM)GetDlgItem(hDlg, IDC_PHONEME_EDIT_VALUE)
          );
          return TRUE;
        }
      }
      break;
    }
  }

  return FALSE;
}

// -------------------------
// Speech settings persistence
// -------------------------
tgsb_editor::SpeechSettings loadSpeechSettingsFromIni() {
  tgsb_editor::SpeechSettings s;
  s.voiceName = wideToUtf8(readIni(L"speech", L"voice", L"Adam"));
  s.rate = readIniInt(L"speech", L"rate", s.rate);
  s.pitch = readIniInt(L"speech", L"pitch", s.pitch);
  s.volume = readIniInt(L"speech", L"volume", s.volume);
  s.inflection = readIniInt(L"speech", L"inflection", s.inflection);
  s.pauseMode = wideToUtf8(readIni(L"speech", L"pauseMode", L"short"));

  const auto& names = TgsbRuntime::frameParamNames();
  s.frameParams.assign(names.size(), 50);
  for (size_t i = 0; i < names.size(); ++i) {
    std::wstring key = L"frame_" + utf8ToWide(names[i]);
    s.frameParams[i] = readIniInt(L"speech", key.c_str(), 50);
  }
  
  // Load voicing params from per-voice section if available, else from [speech]
  const auto& voicingNames = TgsbRuntime::voicingParamNames();
  s.voicingParams.assign(voicingNames.size(), 50);
  // tremorDepth (index 13) defaults to 0, not 50
  if (s.voicingParams.size() > 13) s.voicingParams[13] = 0;

  // Determine which INI section to use for voicing params
  // For Python presets: [voice_Adam], [voice_Benjamin], etc.
  // For profiles: use [speech] defaults (profiles get voicingTone from YAML)
  std::wstring voiceSection = L"speech";
  if (!TgsbRuntime::isVoiceProfile(s.voiceName)) {
    voiceSection = L"voice_" + utf8ToWide(s.voiceName);
  }
  
  for (size_t i = 0; i < voicingNames.size(); ++i) {
std::wstring key = L"voicing_" + utf8ToWide(voicingNames[i]);
    // Try voice-specific section first, fallback to [speech] defaults
    // tremorDepth (13), chorusDepth (17) default to 0; chorusDetune (18) to 33
    int defaultVal = (i == 13 || i == 17) ? 0 : (i == 18) ? 33 : 50;
    int val = readIniInt(voiceSection.c_str(), key.c_str(), -1);
    if (val < 0) {
      val = readIniInt(L"speech", key.c_str(), defaultVal);
    }
    s.voicingParams[i] = val;
  }
  return s;
}

void saveSpeechSettingsToIni(const tgsb_editor::SpeechSettings& s) {
  writeIni(L"speech", L"voice", utf8ToWide(s.voiceName));
  writeIniInt(L"speech", L"rate", s.rate);
  writeIniInt(L"speech", L"pitch", s.pitch);
  writeIniInt(L"speech", L"volume", s.volume);
  writeIniInt(L"speech", L"inflection", s.inflection);
  writeIni(L"speech", L"pauseMode", utf8ToWide(s.pauseMode));

  const auto& names = TgsbRuntime::frameParamNames();
  for (size_t i = 0; i < names.size() && i < s.frameParams.size(); ++i) {
    std::wstring key = L"frame_" + utf8ToWide(names[i]);
    writeIniInt(L"speech", key.c_str(), s.frameParams[i]);
  }
  
  // Save voicing params to per-voice section for Python presets
  // For profiles, voicingTone is stored in YAML, so we still save to [speech] as fallback
  const auto& voicingNames = TgsbRuntime::voicingParamNames();
  
  std::wstring voiceSection = L"speech";
  if (!TgsbRuntime::isVoiceProfile(s.voiceName)) {
    voiceSection = L"voice_" + utf8ToWide(s.voiceName);
  }
  
  for (size_t i = 0; i < voicingNames.size() && i < s.voicingParams.size(); ++i) {
    std::wstring key = L"voicing_" + utf8ToWide(voicingNames[i]);
    writeIniInt(voiceSection.c_str(), key.c_str(), s.voicingParams[i]);
  }
}


static void setTrackbarRangeAndPos(HWND tb, int pos) {
  if (!tb) return;
  SendMessageW(tb, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
  SendMessageW(tb, TBM_SETTICFREQ, 10, 0);
  SendMessageW(tb, TBM_SETPOS, TRUE, pos);
}

static int getTrackbarPos(HWND tb) {
  if (!tb) return 0;
  return static_cast<int>(SendMessageW(tb, TBM_GETPOS, 0, 0));
}

static void setDlgIntText(HWND hDlg, int id, int value) {
  wchar_t buf[64];
  _itow_s(value, buf, 10);
  SetDlgItemTextW(hDlg, id, buf);
}

static void fillVoices(HWND combo, const std::string& selected, const std::vector<std::string>& profiles) {
  if (!combo) return;
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  
  // Python presets first
  const char* presets[] = {"Adam", "Benjamin", "Caleb", "David", "Robert"};
  int sel = 0;
  int idx = 0;
  
  for (const char* preset : presets) {
    std::wstring w = utf8ToWide(preset);
    int pos = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str())));
    if (selected == preset) sel = pos;
    ++idx;
  }
  
  // Voice profiles from phonemes.yaml
  for (const std::string& profileName : profiles) {
    // Display as "profileName (profile)" to distinguish from presets
    std::string displayName = profileName + " (profile)";
    std::wstring w = utf8ToWide(displayName);
    int pos = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str())));
    
    // Voice ID uses prefix
    std::string voiceId = std::string(tgsb_editor::TgsbRuntime::kVoiceProfilePrefix) + profileName;
    if (selected == voiceId) sel = pos;
  }
  
  SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

static void populateParamList(HWND list, const std::vector<std::string>& names, const std::vector<int>& values) {
  if (!list) return;
  SendMessageW(list, LB_RESETCONTENT, 0, 0);
  for (size_t i = 0; i < names.size(); ++i) {
    std::wstring text = formatSpeechParamRowW(names[i], (i < values.size()) ? values[i] : 50);
    SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
  }
  SendMessageW(list, LB_SETCURSEL, 0, 0);
}

static void refreshParamListRow(HWND list, size_t idx, const std::string& name, int value) {
  if (!list) return;
  std::wstring text = formatSpeechParamRowW(name, value);
  SendMessageW(list, LB_DELETESTRING, static_cast<WPARAM>(idx), 0);
  SendMessageW(list, LB_INSERTSTRING, static_cast<WPARAM>(idx), reinterpret_cast<LPARAM>(text.c_str()));
}

static INT_PTR CALLBACK SpeechSettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
  SpeechSettingsDialogState* st = reinterpret_cast<SpeechSettingsDialogState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));

  auto syncSelectedParamToUi = [&]() {
    if (!st) return;
    HWND lb = GetDlgItem(hDlg, IDC_SPEECH_PARAM_LIST);
    int sel = lb ? static_cast<int>(SendMessageW(lb, LB_GETCURSEL, 0, 0)) : -1;
    if (sel < 0) sel = 0;
    if (sel >= static_cast<int>(st->paramNames.size())) return;
    int v = (sel < static_cast<int>(st->settings.frameParams.size())) ? st->settings.frameParams[static_cast<size_t>(sel)] : 50;
    HWND tb = GetDlgItem(hDlg, IDC_SPEECH_PARAM_SLIDER);
    setTrackbarRangeAndPos(tb, v);
    setDlgIntText(hDlg, IDC_SPEECH_PARAM_VAL, v);
  };

  auto syncSelectedVoicingParamToUi = [&]() {
    if (!st) return;
    HWND lb = GetDlgItem(hDlg, IDC_SPEECH_VOICING_LIST);
    int sel = lb ? static_cast<int>(SendMessageW(lb, LB_GETCURSEL, 0, 0)) : -1;
    if (sel < 0) sel = 0;
    if (sel >= static_cast<int>(st->voicingParamNames.size())) return;
    int v = (sel < static_cast<int>(st->settings.voicingParams.size())) ? st->settings.voicingParams[static_cast<size_t>(sel)] : 50;
    HWND tb = GetDlgItem(hDlg, IDC_SPEECH_VOICING_SLIDER);
    setTrackbarRangeAndPos(tb, v);
    setDlgIntText(hDlg, IDC_SPEECH_VOICING_VAL, v);
  };

  auto syncSelectedFrameExParamToUi = [&]() {
    if (!st) return;
    HWND lb = GetDlgItem(hDlg, IDC_SPEECH_FRAMEEX_LIST);
    int sel = lb ? static_cast<int>(SendMessageW(lb, LB_GETCURSEL, 0, 0)) : -1;
    if (sel < 0) sel = 0;
    if (sel >= static_cast<int>(st->frameExParamNames.size())) return;
    // Default: 0 for creakiness/breathiness/jitter/shimmer, 50 for sharpness
    int defaultVal = (sel == 4) ? 50 : 0;
    int v = (sel < static_cast<int>(st->settings.frameExParams.size())) ? st->settings.frameExParams[static_cast<size_t>(sel)] : defaultVal;
    HWND tb = GetDlgItem(hDlg, IDC_SPEECH_FRAMEEX_SLIDER);
    setTrackbarRangeAndPos(tb, v);
    setDlgIntText(hDlg, IDC_SPEECH_FRAMEEX_VAL, v);
  };

  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<SpeechSettingsDialogState*>(lParam);
      SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

      // Accessible names for any ListView controls (none here), and predictable defaults.
      HWND combo = GetDlgItem(hDlg, IDC_SPEECH_VOICE);
      fillVoices(combo, st->settings.voiceName, st->voiceProfiles);

      setTrackbarRangeAndPos(GetDlgItem(hDlg, IDC_SPEECH_RATE_SLIDER), st->settings.rate);
      setDlgIntText(hDlg, IDC_SPEECH_RATE_VAL, st->settings.rate);

      setTrackbarRangeAndPos(GetDlgItem(hDlg, IDC_SPEECH_PITCH_SLIDER), st->settings.pitch);
      setDlgIntText(hDlg, IDC_SPEECH_PITCH_VAL, st->settings.pitch);

      setTrackbarRangeAndPos(GetDlgItem(hDlg, IDC_SPEECH_VOLUME_SLIDER), st->settings.volume);
      setDlgIntText(hDlg, IDC_SPEECH_VOLUME_VAL, st->settings.volume);

      setTrackbarRangeAndPos(GetDlgItem(hDlg, IDC_SPEECH_INFLECTION_SLIDER), st->settings.inflection);
      setDlgIntText(hDlg, IDC_SPEECH_INFLECTION_VAL, st->settings.inflection);

      // Frame param list
      HWND lb = GetDlgItem(hDlg, IDC_SPEECH_PARAM_LIST);
      populateParamList(lb, st->paramNames, st->settings.frameParams);
      syncSelectedParamToUi();
      
      // Voicing param list
      HWND vlb = GetDlgItem(hDlg, IDC_SPEECH_VOICING_LIST);
      populateParamList(vlb, st->voicingParamNames, st->settings.voicingParams);
      syncSelectedVoicingParamToUi();
      
      // FrameEx param list (voice quality)
      HWND flb = GetDlgItem(hDlg, IDC_SPEECH_FRAMEEX_LIST);
      populateParamList(flb, st->frameExParamNames, st->settings.frameExParams);
      syncSelectedFrameExParamToUi();
      
      return TRUE;
    }

    case WM_HSCROLL: {
      if (!st) break;
      HWND src = reinterpret_cast<HWND>(lParam);
      if (!src) break;

      const int id = GetDlgCtrlID(src);
      if (id == IDC_SPEECH_RATE_SLIDER) {
        st->settings.rate = getTrackbarPos(src);
        setDlgIntText(hDlg, IDC_SPEECH_RATE_VAL, st->settings.rate);
        return TRUE;
      }
      if (id == IDC_SPEECH_PITCH_SLIDER) {
        st->settings.pitch = getTrackbarPos(src);
        setDlgIntText(hDlg, IDC_SPEECH_PITCH_VAL, st->settings.pitch);
        return TRUE;
      }
      if (id == IDC_SPEECH_VOLUME_SLIDER) {
        st->settings.volume = getTrackbarPos(src);
        setDlgIntText(hDlg, IDC_SPEECH_VOLUME_VAL, st->settings.volume);
        return TRUE;
      }
      if (id == IDC_SPEECH_INFLECTION_SLIDER) {
        st->settings.inflection = getTrackbarPos(src);
        setDlgIntText(hDlg, IDC_SPEECH_INFLECTION_VAL, st->settings.inflection);
        return TRUE;
      }
      if (id == IDC_SPEECH_PARAM_SLIDER) {
        int v = getTrackbarPos(src);
        HWND lb = GetDlgItem(hDlg, IDC_SPEECH_PARAM_LIST);
        int sel = lb ? static_cast<int>(SendMessageW(lb, LB_GETCURSEL, 0, 0)) : -1;
        if (sel < 0) sel = 0;
        if (sel >= 0 && sel < static_cast<int>(st->settings.frameParams.size())) {
          st->settings.frameParams[static_cast<size_t>(sel)] = v;
          setDlgIntText(hDlg, IDC_SPEECH_PARAM_VAL, v);
          if (sel < static_cast<int>(st->paramNames.size())) {
            refreshParamListRow(lb, static_cast<size_t>(sel), st->paramNames[static_cast<size_t>(sel)], v);
            SendMessageW(lb, LB_SETCURSEL, sel, 0);
          }
        }
        return TRUE;
      }
      if (id == IDC_SPEECH_VOICING_SLIDER) {
        int v = getTrackbarPos(src);
        HWND lb = GetDlgItem(hDlg, IDC_SPEECH_VOICING_LIST);
        int sel = lb ? static_cast<int>(SendMessageW(lb, LB_GETCURSEL, 0, 0)) : -1;
        if (sel < 0) sel = 0;
        if (sel >= 0 && sel < static_cast<int>(st->settings.voicingParams.size())) {
          st->settings.voicingParams[static_cast<size_t>(sel)] = v;
          setDlgIntText(hDlg, IDC_SPEECH_VOICING_VAL, v);
          if (sel < static_cast<int>(st->voicingParamNames.size())) {
            refreshParamListRow(lb, static_cast<size_t>(sel), st->voicingParamNames[static_cast<size_t>(sel)], v);
            SendMessageW(lb, LB_SETCURSEL, sel, 0);
          }
        }
        return TRUE;
      }
      if (id == IDC_SPEECH_FRAMEEX_SLIDER) {
        int v = getTrackbarPos(src);
        HWND lb = GetDlgItem(hDlg, IDC_SPEECH_FRAMEEX_LIST);
        int sel = lb ? static_cast<int>(SendMessageW(lb, LB_GETCURSEL, 0, 0)) : -1;
        if (sel < 0) sel = 0;
        if (sel >= 0 && sel < static_cast<int>(st->settings.frameExParams.size())) {
          st->settings.frameExParams[static_cast<size_t>(sel)] = v;
          setDlgIntText(hDlg, IDC_SPEECH_FRAMEEX_VAL, v);
          if (sel < static_cast<int>(st->frameExParamNames.size())) {
            refreshParamListRow(lb, static_cast<size_t>(sel), st->frameExParamNames[static_cast<size_t>(sel)], v);
            SendMessageW(lb, LB_SETCURSEL, sel, 0);
          }
        }
        return TRUE;
      }
      break;
    }

    case WM_COMMAND: {
      if (!st) break;
      const int id = LOWORD(wParam);
      const int code = HIWORD(wParam);

      if (id == IDC_SPEECH_VOICE && code == CBN_SELCHANGE) {
        HWND combo = GetDlgItem(hDlg, IDC_SPEECH_VOICE);
        int sel = combo ? static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0)) : -1;
        if (sel >= 0) {
          wchar_t buf[128];
          SendMessageW(combo, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(buf));
          std::string displayName = wideToUtf8(buf);
          
          std::string newVoiceName;
          
          // Check if this is a profile (ends with " (profile)")
          const std::string suffix = " (profile)";
          if (displayName.size() > suffix.size() &&
              displayName.substr(displayName.size() - suffix.size()) == suffix) {
            // Extract profile name and add prefix
            std::string profileName = displayName.substr(0, displayName.size() - suffix.size());
            newVoiceName = std::string(tgsb_editor::TgsbRuntime::kVoiceProfilePrefix) + profileName;
            
            // Set the voice profile on the frontend
            if (st->runtime) {
              std::string err;
              st->runtime->setVoiceProfile(profileName, err);
            }
          } else {
            // Regular Python preset
            newVoiceName = displayName;
            
            // Clear any active voice profile
            if (st->runtime) {
              std::string err;
              st->runtime->setVoiceProfile("", err);
            }
          }
          
          // Load voicing params for this voice from INI
          st->settings.voiceName = newVoiceName;
          
          std::wstring voiceSection = L"speech";
          if (!tgsb_editor::TgsbRuntime::isVoiceProfile(newVoiceName)) {
            voiceSection = L"voice_" + utf8ToWide(newVoiceName);
          }
          
          const auto& voicingNames = tgsb_editor::TgsbRuntime::voicingParamNames();
          for (size_t i = 0; i < voicingNames.size() && i < st->settings.voicingParams.size(); ++i) {
            std::wstring key = L"voicing_" + utf8ToWide(voicingNames[i]);
            int val = readIniInt(voiceSection.c_str(), key.c_str(), -1);
            if (val < 0) {
              val = readIniInt(L"speech", key.c_str(), 50);
            }
            st->settings.voicingParams[i] = val;
          }
          
          // Refresh voicing params list UI
          HWND vlb = GetDlgItem(hDlg, IDC_SPEECH_VOICING_LIST);
          populateParamList(vlb, st->voicingParamNames, st->settings.voicingParams);
          syncSelectedVoicingParamToUi();
        }
        return TRUE;
      }

      if (id == IDC_SPEECH_PARAM_LIST && code == LBN_SELCHANGE) {
        syncSelectedParamToUi();
        return TRUE;
      }

      if (id == IDC_SPEECH_VOICING_LIST && code == LBN_SELCHANGE) {
        syncSelectedVoicingParamToUi();
        return TRUE;
      }

      if (id == IDC_SPEECH_FRAMEEX_LIST && code == LBN_SELCHANGE) {
        syncSelectedFrameExParamToUi();
        return TRUE;
      }

      if (id == IDC_SPEECH_PARAM_RESET) {
        HWND lb = GetDlgItem(hDlg, IDC_SPEECH_PARAM_LIST);
        int sel = lb ? static_cast<int>(SendMessageW(lb, LB_GETCURSEL, 0, 0)) : -1;
        if (sel < 0) sel = 0;
        if (sel >= 0 && sel < static_cast<int>(st->settings.frameParams.size())) {
          st->settings.frameParams[static_cast<size_t>(sel)] = 50;
          setTrackbarRangeAndPos(GetDlgItem(hDlg, IDC_SPEECH_PARAM_SLIDER), 50);
          setDlgIntText(hDlg, IDC_SPEECH_PARAM_VAL, 50);
          if (sel < static_cast<int>(st->paramNames.size())) {
            refreshParamListRow(lb, static_cast<size_t>(sel), st->paramNames[static_cast<size_t>(sel)], 50);
            SendMessageW(lb, LB_SETCURSEL, sel, 0);
          }
        }
        return TRUE;
      }

      if (id == IDC_SPEECH_VOICING_RESET) {
        HWND lb = GetDlgItem(hDlg, IDC_SPEECH_VOICING_LIST);
        int sel = lb ? static_cast<int>(SendMessageW(lb, LB_GETCURSEL, 0, 0)) : -1;
        if (sel < 0) sel = 0;
        if (sel >= 0 && sel < static_cast<int>(st->settings.voicingParams.size())) {
          st->settings.voicingParams[static_cast<size_t>(sel)] = 50;
          setTrackbarRangeAndPos(GetDlgItem(hDlg, IDC_SPEECH_VOICING_SLIDER), 50);
          setDlgIntText(hDlg, IDC_SPEECH_VOICING_VAL, 50);
          if (sel < static_cast<int>(st->voicingParamNames.size())) {
            refreshParamListRow(lb, static_cast<size_t>(sel), st->voicingParamNames[static_cast<size_t>(sel)], 50);
            SendMessageW(lb, LB_SETCURSEL, sel, 0);
          }
        }
        return TRUE;
      }

      if (id == IDC_SPEECH_RESET_ALL) {
        st->settings.frameParams.assign(st->paramNames.size(), 50);
        st->settings.voiceName = st->settings.voiceName.empty() ? "Adam" : st->settings.voiceName;
        HWND lb = GetDlgItem(hDlg, IDC_SPEECH_PARAM_LIST);
        populateParamList(lb, st->paramNames, st->settings.frameParams);
        syncSelectedParamToUi();
        return TRUE;
      }
if (id == IDC_SPEECH_VOICING_RESET_ALL) {
        st->settings.voicingParams.assign(st->voicingParamNames.size(), 50);
        // tremorDepth (13), chorusDepth (17) default to 0; chorusDetune (18) defaults to 33
        if (st->settings.voicingParams.size() > 13) st->settings.voicingParams[13] = 0;
        if (st->settings.voicingParams.size() > 17) st->settings.voicingParams[17] = 0;
        if (st->settings.voicingParams.size() > 18) st->settings.voicingParams[18] = 33;
        HWND lb = GetDlgItem(hDlg, IDC_SPEECH_VOICING_LIST);
        populateParamList(lb, st->voicingParamNames, st->settings.voicingParams);
        syncSelectedVoicingParamToUi();
        return TRUE;
      }

      if (id == IDC_SPEECH_FRAMEEX_RESET) {
        HWND lb = GetDlgItem(hDlg, IDC_SPEECH_FRAMEEX_LIST);
        int sel = lb ? static_cast<int>(SendMessageW(lb, LB_GETCURSEL, 0, 0)) : -1;
        if (sel < 0) sel = 0;
        if (sel >= 0 && sel < static_cast<int>(st->settings.frameExParams.size())) {
          // Default: 0 for creakiness/breathiness/jitter/shimmer, 50 for sharpness
          int defaultVal = (sel == 4) ? 50 : 0;
          st->settings.frameExParams[static_cast<size_t>(sel)] = defaultVal;
          setTrackbarRangeAndPos(GetDlgItem(hDlg, IDC_SPEECH_FRAMEEX_SLIDER), defaultVal);
          setDlgIntText(hDlg, IDC_SPEECH_FRAMEEX_VAL, defaultVal);
          if (sel < static_cast<int>(st->frameExParamNames.size())) {
            refreshParamListRow(lb, static_cast<size_t>(sel), st->frameExParamNames[static_cast<size_t>(sel)], defaultVal);
            SendMessageW(lb, LB_SETCURSEL, sel, 0);
          }
        }
        return TRUE;
      }

      if (id == IDC_SPEECH_FRAMEEX_RESET_ALL) {
        // Reset: creakiness/breathiness/jitter/shimmer=0, sharpness=50
        st->settings.frameExParams.assign(st->frameExParamNames.size(), 0);
        if (st->settings.frameExParams.size() >= 5) st->settings.frameExParams[4] = 50;
        HWND lb = GetDlgItem(hDlg, IDC_SPEECH_FRAMEEX_LIST);
        populateParamList(lb, st->frameExParamNames, st->settings.frameExParams);
        syncSelectedFrameExParamToUi();
        return TRUE;
      }

      if (id == IDC_SPEECH_SAVE_TO_PROFILE) {
        // Get profile name - strip "profile:" prefix if present, otherwise use voice name directly
        std::string profileName;
        if (tgsb_editor::TgsbRuntime::isVoiceProfile(st->settings.voiceName)) {
          profileName = tgsb_editor::TgsbRuntime::getProfileNameFromVoice(st->settings.voiceName);
        } else {
          profileName = st->settings.voiceName;
        }
        
        if (profileName.empty()) {
          msgBox(hDlg, L"No voice selected.", L"Save to Profile", MB_ICONERROR);
          return TRUE;
        }
        
        // Check runtime is available
        if (!st->runtime) {
          msgBox(hDlg, L"Runtime not available.", L"Save to Profile", MB_ICONERROR);
          return TRUE;
        }
        
        // Save the 17 params (12 voicing + 5 FrameEx) to phonemes.yaml
        std::string err;
        if (st->runtime->saveVoiceProfileSliders(profileName, st->settings.voicingParams, st->settings.frameExParams, err)) {
          std::wstring msg = L"Saved voicing and voice quality settings to profile \"" + utf8ToWide(profileName) + L"\" in phonemes.yaml.";
          msgBox(hDlg, msg.c_str(), L"Save to Profile", MB_ICONINFORMATION);
          
          // Refresh voice list so the new profile appears
          st->voiceProfiles = st->runtime->discoverVoiceProfiles();
          st->settings.voiceName = std::string(tgsb_editor::TgsbRuntime::kVoiceProfilePrefix) + profileName;
          HWND combo = GetDlgItem(hDlg, IDC_SPEECH_VOICE);
          fillVoices(combo, st->settings.voiceName, st->voiceProfiles);
        } else {
          std::wstring msg = L"Failed to save: " + utf8ToWide(err);
          msgBox(hDlg, msg.c_str(), L"Save to Profile", MB_ICONERROR);
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

// -------------------------
// Dialog launch helpers
// -------------------------

bool ShowAddMappingDialog(HINSTANCE hInst, HWND parent, AddMappingDialogState& st) {
  st.ok = false;
  DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_ADD_MAPPING), parent, AddMappingDlgProc, (LPARAM)&st);
  return st.ok;
}

bool ShowClonePhonemeDialog(HINSTANCE hInst, HWND parent, ClonePhonemeDialogState& st) {
  st.ok = false;
  DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_CLONE_PHONEME), parent, ClonePhonemeDlgProc, (LPARAM)&st);
  return st.ok;
}

bool ShowEditValueDialog(HINSTANCE hInst, HWND parent, EditValueDialogState& st) {
  st.ok = false;
  DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_EDIT_VALUE), parent, EditValueDlgProc, (LPARAM)&st);
  return st.ok;
}

bool ShowEditSettingsDialog(HINSTANCE hInst, HWND parent, EditSettingsDialogState& st) {
  st.ok = false;
  DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_EDIT_SETTINGS), parent, EditSettingsDlgProc, (LPARAM)&st);
  return st.ok;
}

bool ShowEditPhonemeDialog(HINSTANCE hInst, HWND parent, EditPhonemeDialogState& st) {
  st.ok = false;
  DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_EDIT_PHONEME), parent, EditPhonemeDlgProc, (LPARAM)&st);
  return st.ok;
}

bool ShowSpeechSettingsDialog(HINSTANCE hInst, HWND parent, SpeechSettingsDialogState& st) {
  st.ok = false;
  DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_SPEECH_SETTINGS), parent, SpeechSettingsDlgProc, (LPARAM)&st);
  return st.ok;
}


// ----------------------------
// Phonemizer settings dialog
// ----------------------------

static std::wstring getDlgItemTextAllocW(HWND hDlg, int id) {
  HWND h = GetDlgItem(hDlg, id);
  if (!h) return L"";
  int len = GetWindowTextLengthW(h);
  if (len <= 0) return L"";
  std::wstring buf;
  buf.resize(static_cast<size_t>(len) + 1);
  GetWindowTextW(h, buf.data(), len + 1);
  // Trim to actual length.
  buf.resize(wcslen(buf.c_str()));
  return buf;
}

struct PhonemizerTemplateItem {
  const wchar_t* name;
  const wchar_t* exePath;
  const wchar_t* argsStdin;
  const wchar_t* argsCli;
  bool preferStdin;
  int maxChunkChars;
  bool apply; // if false, selecting it doesn't overwrite fields
};

static const PhonemizerTemplateItem kPhonemizerTemplates[] = {
  {
    L"Custom (do not overwrite fields)",
    L"",
    L"",
    L"",
    true,
    420,
    false
  },
  {
    L"eSpeak NG (recommended, uses eSpeak directory if exe is blank)",
    L"",
    L"-q {pathArg}--ipa=3 -b 1 -v {qlang} --stdin",
    L"-q {pathArg}--ipa=3 -b 1 -v {qlang} {qtext}",
    true,
    420,
    true
  },
  {
    L"phonemize (Python phonemizer package, espeak backend)",
    L"phonemize",
    L"-l {qlang} -b espeak --strip -p _",
    L"",
    true,
    420,
    true
  }
};

static void setDlgItemIntText(HWND hDlg, int id, int val) {
  wchar_t buf[64];
  _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d", val);
  SetDlgItemTextW(hDlg, id, buf);
}

static void applyPhonemizerTemplate(HWND hDlg, int idx) {
  if (idx < 0) return;
  const int count = static_cast<int>(sizeof(kPhonemizerTemplates) / sizeof(kPhonemizerTemplates[0]));
  if (idx >= count) return;
  const auto& t = kPhonemizerTemplates[idx];
  if (!t.apply) return;

  SetDlgItemTextW(hDlg, IDC_PHONEMIZER_EXE, t.exePath ? t.exePath : L"");
  SetDlgItemTextW(hDlg, IDC_PHONEMIZER_ARGS_STDIN, t.argsStdin ? t.argsStdin : L"");
  SetDlgItemTextW(hDlg, IDC_PHONEMIZER_ARGS_CLI, t.argsCli ? t.argsCli : L"");
  setDlgItemIntText(hDlg, IDC_PHONEMIZER_MAXCHUNK, t.maxChunkChars);

  // Mode combo: 0 = prefer stdin, 1 = CLI only
  SendDlgItemMessageW(hDlg, IDC_PHONEMIZER_MODE, CB_SETCURSEL, t.preferStdin ? 0 : 1, 0);
}

static void updatePhonemizerDialogEnableState(HWND hDlg) {
  int mode = static_cast<int>(SendDlgItemMessageW(hDlg, IDC_PHONEMIZER_MODE, CB_GETCURSEL, 0, 0));
  bool preferStdin = (mode == 0);
  EnableWindow(GetDlgItem(hDlg, IDC_PHONEMIZER_ARGS_STDIN), preferStdin ? TRUE : FALSE);
}

static INT_PTR CALLBACK PhonemizerSettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
  auto* st = reinterpret_cast<PhonemizerSettingsDialogState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));

  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<PhonemizerSettingsDialogState*>(lParam);
      SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)st);

      // Populate template combo
      HWND cmbTemplate = GetDlgItem(hDlg, IDC_PHONEMIZER_TEMPLATE);
      if (cmbTemplate) {
        const int count = static_cast<int>(sizeof(kPhonemizerTemplates) / sizeof(kPhonemizerTemplates[0]));
        for (int i = 0; i < count; ++i) {
          SendMessageW(cmbTemplate, CB_ADDSTRING, 0, (LPARAM)kPhonemizerTemplates[i].name);
        }
        // Default selection is Custom.
        SendMessageW(cmbTemplate, CB_SETCURSEL, 0, 0);
      }

      // Populate mode combo
      HWND cmbMode = GetDlgItem(hDlg, IDC_PHONEMIZER_MODE);
      if (cmbMode) {
        SendMessageW(cmbMode, CB_ADDSTRING, 0, (LPARAM)L"Prefer STDIN (silent, recommended)");
        SendMessageW(cmbMode, CB_ADDSTRING, 0, (LPARAM)L"Command-line only");
        SendMessageW(cmbMode, CB_SETCURSEL, (st && st->preferStdin) ? 0 : 1, 0);
      }

      if (st) {
        SetDlgItemTextW(hDlg, IDC_PHONEMIZER_EXE, st->exePath.c_str());
        SetDlgItemTextW(hDlg, IDC_PHONEMIZER_ARGS_STDIN, st->argsStdin.c_str());
        SetDlgItemTextW(hDlg, IDC_PHONEMIZER_ARGS_CLI, st->argsCli.c_str());
        setDlgItemIntText(hDlg, IDC_PHONEMIZER_MAXCHUNK, st->maxChunkChars);

        // Auto-select a matching template if it looks like one.
        auto looksLike = [&](const PhonemizerTemplateItem& t) -> bool {
          if (!t.apply) return false;
          // exe path match: blank means "leave blank"
          if (t.exePath && wcslen(t.exePath) > 0) {
            if (_wcsicmp(st->exePath.c_str(), t.exePath) != 0) return false;
          } else {
            if (!st->exePath.empty()) return false;
          }
          if ((t.argsStdin ? t.argsStdin : L"") != st->argsStdin) return false;
          if ((t.argsCli ? t.argsCli : L"") != st->argsCli) return false;
          if (t.preferStdin != st->preferStdin) return false;
          return true;
        };

        const int count = static_cast<int>(sizeof(kPhonemizerTemplates) / sizeof(kPhonemizerTemplates[0]));
        int match = 0;
        for (int i = 1; i < count; ++i) {
          if (looksLike(kPhonemizerTemplates[i])) { match = i; break; }
        }
        SendDlgItemMessageW(hDlg, IDC_PHONEMIZER_TEMPLATE, CB_SETCURSEL, match, 0);
      }

      updatePhonemizerDialogEnableState(hDlg);
      return TRUE;
    }

    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      const int code = HIWORD(wParam);

      if (id == IDC_PHONEMIZER_BROWSE && code == BN_CLICKED) {
        std::wstring path;
        if (pickOpenExe(hDlg, path)) {
          SetDlgItemTextW(hDlg, IDC_PHONEMIZER_EXE, path.c_str());
          // Selecting browse implies custom.
          SendDlgItemMessageW(hDlg, IDC_PHONEMIZER_TEMPLATE, CB_SETCURSEL, 0, 0);
        }
        return TRUE;
      }

      if (id == IDC_PHONEMIZER_TEMPLATE && code == CBN_SELCHANGE) {
        int sel = static_cast<int>(SendDlgItemMessageW(hDlg, IDC_PHONEMIZER_TEMPLATE, CB_GETCURSEL, 0, 0));
        applyPhonemizerTemplate(hDlg, sel);
        updatePhonemizerDialogEnableState(hDlg);
        return TRUE;
      }

      if (id == IDC_PHONEMIZER_MODE && code == CBN_SELCHANGE) {
        updatePhonemizerDialogEnableState(hDlg);
        return TRUE;
      }

      if (id == IDOK && st) {
        st->exePath = getDlgItemTextAllocW(hDlg, IDC_PHONEMIZER_EXE);
        st->argsStdin = getDlgItemTextAllocW(hDlg, IDC_PHONEMIZER_ARGS_STDIN);
        st->argsCli = getDlgItemTextAllocW(hDlg, IDC_PHONEMIZER_ARGS_CLI);

        // Mode: 0 = prefer stdin, 1 = CLI only
        int mode = static_cast<int>(SendDlgItemMessageW(hDlg, IDC_PHONEMIZER_MODE, CB_GETCURSEL, 0, 0));
        st->preferStdin = (mode == 0);

        BOOL okNum = FALSE;
        UINT mc = GetDlgItemInt(hDlg, IDC_PHONEMIZER_MAXCHUNK, &okNum, FALSE);
        st->maxChunkChars = okNum ? static_cast<int>(mc) : 420;

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

bool ShowPhonemizerSettingsDialog(HINSTANCE hInst, HWND parent, PhonemizerSettingsDialogState& st) {
  st.ok = false;
  DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_PHONEMIZER_SETTINGS), parent, PhonemizerSettingsDlgProc, (LPARAM)&st);
  return st.ok;
}

// -------------------------------------------------------------------
// Class Editor Dialog
// -------------------------------------------------------------------

static INT_PTR CALLBACK ClassEditorDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
  ClassEditorDialogState* st = reinterpret_cast<ClassEditorDialogState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));

  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<ClassEditorDialogState*>(lParam);
      SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

      // Populate class list
      HWND list = GetDlgItem(hDlg, IDC_CE_CLASS_LIST);
      for (const auto& kv : st->classes) {
        std::wstring w = utf8ToWide(kv.first);
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
      }

      // Select first item if any
      if (!st->classes.empty()) {
        SendMessageW(list, LB_SETCURSEL, 0, 0);
        // Show members of first class
        auto it = st->classes.begin();
        SetDlgItemTextW(hDlg, IDC_CE_MEMBERS_EDIT, utf8ToWide(it->second).c_str());
        std::wstring label = L"Members of \"" + utf8ToWide(it->first) + L"\":";
        SetDlgItemTextW(hDlg, IDC_CE_MEMBERS_LABEL, label.c_str());
      }

      return TRUE;
    }

    case WM_COMMAND: {
      int id = LOWORD(wParam);
      int notif = HIWORD(wParam);

      if (id == IDC_CE_CLASS_LIST && notif == LBN_SELCHANGE && st) {
        // Save current class members before switching
        // First, find previously selected class
        static std::string lastClass;

        HWND list = GetDlgItem(hDlg, IDC_CE_CLASS_LIST);
        int sel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        if (sel == LB_ERR) return TRUE;

        // Get selected class name
        int len = static_cast<int>(SendMessageW(list, LB_GETTEXTLEN, sel, 0));
        std::wstring buf(len + 1, L'\0');
        SendMessageW(list, LB_GETTEXT, sel, reinterpret_cast<LPARAM>(&buf[0]));
        buf.resize(len);
        std::string className = wideToUtf8(buf);

        // Update members edit
        auto it = st->classes.find(className);
        if (it != st->classes.end()) {
          SetDlgItemTextW(hDlg, IDC_CE_MEMBERS_EDIT, utf8ToWide(it->second).c_str());
        } else {
          SetDlgItemTextW(hDlg, IDC_CE_MEMBERS_EDIT, L"");
        }

        // Update label
        std::wstring label = L"Members of \"" + utf8ToWide(className) + L"\":";
        SetDlgItemTextW(hDlg, IDC_CE_MEMBERS_LABEL, label.c_str());

        return TRUE;
      }

      if (id == IDC_CE_MEMBERS_EDIT && notif == EN_KILLFOCUS && st) {
        // Save members when edit loses focus
        HWND list = GetDlgItem(hDlg, IDC_CE_CLASS_LIST);
        int sel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        if (sel == LB_ERR) return TRUE;

        int len = static_cast<int>(SendMessageW(list, LB_GETTEXTLEN, sel, 0));
        std::wstring buf(len + 1, L'\0');
        SendMessageW(list, LB_GETTEXT, sel, reinterpret_cast<LPARAM>(&buf[0]));
        buf.resize(len);
        std::string className = wideToUtf8(buf);

        wchar_t membersBuf[4096];
        GetDlgItemTextW(hDlg, IDC_CE_MEMBERS_EDIT, membersBuf, 4096);
        st->classes[className] = wideToUtf8(membersBuf);

        return TRUE;
      }

      if (id == IDC_CE_CLASS_ADD && st) {
        // Prompt for new class name
        // For simplicity, use a simple input box approach
        // We'll reuse a pattern: ask user via a small modal or just add "NewClass" and let them rename

        // Generate unique name
        std::string baseName = "NEW_CLASS";
        std::string newName = baseName;
        int suffix = 1;
        while (st->classes.count(newName)) {
          newName = baseName + std::to_string(suffix++);
        }

        st->classes[newName] = "";

        HWND list = GetDlgItem(hDlg, IDC_CE_CLASS_LIST);
        int pos = static_cast<int>(SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(utf8ToWide(newName).c_str())));
        SendMessageW(list, LB_SETCURSEL, pos, 0);

        SetDlgItemTextW(hDlg, IDC_CE_MEMBERS_EDIT, L"");
        std::wstring label = L"Members of \"" + utf8ToWide(newName) + L"\":";
        SetDlgItemTextW(hDlg, IDC_CE_MEMBERS_LABEL, label.c_str());

        // Focus the members edit so user can type
        SetFocus(GetDlgItem(hDlg, IDC_CE_MEMBERS_EDIT));

        msgBox(hDlg, L"New class created. Edit the YAML directly to rename it, or add members now.", L"Class Editor", MB_ICONINFORMATION);

        return TRUE;
      }

      if (id == IDC_CE_CLASS_REMOVE && st) {
        HWND list = GetDlgItem(hDlg, IDC_CE_CLASS_LIST);
        int sel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        if (sel == LB_ERR) {
          msgBox(hDlg, L"Select a class to remove.", L"Class Editor", MB_ICONWARNING);
          return TRUE;
        }

        int len = static_cast<int>(SendMessageW(list, LB_GETTEXTLEN, sel, 0));
        std::wstring buf(len + 1, L'\0');
        SendMessageW(list, LB_GETTEXT, sel, reinterpret_cast<LPARAM>(&buf[0]));
        buf.resize(len);
        std::string className = wideToUtf8(buf);

        st->classes.erase(className);
        SendMessageW(list, LB_DELETESTRING, sel, 0);

        // Select next or previous item
        int count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
        if (count > 0) {
          if (sel >= count) sel = count - 1;
          SendMessageW(list, LB_SETCURSEL, sel, 0);
          // Trigger selection change
          SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDC_CE_CLASS_LIST, LBN_SELCHANGE), (LPARAM)list);
        } else {
          SetDlgItemTextW(hDlg, IDC_CE_MEMBERS_EDIT, L"");
          SetDlgItemTextW(hDlg, IDC_CE_MEMBERS_LABEL, L"Members (IPA characters):");
        }

        return TRUE;
      }

      if (id == IDOK && st) {
        // Save current selection's members before closing
        HWND list = GetDlgItem(hDlg, IDC_CE_CLASS_LIST);
        int sel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        if (sel != LB_ERR) {
          int len = static_cast<int>(SendMessageW(list, LB_GETTEXTLEN, sel, 0));
          std::wstring buf(len + 1, L'\0');
          SendMessageW(list, LB_GETTEXT, sel, reinterpret_cast<LPARAM>(&buf[0]));
          buf.resize(len);
          std::string className = wideToUtf8(buf);

          wchar_t membersBuf[4096];
          GetDlgItemTextW(hDlg, IDC_CE_MEMBERS_EDIT, membersBuf, 4096);
          st->classes[className] = wideToUtf8(membersBuf);
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

bool ShowClassEditorDialog(HINSTANCE hInst, HWND parent, ClassEditorDialogState& st) {
  st.ok = false;
  DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_CLASS_EDITOR), parent, ClassEditorDlgProc, (LPARAM)&st);
  return st.ok;
}
