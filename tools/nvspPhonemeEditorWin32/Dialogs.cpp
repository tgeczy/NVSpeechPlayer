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
#include <vector>

static constexpr int kSampleRate = 22050;

using nvsp_editor::NvspRuntime;

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

      comboAddNone(before);
      comboAddNone(after);

      int idxBefore = 0;
      int idxAfter = 0;

      for (size_t i = 0; i < st->classNames.size(); ++i) {
        std::wstring w = utf8ToWide(st->classNames[i]);
        int posB = static_cast<int>(SendMessageW(before, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str())));
        int posA = static_cast<int>(SendMessageW(after, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str())));
        if (!st->rule.when.beforeClass.empty() && st->classNames[i] == st->rule.when.beforeClass) idxBefore = posB;
        if (!st->rule.when.afterClass.empty() && st->classNames[i] == st->rule.when.afterClass) idxAfter = posA;
      }

      SendMessageW(before, CB_SETCURSEL, idxBefore, 0);
      SendMessageW(after, CB_SETCURSEL, idxAfter, 0);

      return TRUE;
    }

    case WM_COMMAND: {
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

    nvsp_editor::Node tmp = st->baseMap;
    auto it = tmp.map.find(st->field);
    if (it == tmp.map.end()) {
      // If missing, create it.
      tmp.map[st->field] = nvsp_editor::Node{};
      it = tmp.map.find(st->field);
    }
    it->second.type = nvsp_editor::Node::Type::Scalar;
    it->second.scalar = st->value;

    std::vector<sample> samples;
    std::string err;
    if (!st->runtime->synthPreviewPhoneme(tmp, kSampleRate, samples, err)) {
      return; // silent on preview errors
    }
    if (samples.empty()) return;

    std::wstring wavPath = nvsp_editor::makeTempWavPath(L"nvpe");
    if (!nvsp_editor::writeWav16Mono(wavPath, kSampleRate, samples, err)) {
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
        installAccessibleNameForListView(lv, L"Language settings list");
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

static std::vector<std::string> sortedNodeKeys(const nvsp_editor::Node& n) {
  std::vector<std::string> keys;
  if (!n.isMap()) return keys;
  keys.reserve(n.map.size());
  for (const auto& kv : n.map) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  return keys;
}

static void populatePhonemeFieldsList(HWND lv, const nvsp_editor::Node& phonemeMap) {
  ListView_DeleteAllItems(lv);
  auto keys = sortedNodeKeys(phonemeMap);

  int row = 0;
  for (const auto& k : keys) {
    const auto& v = phonemeMap.map.at(k);
    if (!v.isScalar()) continue;

    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.iSubItem = 0;
    std::wstring wk = formatFieldLabelW(k);
    item.pszText = wk.data();
    ListView_InsertItem(lv, &item);

    std::wstring wv = utf8ToWide(v.scalar);
    ListView_SetItemText(lv, row, 1, const_cast<wchar_t*>(wv.c_str()));

    row++;
  }
}

static std::string getSelectedField(HWND lv, const nvsp_editor::Node& phonemeMap) {
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
      if (lv) installAccessibleNameForListView(lv, L"Phoneme fields list");
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

        auto it = st->working.map.find(field);
        if (it == st->working.map.end() || !it->second.isScalar()) {
          msgBox(hDlg, L"That field isn't a scalar value.", L"Edit phoneme", MB_ICONERROR);
          return TRUE;
        }

        EditValueDialogState vs;
        vs.field = field;
        vs.value = it->second.scalar;
        vs.baseMap = st->working;
        vs.runtime = st->runtime;
        vs.livePreview = true;

        DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_EDIT_VALUE), hDlg, EditValueDlgProc, reinterpret_cast<LPARAM>(&vs));
        if (vs.ok) {
          it->second.type = nvsp_editor::Node::Type::Scalar;
          it->second.scalar = vs.value;
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
nvsp_editor::SpeechSettings loadSpeechSettingsFromIni() {
  nvsp_editor::SpeechSettings s;
  s.voiceName = wideToUtf8(readIni(L"speech", L"voice", L"Adam"));
  s.rate = readIniInt(L"speech", L"rate", s.rate);
  s.pitch = readIniInt(L"speech", L"pitch", s.pitch);
  s.volume = readIniInt(L"speech", L"volume", s.volume);
  s.inflection = readIniInt(L"speech", L"inflection", s.inflection);
  s.pauseMode = wideToUtf8(readIni(L"speech", L"pauseMode", L"short"));

  const auto& names = NvspRuntime::frameParamNames();
  s.frameParams.assign(names.size(), 50);
  for (size_t i = 0; i < names.size(); ++i) {
    std::wstring key = L"frame_" + utf8ToWide(names[i]);
    s.frameParams[i] = readIniInt(L"speech", key.c_str(), 50);
  }
  return s;
}

void saveSpeechSettingsToIni(const nvsp_editor::SpeechSettings& s) {
  writeIni(L"speech", L"voice", utf8ToWide(s.voiceName));
  writeIniInt(L"speech", L"rate", s.rate);
  writeIniInt(L"speech", L"pitch", s.pitch);
  writeIniInt(L"speech", L"volume", s.volume);
  writeIniInt(L"speech", L"inflection", s.inflection);
  writeIni(L"speech", L"pauseMode", utf8ToWide(s.pauseMode));

  const auto& names = NvspRuntime::frameParamNames();
  for (size_t i = 0; i < names.size() && i < s.frameParams.size(); ++i) {
    std::wstring key = L"frame_" + utf8ToWide(names[i]);
    writeIniInt(L"speech", key.c_str(), s.frameParams[i]);
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

static void fillVoices(HWND combo, const std::string& selected) {
  if (!combo) return;
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  const char* voices[] = {"Adam", "Benjamin", "Caleb", "David"};
  int sel = 0;
  for (int i = 0; i < 4; ++i) {
    std::wstring w = utf8ToWide(voices[i]);
    int idx = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str())));
    if (selected == voices[i]) sel = idx;
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

  switch (msg) {
    case WM_INITDIALOG: {
      st = reinterpret_cast<SpeechSettingsDialogState*>(lParam);
      SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

      // Accessible names for any ListView controls (none here), and predictable defaults.
      HWND combo = GetDlgItem(hDlg, IDC_SPEECH_VOICE);
      fillVoices(combo, st->settings.voiceName);

      setTrackbarRangeAndPos(GetDlgItem(hDlg, IDC_SPEECH_RATE_SLIDER), st->settings.rate);
      setDlgIntText(hDlg, IDC_SPEECH_RATE_VAL, st->settings.rate);

      setTrackbarRangeAndPos(GetDlgItem(hDlg, IDC_SPEECH_PITCH_SLIDER), st->settings.pitch);
      setDlgIntText(hDlg, IDC_SPEECH_PITCH_VAL, st->settings.pitch);

      setTrackbarRangeAndPos(GetDlgItem(hDlg, IDC_SPEECH_VOLUME_SLIDER), st->settings.volume);
      setDlgIntText(hDlg, IDC_SPEECH_VOLUME_VAL, st->settings.volume);

      setTrackbarRangeAndPos(GetDlgItem(hDlg, IDC_SPEECH_INFLECTION_SLIDER), st->settings.inflection);
      setDlgIntText(hDlg, IDC_SPEECH_INFLECTION_VAL, st->settings.inflection);

      // Param list
      HWND lb = GetDlgItem(hDlg, IDC_SPEECH_PARAM_LIST);
      populateParamList(lb, st->paramNames, st->settings.frameParams);
      syncSelectedParamToUi();
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
          st->settings.voiceName = wideToUtf8(buf);
        }
        return TRUE;
      }

      if (id == IDC_SPEECH_PARAM_LIST && code == LBN_SELCHANGE) {
        syncSelectedParamToUi();
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

      if (id == IDC_SPEECH_RESET_ALL) {
        st->settings.frameParams.assign(st->paramNames.size(), 50);
        st->settings.voiceName = st->settings.voiceName.empty() ? "Adam" : st->settings.voiceName;
        HWND lb = GetDlgItem(hDlg, IDC_SPEECH_PARAM_LIST);
        populateParamList(lb, st->paramNames, st->settings.frameParams);
        syncSelectedParamToUi();
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
