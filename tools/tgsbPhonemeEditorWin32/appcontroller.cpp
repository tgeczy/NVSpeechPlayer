/*
TGSpeechBox — Phoneme editor main application controller.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#define UNICODE
#define _UNICODE

#include "AppController.h"

#include "AccessibilityUtils.h"
#include "Dialogs.h"
#include "DictionaryEditor.h"
#include "VoiceProfileEditor.h"
#include "RulesEditor.h"
#include "WinUtils.h"

#include "process_util.h"
#include "phonemizer_cli.h"
#include "resource.h"
#include "utf8.h"
#include "wav_writer.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

// yaml_edit.* and tgsb_runtime.* live in the tgsb_editor namespace.
using tgsb_editor::Node;
using tgsb_editor::ReplacementRule;
using tgsb_editor::ReplacementWhen;
using tgsb_editor::SkipRule;
using tgsb_editor::TgsbRuntime;

static constexpr int kSampleRate = 22050;

static bool handleTabNavigation(HWND hWnd, const MSG& msg);
static bool handleCtrlASelectAll(HWND hWnd, const MSG& msg);
static bool handleAltShortcuts(HWND hWnd, const MSG& msg);

LRESULT CALLBACK AppController::StaticWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
    auto* self = reinterpret_cast<AppController*>(cs->lpCreateParams);
    if (self) {
      self->wnd = hWnd;
      SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
  }

  auto* self = reinterpret_cast<AppController*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
  if (self) return self->HandleMessage(hWnd, msg, wParam, lParam);
  return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool AppController::Initialize(HINSTANCE hInstance, int nCmdShow) {
  hInst = hInstance;

  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(hr)) {
    msgBox(nullptr, L"Failed to initialize COM.", L"Error", MB_OK | MB_ICONERROR);
    return false;
  }

  INITCOMMONCONTROLSEX icc{};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES;
  InitCommonControlsEx(&icc);

  // No accelerator table is bundled with this lightweight tool.
  // (If one is added later, wire it up here.)
  accel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDR_ACCEL));

  WNDCLASSW wc{};
  wc.lpfnWndProc = AppController::StaticWndProc;
  wc.hInstance = hInstance;
  wc.lpszClassName = L"TgsbPhonemeEditorWnd";
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

  RegisterClassW(&wc);

  HMENU hMenu = LoadMenuW(hInstance, MAKEINTRESOURCEW(IDR_MAINMENU));

  wnd = CreateWindowExW(
    0,
    wc.lpszClassName,
    L"TGSpeechBox - Phoneme Editor",
    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
    CW_USEDEFAULT,
    CW_USEDEFAULT,
    1050,
    780,
    nullptr,
    hMenu,
    hInstance,
    this
  );

  if (!wnd) {
    msgBox(nullptr, L"Failed to create main window.", L"Error", MB_OK | MB_ICONERROR);
    CoUninitialize();
    return false;
  }

  ShowWindow(wnd, nCmdShow);
  UpdateWindow(wnd);
  return true;
}

int AppController::RunMessageLoop() {
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
      if (handleTabNavigation(wnd, msg)) continue;
      if (handleCtrlASelectAll(wnd, msg)) continue;
      if (handleAltShortcuts(wnd, msg)) continue;
    }

    if (accel && TranslateAcceleratorW(wnd, accel, &msg)) continue;

    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  if (accel) {
    DestroyAcceleratorTable(accel);
    accel = nullptr;
  }

  CoUninitialize();
  return 0;
}




// -------------------------
// UI list helpers
// -------------------------
static void lvClear(HWND lv) {
  ListView_DeleteAllItems(lv);
}

static void lvAddColumn(HWND lv, int idx, const wchar_t* text, int width) {
  LVCOLUMNW col{};
  col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
  col.pszText = const_cast<wchar_t*>(text);
  col.cx = width;
  col.iSubItem = idx;
  ListView_InsertColumn(lv, idx, &col);
}

static void lvAddRow2(HWND lv, int row, const std::wstring& c1, const std::wstring& c2) {
  LVITEMW it{};
  it.mask = LVIF_TEXT;
  it.iItem = row;
  it.iSubItem = 0;
  it.pszText = const_cast<wchar_t*>(c1.c_str());
  ListView_InsertItem(lv, &it);
  ListView_SetItemText(lv, row, 1, const_cast<wchar_t*>(c2.c_str()));
}

static void lvAddRow3(HWND lv, int row, const std::wstring& c1, const std::wstring& c2, const std::wstring& c3) {
  LVITEMW it{};
  it.mask = LVIF_TEXT;
  it.iItem = row;
  it.iSubItem = 0;
  it.pszText = const_cast<wchar_t*>(c1.c_str());
  ListView_InsertItem(lv, &it);
  ListView_SetItemText(lv, row, 1, const_cast<wchar_t*>(c2.c_str()));
  ListView_SetItemText(lv, row, 2, const_cast<wchar_t*>(c3.c_str()));
}

static int lvSelectedIndex(HWND lv) {
  return ListView_GetNextItem(lv, -1, LVNI_SELECTED);
}


static std::string lvGetTextUtf8(HWND lv, int row, int col) {
  wchar_t buf[512];
  ListView_GetItemText(lv, row, col, buf, 512);
  return wideToUtf8(buf);
}

static std::string getSelectedPhonemeKey(HWND lv) {
  int sel = lvSelectedIndex(lv);
  if (sel < 0) return {};
  return lvGetTextUtf8(lv, sel, 0);
}

// -------------------------
// Data -> UI
// -------------------------
static void rebuildPhonemeKeysU32(AppController& app) {
  app.phonemeKeysU32Sorted.clear();
  app.phonemeKeysU32Sorted.reserve(app.phonemeKeys.size());
  for (const auto& k : app.phonemeKeys) {
    app.phonemeKeysU32Sorted.push_back(nvsp_frontend::utf8ToU32(k));
  }
  std::sort(app.phonemeKeysU32Sorted.begin(), app.phonemeKeysU32Sorted.end(), [](const auto& a, const auto& b) {
    if (a.size() != b.size()) return a.size() > b.size();
    return a < b;
  });
}

static std::vector<std::string> extractUsedPhonemes(const AppController& app, const std::vector<ReplacementRule>& repls) {
  std::unordered_set<std::string> used;

  for (const auto& r : repls) {
    std::u32string text = nvsp_frontend::utf8ToU32(r.to);
    size_t i = 0;
    while (i < text.size()) {
      char32_t c = text[i];
      if (c == U' ' || c == U'\t' || c == U'\n' || c == U'\r') {
        i++;
        continue;
      }

      bool matched = false;
      for (const auto& key : app.phonemeKeysU32Sorted) {
        if (key.empty()) continue;
        if (i + key.size() <= text.size() && std::equal(key.begin(), key.end(), text.begin() + static_cast<long long>(i))) {
          used.insert(nvsp_frontend::u32ToUtf8(key));
          i += key.size();
          matched = true;
          break;
        }
      }
      if (!matched) i++;
    }
  }

  std::vector<std::string> out(used.begin(), used.end());
  std::sort(out.begin(), out.end());
  return out;
}

static std::wstring whenToText(const ReplacementWhen& w) {
  std::wstring out;
  auto add = [&](const std::wstring& s) {
    if (!out.empty()) out += L", ";
    out += s;
  };
  if (w.atWordStart) add(L"wordStart");
  if (w.atWordEnd) add(L"wordEnd");
  if (!w.beforeClass.empty()) add(L"before=" + utf8ToWide(w.beforeClass));
  if (!w.afterClass.empty()) add(L"after=" + utf8ToWide(w.afterClass));
  return out;
}

static void populatePhonemeList(AppController& app, const std::wstring& filter) {
  app.filteredPhonemeKeys.clear();

  std::string filterUtf8 = wideToUtf8(filter);
  std::string filterLower;
  filterLower.reserve(filterUtf8.size());
  for (unsigned char c : filterUtf8) filterLower.push_back(static_cast<char>(std::tolower(c)));

  for (const auto& k : app.phonemeKeys) {
    if (filterLower.empty()) {
      app.filteredPhonemeKeys.push_back(k);
      continue;
    }
    std::string kl = k;
    for (auto& ch : kl) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (kl.find(filterLower) != std::string::npos) {
      app.filteredPhonemeKeys.push_back(k);
    }
  }

  lvClear(app.listPhonemes);
  int row = 0;
  for (const auto& k : app.filteredPhonemeKeys) {
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.iSubItem = 0;
    std::wstring wk = utf8ToWide(k);
    it.pszText = wk.data();
    ListView_InsertItem(app.listPhonemes, &it);
    row++;
  }

  EnsureListViewHasSelection(app.listPhonemes);
}

static void populateMappingsList(AppController& app) {
  lvClear(app.listMappings);
  int row = 0;
  for (const auto& r : app.repls) {
    lvAddRow3(app.listMappings, row, utf8ToWide(r.from), utf8ToWide(r.to), whenToText(r.when));
    row++;
  }

  EnsureListViewHasSelection(app.listMappings);
}

static void populateLanguagePhonemesList(AppController& app) {
  lvClear(app.listLangPhonemes);
  int row = 0;
  for (const auto& k : app.usedPhonemeKeys) {
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.iSubItem = 0;
    std::wstring wk = utf8ToWide(k);
    it.pszText = wk.data();
    ListView_InsertItem(app.listLangPhonemes, &it);
    row++;
  }

  EnsureListViewHasSelection(app.listLangPhonemes);
}

static void populateSkipList(AppController& app) {
  lvClear(app.listSkip);
  int row = 0;
  for (const auto& sr : app.skipRepls) {
    std::wstring toW = sr.to.empty() ? L"(any)" : utf8ToWide(sr.to);
    lvAddRow2(app.listSkip, row, utf8ToWide(sr.from), toW);
    row++;
  }
  EnsureListViewHasSelection(app.listSkip);
}

static void refreshLanguageDerivedLists(AppController& app) {
  app.usedPhonemeKeys = extractUsedPhonemes(app, app.repls);
  populateMappingsList(app);
  populateSkipList(app);
  populateLanguagePhonemesList(app);
}

// -------------------------
// Load packs
// -------------------------
static bool maybeCopyGoodPhonemesToExpected(HWND owner, const std::wstring& packsDir) {
  fs::path phonemes = fs::path(packsDir) / "phonemes.yaml";
  if (fs::exists(phonemes)) return true;

  fs::path good = fs::path(packsDir) / "phonemes-good.yaml";
  if (!fs::exists(good)) return false;

  int res = MessageBoxW(
    owner,
    L"packs/phonemes.yaml was not found, but packs/phonemes-good.yaml exists.\n\n"
    L"nvspFrontend.dll expects packs/phonemes.yaml.\n\n"
    L"Create a copy now?",
    L"TGSB Phoneme Editor",
    MB_YESNO | MB_ICONQUESTION
  );

  if (res != IDYES) return true; // allow editor to still work

  try {
    fs::copy_file(good, phonemes, fs::copy_options::overwrite_existing);
    return true;
  } catch (...) {
    msgBox(owner, L"Failed to copy phonemes-good.yaml to phonemes.yaml.", L"TGSB Phoneme Editor", MB_ICONERROR);
    return false;
  }
}

static bool loadPhonemes(AppController& app, const std::wstring& packsDir) {
  // Prefer packs/phonemes.yaml; fallback to packs/phonemes-good.yaml.
  fs::path p1 = fs::path(packsDir) / "phonemes.yaml";
  fs::path p2 = fs::path(packsDir) / "phonemes-good.yaml";

  fs::path use;
  if (fs::exists(p1)) use = p1;
  else if (fs::exists(p2)) use = p2;
  else return false;

  std::string err;
  if (!app.phonemes.load(use.u8string(), err)) {
    msgBox(app.wnd, L"Failed to load phonemes YAML:\n" + utf8ToWide(err), L"TGSB Phoneme Editor", MB_ICONERROR);
    return false;
  }

  app.phonemesPath = use.wstring();
  app.phonemeKeys = app.phonemes.phonemeKeysSorted();
  app.phonemesDirty = false;
  rebuildPhonemeKeysU32(app);

  std::wstring filter;
  wchar_t buf[512];
  GetWindowTextW(app.editFilter, buf, 512);
  filter = buf;
  populatePhonemeList(app, filter);

  return true;
}

static void populateLanguageCombo(AppController& app) {
  SendMessageW(app.comboLang, CB_RESETCONTENT, 0, 0);
  app.languageFiles.clear();

  fs::path dir(app.langDir);
  if (!fs::exists(dir)) return;

  std::vector<fs::path> files;
  for (auto& e : fs::directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    auto p = e.path();
    if (p.extension() == ".yaml" || p.extension() == ".yml") {
      files.push_back(p);
    }
  }
  std::sort(files.begin(), files.end());

  int i = 0;
  for (const auto& p : files) {
    std::wstring name = p.filename().wstring();
    SendMessageW(app.comboLang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    app.languageFiles.push_back(p.wstring());
    i++;
  }

  // Try to restore previous.
  std::wstring last = readIni(L"state", L"lastLanguage", L"");
  int sel = 0;
  if (!last.empty()) {
    for (size_t idx = 0; idx < app.languageFiles.size(); ++idx) {
      if (fs::path(app.languageFiles[idx]).filename().wstring() == fs::path(last).filename().wstring()) {
        sel = static_cast<int>(idx);
        break;
      }
    }
  }

  SendMessageW(app.comboLang, CB_SETCURSEL, sel, 0);
}

static std::string selectedLangTagUtf8(const AppController& app) {
  int sel = static_cast<int>(SendMessageW(app.comboLang, CB_GETCURSEL, 0, 0));
  if (sel < 0 || sel >= static_cast<int>(app.languageFiles.size())) return {};
  fs::path p(app.languageFiles[static_cast<size_t>(sel)]);
  std::string stem = p.stem().u8string();
  return stem; // keep as-is; nvspFrontend normalizes internally
}

static bool loadLanguage(AppController& app, const std::wstring& langPath) {
  std::string err;
  if (!app.language.load(fs::path(langPath).u8string(), err)) {
    msgBox(app.wnd, L"Failed to load language YAML:\n" + utf8ToWide(err), L"TGSB Phoneme Editor", MB_ICONERROR);
    return false;
  }

  app.repls = app.language.replacements();
  app.skipRepls = app.language.skipReplacements();
  app.classNames = app.language.classNamesSorted();
  app.languageDirty = false;

  refreshLanguageDerivedLists(app);

  // Update runtime language for TTS.
  std::string langTag = selectedLangTagUtf8(app);
  if (!langTag.empty() && app.runtime.dllsLoaded() && !app.packRoot.empty()) {
    std::string rtErr;
    app.runtime.setLanguage(langTag, rtErr);
    if (!rtErr.empty()) {
      // Soft error; don't block editing.
      app.setStatus(L"TTS warning: " + utf8ToWide(rtErr));
    }
  }

  writeIni(L"state", L"lastLanguage", fs::path(langPath).filename().wstring());

  return true;
}

static std::wstring runtimePackDir(const AppController& app) {
  if (!app.packsDir.empty()) return app.packsDir;
  if (!app.packRoot.empty()) {
    fs::path p(app.packRoot);
    p /= "packs";
    return p.wstring();
  }
  return {};
}

static bool loadPackRoot(AppController& app, const std::wstring& root) {
  if (root.empty()) return false;

  fs::path rootPath(root);
  fs::path packs = rootPath / "packs";
  if (!fs::exists(packs) || !fs::is_directory(packs)) {
    msgBox(app.wnd, L"That folder doesn't contain a 'packs' subfolder.", L"TGSB Phoneme Editor", MB_ICONERROR);
    return false;
  }

  app.packRoot = root;
  app.packsDir = packs.wstring();
  app.langDir = (packs / "lang").wstring();

  maybeCopyGoodPhonemesToExpected(app.wnd, app.packsDir);

  if (!loadPhonemes(app, app.packsDir)) {
    msgBox(app.wnd, L"Couldn't find phonemes.yaml or phonemes-good.yaml under packs/.", L"TGSB Phoneme Editor", MB_ICONERROR);
    return false;
  }

  populateLanguageCombo(app);

  // Load selected language.
  int sel = static_cast<int>(SendMessageW(app.comboLang, CB_GETCURSEL, 0, 0));
  if (sel >= 0 && sel < static_cast<int>(app.languageFiles.size())) {
    loadLanguage(app, app.languageFiles[static_cast<size_t>(sel)]);
  }

  // Point runtime at pack root.
  if (app.runtime.dllsLoaded()) {
    std::string rtErr;
    app.runtime.setPackRoot(runtimePackDir(app), rtErr);
  }

  writeIni(L"state", L"packRoot", app.packRoot);

  app.setStatus(L"Loaded packs from: " + app.packRoot);
  return true;
}

// -------------------------
// Audio actions
// -------------------------
static bool ensureDllDir(AppController& app) {
  if (app.runtime.dllsLoaded()) return true;

  std::wstring dllDir = app.dllDir;
  if (dllDir.empty()) {
    dllDir = readIni(L"paths", L"dllDir", L"");
  }
  if (dllDir.empty()) {
    // Try exe directory.
    dllDir = exeDir();
  }

  std::string err;
  if (!app.runtime.setDllDirectory(dllDir, err)) {
    msgBox(app.wnd, L"DLL load failed:\n" + utf8ToWide(err) + L"\n\nUse Settings > Set DLL directory...", L"TGSB Phoneme Editor", MB_ICONERROR);
    return false;
  }

  app.dllDir = dllDir;
  writeIni(L"paths", L"dllDir", app.dllDir);

  // Also set pack root on runtime.
  if (!app.packRoot.empty()) {
    std::string tmp;
    app.runtime.setPackRoot(runtimePackDir(app), tmp);
    std::string tmp2;
    std::string langTag = selectedLangTagUtf8(app);
    if (!langTag.empty()) app.runtime.setLanguage(langTag, tmp2);
  }

  return true;
}

static void playSamplesTemp(AppController& app, const std::vector<sample>& samples) {
  if (samples.empty()) {
    msgBox(app.wnd, L"No audio samples were generated.", L"TGSB Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  std::wstring wavPath = tgsb_editor::makeTempWavPath(L"nvp");
  std::string err;
  if (!tgsb_editor::writeWav16Mono(wavPath, kSampleRate, samples, err)) {
    msgBox(app.wnd, L"WAV write failed:\n" + utf8ToWide(err), L"TGSB Phoneme Editor", MB_ICONERROR);
    return;
  }

  PlaySoundW(wavPath.c_str(), NULL, SND_FILENAME | SND_ASYNC);
}

static void onPlaySelectedPhoneme(AppController& app, bool fromLanguageList) {
  if (!ensureDllDir(app)) return;

  std::string key = fromLanguageList ? getSelectedPhonemeKey(app.listLangPhonemes) : getSelectedPhonemeKey(app.listPhonemes);
  if (key.empty()) {
    msgBox(app.wnd, L"Select a phoneme first.", L"TGSB Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  tgsb_editor::Node* node = app.phonemes.getPhonemeNode(key);
  if (!node || !node->isMap()) {
    msgBox(app.wnd, L"Phoneme not found in phonemes.yaml.", L"TGSB Phoneme Editor", MB_ICONERROR);
    return;
  }

  std::vector<sample> samples;
  std::string err;
  if (!app.runtime.synthPreviewPhoneme(*node, kSampleRate, samples, err)) {
    msgBox(app.wnd, L"Preview failed:\n" + utf8ToWide(err), L"TGSB Phoneme Editor", MB_ICONERROR);
    return;
  }

  playSamplesTemp(app, samples);
}

// -------------------------
// Mapping operations
// -------------------------
static void onAddMapping(AppController& app, const std::string& defaultTo = {}) {
  if (!app.language.isLoaded()) {
    msgBox(app.wnd, L"Load a language first.", L"TGSB Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  AddMappingDialogState st;
  st.rule.to = defaultTo;
  st.classNames = app.classNames;
  st.language = &app.language;

  ShowAddMappingDialog(app.hInst, app.wnd, st);

  // Refresh classNames in case classes were edited in dialog
  if (st.classNames != app.classNames) {
    app.classNames = st.classNames;
    app.languageDirty = true;
  }

  if (!st.ok) return;

  app.repls.push_back(st.rule);
  app.language.setReplacements(app.repls);
  app.languageDirty = true;
  refreshLanguageDerivedLists(app);
}

static void onEditSelectedMapping(AppController& app) {
  int sel = lvSelectedIndex(app.listMappings);
  if (sel < 0 || sel >= static_cast<int>(app.repls.size())) {
    msgBox(app.wnd, L"Select a mapping first.", L"TGSB Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  AddMappingDialogState st;
  st.rule = app.repls[static_cast<size_t>(sel)];
  st.classNames = app.classNames;
  st.language = &app.language;

  ShowAddMappingDialog(app.hInst, app.wnd, st);

  // Refresh classNames in case classes were edited in dialog
  if (st.classNames != app.classNames) {
    app.classNames = st.classNames;
    app.languageDirty = true;
  }

  if (!st.ok) return;

  app.repls[static_cast<size_t>(sel)] = st.rule;
  app.language.setReplacements(app.repls);
  app.languageDirty = true;
  refreshLanguageDerivedLists(app);
}

static void onRemoveSelectedMapping(AppController& app) {
  int sel = lvSelectedIndex(app.listMappings);
  if (sel < 0 || sel >= static_cast<int>(app.repls.size())) {
    msgBox(app.wnd, L"Select a mapping first.", L"TGSB Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  app.repls.erase(app.repls.begin() + sel);
  app.language.setReplacements(app.repls);
  app.languageDirty = true;
  refreshLanguageDerivedLists(app);
}

// -------------------------
// Skip rule handlers
// -------------------------
static void onAddSkipRule(AppController& app) {
  if (!app.language.isLoaded()) return;

  // Reuse the mapping dialog — From/To fields are all we need; 'when' is ignored.
  AddMappingDialogState st;
  st.classNames = app.classNames;
  st.language = &app.language;
  ShowAddMappingDialog(app.hInst, app.wnd, st);
  if (!st.ok) return;

  SkipRule sr;
  sr.from = st.rule.from;
  sr.to = st.rule.to;
  app.skipRepls.push_back(std::move(sr));
  app.language.setSkipReplacements(app.skipRepls);
  app.languageDirty = true;
  populateSkipList(app);
}

static void onRemoveSelectedSkipRule(AppController& app) {
  int sel = lvSelectedIndex(app.listSkip);
  if (sel < 0 || sel >= static_cast<int>(app.skipRepls.size())) {
    msgBox(app.wnd, L"Select a skip rule first.", L"TGSB Phoneme Editor", MB_ICONINFORMATION);
    return;
  }
  app.skipRepls.erase(app.skipRepls.begin() + sel);
  app.language.setSkipReplacements(app.skipRepls);
  app.languageDirty = true;
  populateSkipList(app);
}

// -------------------------
// Language settings
// -------------------------
static std::vector<std::string> knownLanguageSettingKeys() {
  return {
    // Alphabetically sorted list of all language pack settings
    "allophoneRulesEnabled",
    "applyLengthenedScaleToVowelsOnly",
    "autoDiphthongOffglideToSemivowel",
    "autoTieDiphthongs",
    "boundarySmoothingEnabled",
    "boundarySmoothingF1Scale",
    "boundarySmoothingF2Scale",
    "boundarySmoothingF3Scale",
    "boundarySmoothingNasalF1Instant",
    "boundarySmoothingNasalF2F3SpansPhone",
    "boundarySmoothingFricToStopFadeMs",
    "boundarySmoothingFricToVowelFadeMs",
    "boundarySmoothingLiquidToStopFadeMs",
    "boundarySmoothingLiquidToVowelFadeMs",
    "boundarySmoothingNasalToStopFadeMs",
    "boundarySmoothingNasalToVowelFadeMs",
    "boundarySmoothingPlosiveSpansPhone",
    "boundarySmoothingStopToFricFadeMs",
    "boundarySmoothingStopToVowelFadeMs",
    "boundarySmoothingVowelToFricFadeMs",
    "boundarySmoothingVowelToLiquidFadeMs",
    "boundarySmoothingVowelToNasalFadeMs",
    "boundarySmoothingVowelToStopFadeMs",
    "boundarySmoothingVowelToVowelFadeMs",
    "boundarySmoothingWithinSyllableFadeScale",
    "boundarySmoothingWithinSyllableScale",
    "boundarySmoothingHighRateFadeRatioFloor",
    "boundarySmoothingAffricateToVowelHold",
    "boundarySmoothingFricativeToVowelHold",
    "boundarySmoothingStopToVowelHold",
    "boundarySmoothingAffricateToVowelVoicingHold",
    "boundarySmoothingStopToVowelVoicingHold",
    "boundarySmoothingAlveolarF1Scale",
    "boundarySmoothingAlveolarF2Scale",
    "boundarySmoothingAlveolarF3Scale",
    "boundarySmoothingLabialF1Scale",
    "boundarySmoothingLabialF2Scale",
    "boundarySmoothingLabialF3Scale",
    "boundarySmoothingPalatalF1Scale",
    "boundarySmoothingPalatalF2Scale",
    "boundarySmoothingPalatalF3Scale",
    "boundarySmoothingVelarF1Scale",
    "boundarySmoothingVelarF2Scale",
    "boundarySmoothingVelarF3Scale",
    "clusterBlendDefaultPairScale",
    "clusterBlendEnabled",
    "clusterBlendF1Scale",
    "clusterBlendForwardDriftStrength",
    "clusterBlendFricToFricScale",
    "clusterBlendFricToStopScale",
    "clusterBlendHomorganicScale",
    "clusterBlendLiquidToFricScale",
    "clusterBlendLiquidToStopScale",
    "clusterBlendNasalToFricScale",
    "clusterBlendNasalToStopScale",
    "clusterBlendStopToFricScale",
    "clusterBlendStopToStopScale",
    "clusterBlendStrength",
    "clusterBlendWordBoundaryScale",
    "clusterTimingAffricateInClusterScale",
    "clusterTimingEnabled",
    "clusterTimingFricBeforeFricScale",
    "clusterTimingFricBeforeStopScale",
    "clusterTimingStopBeforeFricScale",
    "clusterTimingStopBeforeStopScale",
    "clusterTimingTripleClusterMiddleScale",
    "clusterTimingWordFinalObstruentScale",
    "clusterTimingWordMedialConsonantScale",
    "clauseFinalFadeMs",
    "clauseFinalHoldMs",
    "codaNoiseTaperEarlyAspAmp",
    "codaNoiseTaperEarlyFricScale",
    "codaNoiseTaperEnabled",
    "codaNoiseTaperLateAspAmp",
    "codaNoiseTaperLateFricScale",
    "codaNoiseTaperPreGain",
    "coarticulationAdjacencyMaxConsonants",
    "coarticulationAlveolarF2Locus",
    "coarticulationAlveolarScale",
    "coarticulationAspirationBlendEnd",
    "coarticulationAspirationBlendStart",
    "coarticulationEnabled",
    "coarticulationF1Scale",
    "coarticulationF2Scale",
    "coarticulationF3Scale",
    "coarticulationGraduated",
    "coarticulationLabialF2Locus",
    "coarticulationLabialScale",
    "coarticulationMitalkK",
    "coarticulationPalatalScale",
    "coarticulationStrength",
    "coarticulationVelarF2Locus",
    "coarticulationVelarF2LocusFront",
    "coarticulationVelarF2LocusBack",
    "coarticulationVelarPinchEnabled",
    "coarticulationVelarPinchF2Scale",
    "coarticulationVelarPinchF3",
    "coarticulationVelarPinchThreshold",
    "coarticulationCrossSyllableScale",
    "coarticulationVelarScale",
    "coarticulationWordInitialFadeScale",
    "coarticulationFadeIntoConsonants",
    "coarticulationSteadyState",
    "coarticulationTransInMs",
    "coarticulationTransOutMs",
    "defaultGlottalOpenQuotient",
    "defaultOutputGain",
    "defaultPreFormantGain",
    "defaultVibratoPitchOffset",
    "defaultVibratoSpeed",
    "defaultVoiceTurbulenceAmplitude",
    "englishLongUKey",
    "englishLongUShortenEnabled",
    "englishLongUWordFinalScale",
    "fujisakiAccentDur",
    "fujisakiAccentLen",
    "fujisakiAccentMode",
    "fujisakiDeclinationMax",
    "fujisakiDeclinationPostFloor",
    "fujisakiCompoundDeclinStep",
    "fujisakiDeclinationRate",
    "fujisakiDeclinationScale",
    "fujisakiMonoAccentDurScale",
    "fujisakiPhraseAmp",
    "fujisakiPhraseDecay",
    "fujisakiPhraseLen",
    "fujisakiPitchCurveExponent",
    "fujisakiPrimaryAccentAmp",
    "fujisakiSecondaryAccentAmp",
    "fujisakiQuestionPhraseAmpScale",
    "fujisakiQuestionAccentBoost",
    "fujisakiQuestionDeclinationScale",
    "fujisakiQuestionBasePitchScale",
    "fujisakiQuestionFinalRiseScale",
    "fujisakiQuestionFinalDropScale",
    "fujisakiExclamationPhraseAmpScale",
    "fujisakiExclamationAccentBoost",
    "fujisakiExclamationDeclinationScale",
    "fujisakiExclamationBasePitchScale",
    "fujisakiExclamationFinalRiseScale",
    "fujisakiExclamationFinalDropScale",
    "fujisakiCommaPhraseAmpScale",
    "fujisakiCommaAccentBoost",
    "fujisakiCommaDeclinationScale",
    "fujisakiCommaBasePitchScale",
    "fujisakiCommaFinalRiseScale",
    "fujisakiCommaFinalDropScale",
    "highRateBandwidthWideningFactor",
    "highRateCoarticulationFloor",
    "highRateThreshold",
    "impulseAssertiveness",
    "impulseContinuationRiseHz",
    "impulseDeclinationHzPerSec",
    "impulseDeclinationRangeHz",
    "impulseFirstStressBoostHz",
    "impulseFourthStressBoostHz",
    "impulseHatFallScale",
    "impulseQuestionReduction",
    "impulseQuestionRiseHz",
    "impulseRiseHz",
    "impulseSecondStressBoostHz",
    "impulseSecondaryStressScale",
    "impulseSmoothAlpha",
    "impulseStressGain",
    "impulseTerminalFallHz",
    "impulseTerminalStressHz",
    "impulseThirdStressBoostHz",
    "klattContinuationRiseHz",
    "klattDeclinationHzPerSec",
    "klattFinalFallBelowBaseHz",
    "klattGlottalLowerHz",
    "klattHatRiseHz",
    "klattQuestionRiseHz",
    "klattSmoothAlpha",
    "klattStress1Hz",
    "klattStress2Hz",
    "klattStress3Hz",
    "klattStress4Hz",
    "huShortAVowelEnabled",
    "huShortAVowelKey",
    "huShortAVowelScale",
    "legacyPitchInflectionScale",
    "legacyPitchMode",
    "diphthongAmplitudeDipFactor",
    "diphthongCollapseEnabled",
    "diphthongDurationScale",
    "diphthongDurationFloorMs",
    "diphthongRateCompensation",
    "diphthongMicroFrameIntervalMs",
    "diphthongOnsetHoldExponent",
    "diphthongOnsetSettleMs",
    "lengthContrastEnabled",
    "lengthContrastGeminateClosureScale",
    "lengthContrastGeminateReleaseScale",
    "lengthContrastLongVowelFloorMs",
    "lengthContrastPreGeminateVowelScale",
    "lengthContrastShortVowelCeilingMs",
    "lengthenedScale",
    "lengthenedScaleHu",
    "lengthenedVowelFinalCodaScale",
    "liquidDynamicsEnabled",
    "liquidDynamicsLabialGlideStartF1",
    "liquidDynamicsLabialGlideStartF2",
    "liquidDynamicsLabialGlideTransitionEnabled",
    "liquidDynamicsLabialGlideTransitionPct",
    "liquidDynamicsLateralOnglideDurationPct",
    "liquidDynamicsLateralOnglideF1Delta",
    "liquidDynamicsLateralOnglideF2Delta",
    "liquidDynamicsRhoticF3DipDurationPct",
    "liquidDynamicsRhoticF3DipEnabled",
    "liquidDynamicsRhoticF3Minimum",
    "microprosodyEnabled",
    "microprosodyFollowingF0Enabled",
    "microprosodyFollowingVoicedLowerHz",
    "microprosodyFollowingVoicelessRaiseHz",
    "microprosodyIntrinsicF0Enabled",
    "microprosodyIntrinsicF0HighRaiseHz",
    "microprosodyIntrinsicF0HighThreshold",
    "microprosodyIntrinsicF0LowDropHz",
    "microprosodyIntrinsicF0LowThreshold",
    "microprosodyMaxTotalDeltaHz",
    "microprosodyMinVowelMs",
    "microprosodyPreVoicelessMinMs",
    "microprosodyPreVoicelessShortenEnabled",
    "microprosodyVoicelessCodaLengthenEnabled",
    "microprosodyVoicelessCodaLengthenScale",
    "microprosodyPreVoicelessShortenScale",
    "microprosodyVoicedF0LowerEnabled",
    "microprosodyVoicedF0LowerHz",
    "microprosodyVoicedFricativeLowerScale",
    "microprosodyVoicelessF0RaiseEnabled",
    "microprosodyVoicelessF0RaiseEndHz",
    "microprosodyVoicelessF0RaiseHz",
    "nasalMinDurationMs",
    "nasalizationAnticipatoryAmplitude",
    "nasalizationAnticipatoryBlend",
    "nasalizationAnticipatoryEnabled",
    "phraseFinalLengtheningCodaFricativeScale",
    "phraseFinalLengtheningCodaNasalScale",
    "phraseFinalLengtheningCodaScale",
    "phraseFinalLengtheningCodaStopScale",
    "phraseFinalLengtheningEnabled",
    "phraseFinalLengtheningFinalSyllableScale",
    "phraseFinalLengtheningNucleusOnlyMode",
    "phraseFinalLengtheningNucleusDiphthongScale",
    "phraseFinalLengtheningNucleusScale",
    "phraseFinalLengtheningPenultimateSyllableScale",
    "phraseFinalLengtheningQuestionScale",
    "phraseFinalLengtheningStatementScale",
    "postStopAspirationEnabled",
    "postStopAspirationPhoneme",
    "primaryStressDiv",
    "prominenceAmplitudeBoostDb",
    "prominenceAmplitudeReductionDb",
    "prominenceDurationPrimaryFloorMs",
    "prominenceDurationProminentFloorMs",
    "prominenceDurationReducedCeiling",
    "prominenceEnabled",
    "prominenceFullVowelFloor",
    "prominenceMonosyllableFloor",
    "prominenceLongVowelMode",
    "prominenceLongVowelWeight",
    "prominencePitchFromProminence",
    "prominencePrimaryStressWeight",
    "prominenceSecondaryStressLevel",
    "prominenceSecondaryStressWeight",
    "prominenceWordFinalReduction",
    "prominenceWordInitialBoost",
    "rateCompAffricateFloorMs",
    "rateCompClusterMaxRatioShift",
    "rateCompClusterProportionGuard",
    "rateCompEnabled",
    "rateCompFloorSpeedScale",
    "rateCompFricativeFloorMs",
    "rateCompLiquidFloorMs",
    "rateCompNasalFloorMs",
    "rateCompSchwaReductionEnabled",
    "rateCompSchwaScale",
    "rateCompSchwaThreshold",
    "rateCompSemivowelFloorMs",
    "rateCompSonorantContextBonusMs",
    "rateCompStopFloorMs",
    "rateCompTapFloorMs",
    "rateCompTrillFloorMs",
    "rateCompDiphthongFloorMs",
    "rateCompVoicedConsonantFloorMs",
    "rateCompVowelFloorMs",
    "rateCompWordFinalBonusMs",
    "sonorantContextAmplitudeScale",
    "secondaryStressDiv",
    "segmentBoundaryFadeMs",
    "segmentBoundaryGapMs",
    "segmentBoundarySkipVowelToLiquid",
    "segmentBoundarySkipVowelToVowel",
    "semivowelOffglideScale",
    "singleWordClauseTypeOverride",
    "singleWordClauseTypeOverrideCommaOnly",
    "singleWordFinalFadeMs",
    "singleWordFinalHoldMs",
    "singleWordFinalLiquidHoldScale",
    "singleWordTuningEnabled",
    "specialCoarticMaxDeltaHz",
    "specialCoarticulationEnabled",
    "spellingDiphthongMode",
    "syllableDurationCodaScale",
    "syllableDurationEnabled",
    "syllableDurationOnsetScale",
    "syllableDurationUnstressedOpenNucleusScale",
    "stopClosureAfterNasalsEnabled",
    "stopClosureClusterFadeMs",
    "stopClosureClusterGapMs",
    "stopClosureNasalToStopFadeMs",
    "stopClosureNasalToStopGapMs",
    "stopClosureClusterGapsEnabled",
    "stopClosureMode",
    "stopClosureVowelFadeMs",
    "stopClosureVowelGapMs",
    "stopClosureWordBoundaryClusterFadeMs",
    "stopClosureWordBoundaryClusterGapMs",
    "stressedVowelHiatusFadeMs",
    "stressedVowelHiatusGapMs",
    "svarabhaktiInheritEnabled",
    "svarabhaktiVowelWeight",
    "svarabhaktiDurationScale",
    "svarabhaktiAmpScale",
    "svarabhaktiTapBlend",
    "svarabhaktiTapDip",
    "stripAllophoneDigits",
    "stripHyphen",
    "tonal",
    "toneContoursAbsolute",
    "toneContoursMode",
    "toneDigitsEnabled",
    "trajectoryLimitApplyAcrossWordBoundary",
    "trajectoryLimitApplyTo",
    "trajectoryLimitEnabled",
    "trajectoryLimitLiquidRateScale",
    "trajectoryLimitMaxHzPerMsCf1",
    "trajectoryLimitMaxHzPerMsCf2",
    "trajectoryLimitMaxHzPerMsCf3",
    "trajectoryLimitMaxHzPerMsPf2",
    "trajectoryLimitMaxHzPerMsPf3",
    "trajectoryLimitWindowMs",
    "trillModulationFadeMs",
    "trillModulationMs",
    "voiceProfileName",
    "wordBoundaryDipDepth",
    "wordBoundaryDipMs",
    "wordFinalSchwaMinDurationMs",
    "wordFinalSchwaReductionEnabled",
    "wordFinalSchwaScale",
    "thousandsSeparatorCommaToSpace",
    "yearSplittingEnabled",
  };
}

// -------------------------
// Speech settings (voice + sliders)
// -------------------------

static void onEditLanguageSettings(AppController& app) {
  if (!app.language.isLoaded()) {
    msgBox(app.wnd, L"Load a language first.", L"TGSB Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  EditSettingsDialogState st;
  st.settings = app.language.settings();
  st.knownKeys = knownLanguageSettingKeys();

  ShowEditSettingsDialog(app.hInst, app.wnd, st);
  if (!st.ok) return;

  app.language.setSettings(st.settings);
  app.languageDirty = true;
  app.setStatus(L"Edited language settings in memory. Use File > Save language YAML (Ctrl+S) to write it.");
}

// -------------------------
// Phoneme edits
// -------------------------
static void onClonePhoneme(AppController& app) {
  if (!app.phonemes.isLoaded()) return;

  std::string selected = getSelectedPhonemeKey(app.listPhonemes);

  ClonePhonemeDialogState st;
  st.keys = app.phonemeKeys;
  st.fromKey = selected;

  ShowClonePhonemeDialog(app.hInst, app.wnd, st);
  if (!st.ok) return;

  std::string err;
  if (!app.phonemes.clonePhoneme(st.fromKey, st.newKey, err)) {
    msgBox(app.wnd, L"Clone failed:\n" + utf8ToWide(err), L"TGSB Phoneme Editor", MB_ICONERROR);
    return;
  }

  // Reload list.
  app.phonemeKeys = app.phonemes.phonemeKeysSorted();
  rebuildPhonemeKeysU32(app);
  populatePhonemeList(app, L"");
  app.phonemesDirty = true;

  msgBox(app.wnd, L"Cloned phoneme. Remember to save phonemes YAML (Ctrl+P).", L"TGSB Phoneme Editor", MB_ICONINFORMATION);
}

// -------------------------
// Bug 6: Language scope awareness helpers
// -------------------------

// Recursively compare two Node trees. Returns true if identical.
static bool nodesEqual(const Node& a, const Node& b) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case Node::Type::Scalar:
      return a.scalar == b.scalar;
    case Node::Type::Map:
      if (a.map.size() != b.map.size()) return false;
      for (const auto& kv : a.map) {
        auto it = b.map.find(kv.first);
        if (it == b.map.end()) return false;
        if (!nodesEqual(kv.second, it->second)) return false;
      }
      return true;
    case Node::Type::Seq:
      if (a.seq.size() != b.seq.size()) return false;
      for (size_t i = 0; i < a.seq.size(); ++i) {
        if (!nodesEqual(a.seq[i], b.seq[i])) return false;
      }
      return true;
    case Node::Type::Null:
    default:
      return true;
  }
}

// Build language suffix: "es" -> "_es", "en-us" -> "_enus", "pt-br" -> "_ptbr".
static std::string makeLangSuffix(const std::string& langTag) {
  std::string out = "_";
  for (char c : langTag) {
    if (c != '-') out += c;
  }
  return out;
}

// Return true if key looks language-specific (contains _ followed by 2+ letters).
static bool isLangSpecificKey(const std::string& key) {
  auto pos = key.find('_');
  if (pos == std::string::npos || pos == 0) return false;
  size_t tail = key.size() - pos - 1;
  if (tail < 2) return false;
  for (size_t i = pos + 1; i < key.size(); ++i) {
    if (!std::isalpha(static_cast<unsigned char>(key[i]))) return false;
  }
  return true;
}

// Result of scanning all language packs for references to a phoneme.
struct PhonemeUsageScan {
  std::vector<std::string> usedInRules;   // lang tags that reference this key in normalization
  std::vector<std::string> hasOverride;   // lang tags that have a phonemes: override for this key
  int totalLangCount = 0;
};

// Scan all .yaml files in langDir for references to a phoneme key.
static PhonemeUsageScan scanPhonemeUsage(const std::wstring& langDir, const std::string& phonemeKey) {
  PhonemeUsageScan result;
  if (langDir.empty()) return result;

  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(langDir, ec)) {
    if (!entry.is_regular_file()) continue;
    auto ext = entry.path().extension().u8string();
    if (ext != ".yaml" && ext != ".yml") continue;

    result.totalLangCount++;
    std::string langTag = entry.path().stem().u8string();

    Node root;
    std::string parseErr;
    if (!nvsp_frontend::yaml_min::loadFile(entry.path().u8string(), root, parseErr)) continue;

    // Check normalization.replacements for references.
    const Node* norm = root.get("normalization");
    if (norm && norm->isMap()) {
      const Node* repl = norm->get("replacements");
      if (repl && repl->isSeq()) {
        for (const auto& item : repl->seq) {
          if (!item.isMap()) continue;
          const Node* f = item.get("from");
          const Node* t = item.get("to");
          std::string fv = f ? f->asString() : "";
          std::string tv = t ? t->asString() : "";
          if (fv == phonemeKey || tv == phonemeKey) {
            result.usedInRules.push_back(langTag);
            break; // one match per lang is enough
          }
        }
      }
    }

    // Check phonemes: section for overrides.
    const Node* phon = root.get("phonemes");
    if (phon && phon->isMap()) {
      if (phon->map.find(phonemeKey) != phon->map.end()) {
        result.hasOverride.push_back(langTag);
      }
    }
  }
  return result;
}

// Build a human-readable summary of what changed between original and working nodes.
static std::string describeChanges(const Node& original, const Node& working) {
  std::string out;
  // Check all keys in working.
  for (const auto& kv : working.map) {
    const Node* orig = original.get(kv.first);
    if (!orig) {
      out += "  + " + kv.first + ": " + kv.second.asString() + "\n";
    } else if (!nodesEqual(*orig, kv.second)) {
      out += "  " + kv.first + ": " + orig->asString() + " -> " + kv.second.asString() + "\n";
    }
  }
  // Check for removed keys.
  for (const auto& kv : original.map) {
    if (working.map.find(kv.first) == working.map.end()) {
      out += "  - " + kv.first + " (removed)\n";
    }
  }
  return out;
}

// Build the full scope warning message for the dialog.
static std::wstring buildScopeWarning(const std::string& key, const std::string& langTag,
                                      const std::string& changes, const PhonemeUsageScan& usage) {
  std::string msg;
  msg += "You modified \"" + key + "\" which is shared globally.\n\n";

  if (!changes.empty()) {
    msg += "Changes:\n" + changes + "\n";
  }

  msg += "Impact:\n";
  if (!usage.usedInRules.empty()) {
    msg += "  Referenced in normalization rules: ";
    for (size_t i = 0; i < usage.usedInRules.size(); ++i) {
      if (i > 0) msg += ", ";
      msg += usage.usedInRules[i];
    }
    msg += " (" + std::to_string(usage.usedInRules.size()) + " packs)\n";
  }
  if (!usage.hasOverride.empty()) {
    msg += "  Has language-specific override: ";
    for (size_t i = 0; i < usage.hasOverride.size(); ++i) {
      if (i > 0) msg += ", ";
      msg += usage.hasOverride[i];
    }
    msg += "\n";
  }
  int directUsers = usage.totalLangCount - static_cast<int>(usage.hasOverride.size());
  if (directUsers > 0) {
    msg += "  Uses global definition directly: " + std::to_string(directUsers) + " packs\n";
  }

  msg += "\nCreate \"" + key + makeLangSuffix(langTag) + "\" for " + langTag + " only?\n\n";
  msg += "Yes = Create language-specific copy\n";
  msg += "No = Apply globally (affects all languages)\n";
  msg += "Cancel = Discard changes";

  return utf8ToWide(msg);
}

static void onEditSelectedPhoneme(AppController& app, bool fromLanguageList) {
  std::string key = fromLanguageList ? getSelectedPhonemeKey(app.listLangPhonemes) : getSelectedPhonemeKey(app.listPhonemes);
  if (key.empty()) {
    msgBox(app.wnd, L"Select a phoneme first.", L"TGSB Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  tgsb_editor::Node* node = app.phonemes.getPhonemeNode(key);
  if (!node || !node->isMap()) {
    msgBox(app.wnd, L"Phoneme not found in phonemes.yaml.", L"TGSB Phoneme Editor", MB_ICONERROR);
    return;
  }

  EditPhonemeDialogState st;
  st.phonemeKey = key;
  st.original = *node;
  st.working = *node;
  st.runtime = &app.runtime;

  ShowEditPhonemeDialog(app.hInst, app.wnd, st);
  if (!st.ok) return;

  // Nothing changed — silent return.
  if (nodesEqual(st.original, st.working)) return;

  // Bug 6: Language scope awareness.
  // If a language is loaded AND the key is not already language-specific,
  // scan all language packs to show the blast radius and offer clone-for-language.
  std::string langTag = selectedLangTagUtf8(app);
  bool langLoaded = app.language.isLoaded() && !langTag.empty();

  if (langLoaded && !isLangSpecificKey(key)) {
    PhonemeUsageScan usage = scanPhonemeUsage(app.langDir, key);
    std::string changes = describeChanges(st.original, st.working);
    std::wstring warning = buildScopeWarning(key, langTag, changes, usage);

    int res = MessageBoxW(app.wnd, warning.c_str(), L"Shared Phoneme Warning",
                          MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON1);

    if (res == IDCANCEL) return;

    if (res == IDYES) {
      // Create language-specific clone.
      std::string newKey = key + makeLangSuffix(langTag);

      // If clone already exists, just apply edits to it.
      Node* existing = app.phonemes.getPhonemeNode(newKey);
      if (existing) {
        *existing = st.working;
      } else {
        std::string cloneErr;
        if (!app.phonemes.clonePhoneme(key, newKey, cloneErr)) {
          msgBox(app.wnd, L"Clone failed:\n" + utf8ToWide(cloneErr), L"TGSB Phoneme Editor", MB_ICONERROR);
          return;
        }
        // Apply the user's edits to the new clone (clonePhoneme copies original).
        Node* cloned = app.phonemes.getPhonemeNode(newKey);
        if (cloned) *cloned = st.working;
      }

      // Add normalization rule from -> to if not already present.
      auto repls = app.language.replacements();
      bool ruleExists = false;
      for (const auto& r : repls) {
        if (r.from == key && r.to == newKey && r.when.isEmpty()) {
          ruleExists = true;
          break;
        }
      }
      if (!ruleExists) {
        ReplacementRule newRule;
        newRule.from = key;
        newRule.to = newKey;
        repls.push_back(newRule);
        app.language.setReplacements(repls);
        app.repls = app.language.replacements();
        app.skipRepls = app.language.skipReplacements();
        app.languageDirty = true;
      }

      // Refresh UI.
      app.phonemeKeys = app.phonemes.phonemeKeysSorted();
      rebuildPhonemeKeysU32(app);
      populatePhonemeList(app, L"");
      refreshLanguageDerivedLists(app);
      app.phonemesDirty = true;

      msgBox(app.wnd, L"Created \"" + utf8ToWide(newKey) + L"\" for " + utf8ToWide(langTag)
        + L".\nRemember to save both phonemes (Ctrl+P) and language (Ctrl+S).",
        L"TGSB Phoneme Editor", MB_ICONINFORMATION);
      return;
    }

    // res == IDNO: fall through to apply globally (with flag warning if applicable).
  }

  // Bug 7: Warn on critical flag changes (isVowel, isVoiced, etc.).
  {
    const char* criticalFlags[] = {"_isVowel", "_isVoiced", "_isStop", "_isNasal",
                                   "_isSemivowel", "_isLiquid", "_isAffricate"};
    std::string flagWarnings;
    for (const char* flag : criticalFlags) {
      const Node* origF = st.original.get(flag);
      const Node* newF = st.working.get(flag);
      std::string origVal = origF ? origF->asString("false") : "false";
      std::string newVal = newF ? newF->asString("false") : "false";
      if (origVal != newVal) {
        flagWarnings += std::string(flag) + ": " + origVal + " -> " + newVal + "\n";
      }
    }
    if (!flagWarnings.empty()) {
      std::wstring msg = L"Warning: You changed critical phoneme flags for '"
        + utf8ToWide(key) + L"':\n\n"
        + utf8ToWide(flagWarnings)
        + L"\nThis phoneme is shared across ALL language packs.\n"
          L"Flag changes affect timing, microprosody, prominence,\n"
          L"and syllable detection globally.\n\n"
          L"Apply this change?";
      int res = MessageBoxW(app.wnd, msg.c_str(), L"Flag Change Warning",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
      if (res != IDYES) return;
    }
  }

  // Apply globally.
  *node = st.working;
  app.phonemesDirty = true;
  msgBox(app.wnd, L"Phoneme updated. Remember to save phonemes YAML (Ctrl+P).", L"TGSB Phoneme Editor", MB_ICONINFORMATION);
}

// -------------------------
// Save YAML
// -------------------------
static void onSaveLanguage(AppController& app) {
  if (!app.language.isLoaded()) {
    msgBox(app.wnd, L"No language YAML loaded.", L"TGSB Phoneme Editor", MB_ICONINFORMATION);
    return;
  }
  std::string err;
  if (!app.language.save(err)) {
    msgBox(app.wnd, L"Save failed:\n" + utf8ToWide(err), L"TGSB Phoneme Editor", MB_ICONERROR);
    return;
  }

  // Reload from disk to sync with any external changes (e.g., edits made in a text editor).
  std::string langPath = app.language.path();
  if (!app.language.load(langPath, err)) {
    msgBox(app.wnd, L"Warning: Failed to reload language YAML after save:\n" + utf8ToWide(err), L"TGSB Phoneme Editor", MB_ICONWARNING);
  } else {
    app.repls = app.language.replacements();
    app.skipRepls = app.language.skipReplacements();
    app.classNames = app.language.classNamesSorted();
    refreshLanguageDerivedLists(app);

    // Update runtime language for TTS.
    std::string langTag = selectedLangTagUtf8(app);
    if (!langTag.empty() && app.runtime.dllsLoaded() && !app.packRoot.empty()) {
      std::string rtErr;
      app.runtime.setLanguage(langTag, rtErr);
    }
  }

  app.languageDirty = false;
  app.setStatus(L"Saved language YAML");
}

// Validate formant ordering: cf1 < cf2 < cf3 ... and pf1 < pf2 < pf3 ...
// Returns empty string on success, or a multi-line error listing violations.
static std::string validateFormantOrdering(AppController& app) {
  std::string errors;
  auto keys = app.phonemes.phonemeKeysSorted();

  // Pairs of (prefix, count): cascade and parallel formant frequencies.
  const char* prefixes[] = {"cf", "pf"};
  const char* labels[] = {"cascade", "parallel"};

  for (const auto& phonemeKey : keys) {
    const Node* node = app.phonemes.getPhonemeNode(phonemeKey);
    if (!node || !node->isMap()) continue;

    for (int p = 0; p < 2; ++p) {
      double prev = 0.0;
      int prevIdx = 0;
      for (int i = 1; i <= 6; ++i) {
        char fieldName[8];
        std::snprintf(fieldName, sizeof(fieldName), "%s%d", prefixes[p], i);
        const Node* fn = node->get(fieldName);
        if (!fn) continue;
        double val = 0.0;
        if (!fn->asNumber(val) || val <= 0.0) continue;
        if (prev > 0.0 && val <= prev) {
          char buf[256];
          std::snprintf(buf, sizeof(buf),
            "%s: %s F%d (%.0f Hz) <= F%d (%.0f Hz)\n",
            phonemeKey.c_str(), labels[p], i, val, prevIdx, prev);
          errors += buf;
        }
        prev = val;
        prevIdx = i;
      }
    }
  }
  return errors;
}

static void onSavePhonemes(AppController& app) {
  if (!app.phonemes.isLoaded()) {
    msgBox(app.wnd, L"No phonemes YAML loaded.", L"TGSB Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  // Validate formant ordering before save (Bug 8).
  std::string formantErrors = validateFormantOrdering(app);
  if (!formantErrors.empty()) {
    std::wstring msg = L"Formant ordering violations found:\n\n"
      + utf8ToWide(formantErrors)
      + L"\nFormant frequencies must be in ascending order (F1 < F2 < F3 ...).\n"
        L"Save anyway?";
    int res = MessageBoxW(app.wnd, msg.c_str(), L"Validation Warning", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (res != IDYES) return;
  }

  std::string err;
  if (!app.phonemes.save(err)) {
    msgBox(app.wnd, L"Save failed:\n" + utf8ToWide(err), L"TGSB Phoneme Editor", MB_ICONERROR);
    return;
  }

  // Reload from disk to sync with any external changes (e.g., edits made in a text editor).
  std::string phonemesPath = app.phonemes.path();
  if (!app.phonemes.load(phonemesPath, err)) {
    msgBox(app.wnd, L"Warning: Failed to reload phonemes YAML after save:\n" + utf8ToWide(err), L"TGSB Phoneme Editor", MB_ICONWARNING);
  } else {
    app.phonemeKeys = app.phonemes.phonemeKeysSorted();
    rebuildPhonemeKeysU32(app);

    std::wstring filter;
    wchar_t buf[512];
    GetWindowTextW(app.editFilter, buf, 512);
    filter = buf;
    populatePhonemeList(app, filter);

    // Also refresh language-derived lists since they depend on phoneme keys.
    app.usedPhonemeKeys = extractUsedPhonemes(app, app.repls);
    populateLanguagePhonemesList(app);
  }

  app.phonemesDirty = false;
  app.setStatus(L"Saved phonemes YAML");
}

// -------------------------
// Reload YAML (from disk)
// -------------------------
static void onReloadLanguage(AppController& app) {
  if (!app.language.isLoaded()) {
    msgBox(app.wnd, L"No language YAML loaded.", L"TGSB Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  // Warn if there are unsaved changes.
  if (app.languageDirty) {
    int res = MessageBoxW(app.wnd,
      L"You have unsaved changes to the language YAML.\n\nReload from disk and discard changes?",
      L"Unsaved Changes", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (res != IDYES) {
      return;
    }
  }

  std::string langPath = app.language.path();
  std::string err;
  if (!app.language.load(langPath, err)) {
    msgBox(app.wnd, L"Failed to reload language YAML:\n" + utf8ToWide(err), L"TGSB Phoneme Editor", MB_ICONERROR);
    return;
  }

  app.repls = app.language.replacements();
  app.skipRepls = app.language.skipReplacements();
  app.classNames = app.language.classNamesSorted();
  app.languageDirty = false;
  refreshLanguageDerivedLists(app);

  // Update runtime language for TTS.
  std::string langTag = selectedLangTagUtf8(app);
  if (!langTag.empty() && app.runtime.dllsLoaded() && !app.packRoot.empty()) {
    std::string rtErr;
    app.runtime.setLanguage(langTag, rtErr);
  }

  app.setStatus(L"Reloaded language YAML from disk");
}

static void onReloadPhonemes(AppController& app) {
  if (!app.phonemes.isLoaded()) {
    msgBox(app.wnd, L"No phonemes YAML loaded.", L"TGSB Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  // Warn if there are unsaved changes.
  if (app.phonemesDirty) {
    int res = MessageBoxW(app.wnd,
      L"You have unsaved changes to the phonemes YAML.\n\nReload from disk and discard changes?",
      L"Unsaved Changes", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (res != IDYES) {
      return;
    }
  }

  std::string phonemesPath = app.phonemes.path();
  std::string err;
  if (!app.phonemes.load(phonemesPath, err)) {
    msgBox(app.wnd, L"Failed to reload phonemes YAML:\n" + utf8ToWide(err), L"TGSB Phoneme Editor", MB_ICONERROR);
    return;
  }

  app.phonemeKeys = app.phonemes.phonemeKeysSorted();
  app.phonemesDirty = false;
  rebuildPhonemeKeysU32(app);

  std::wstring filter;
  wchar_t buf[512];
  GetWindowTextW(app.editFilter, buf, 512);
  filter = buf;
  populatePhonemeList(app, filter);

  // Also refresh language-derived lists since they depend on phoneme keys.
  app.usedPhonemeKeys = extractUsedPhonemes(app, app.repls);
  populateLanguagePhonemesList(app);

  app.setStatus(L"Reloaded phonemes YAML from disk");
}

// -------------------------
// Text test
// -------------------------
static std::wstring getText(HWND hEdit) {
  int len = GetWindowTextLengthW(hEdit);
  if (len <= 0) return L"";

  // GetWindowTextW writes a trailing NUL, so allocate len+1.
  std::wstring buf;
  buf.resize(static_cast<size_t>(len) + 1);

  int copied = GetWindowTextW(hEdit, buf.data(), len + 1);
  if (copied < 0) copied = 0;

  buf.resize(static_cast<size_t>(copied));
  return buf;
}

static void setText(HWND hEdit, const std::wstring& text) {
  SetWindowTextW(hEdit, text.c_str());
}

static bool ensureEspeakDir(AppController& app) {
  if (!app.espeakDir.empty()) return true;
  app.espeakDir = readIni(L"paths", L"espeakDir", L"");
  return !app.espeakDir.empty();
}

static bool convertTextToIpaViaPhonemizer(AppController& app, const std::wstring& text, std::string& outIpaUtf8, std::string& outError) {
  outIpaUtf8.clear();
  outError.clear();

  std::string langTag = selectedLangTagUtf8(app);

  // Config lives in tgsbPhonemeEditor.ini.
  //
  // If [phonemizer].exe is empty, we use the configured eSpeak directory and call
  // espeak-ng.exe/espeak.exe.
  //
  // This is intentionally CLI-only (no DLL loading) to keep licensing simpler and
  // to let advanced users point the tool at other phonemizers.
  tgsb_editor::CliPhonemizerConfig cfg;
  cfg.preferStdin = (readIniInt(L"phonemizer", L"preferStdin", 1) != 0);
  cfg.maxChunkChars = static_cast<size_t>(readIniInt(L"phonemizer", L"maxChunkChars", 420));

  cfg.exePath = readIni(L"phonemizer", L"exe", L"");
  cfg.argsStdinTemplate = readIni(L"phonemizer", L"argsStdin", L"");
  cfg.argsCliTemplate = readIni(L"phonemizer", L"argsCli", L"");

  // Default: use eSpeak NG CLI.
  if (cfg.exePath.empty()) {
    if (!ensureEspeakDir(app)) {
      outError = "eSpeak directory is not set";
      return false;
    }

    cfg.espeakDir = app.espeakDir;
    cfg.espeakDataDir = tgsb_editor::findEspeakDataDir(app.espeakDir);

    cfg.exePath = tgsb_editor::findEspeakExe(app.espeakDir);
    if (cfg.exePath.empty()) {
      outError = "Could not find espeak-ng.exe or espeak.exe in the configured directory";
      return false;
    }

    // Sensible defaults.
    // - Prefer stdin to avoid Windows command-line length limits.
    // - Keep -b 1 so stdin is interpreted as UTF-8.
    if (cfg.argsStdinTemplate.empty()) {
      cfg.argsStdinTemplate = L"-q {pathArg}--ipa=3 -b 1 -v {qlang} --stdin";
    }
    if (cfg.argsCliTemplate.empty()) {
      cfg.argsCliTemplate = L"-q {pathArg}--ipa=3 -b 1 -v {qlang} {qtext}";
    }
  }

  return tgsb_editor::phonemizeTextToIpa(cfg, langTag, text, outIpaUtf8, outError);
}

static void onConvertIpa(AppController& app) {
  std::wstring text = getText(app.editText);
  if (text.empty()) {
    msgBox(app.wnd, L"Enter some text first.", L"TGSB Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  std::string ipa;
  std::string err;
  if (!convertTextToIpaViaPhonemizer(app, text, ipa, err)) {
    msgBox(app.wnd, L"IPA conversion failed:\n" + utf8ToWide(err) + L"\n\nTip: you can also tick 'Input is IPA' and paste IPA directly.", L"TGSB Phoneme Editor", MB_ICONERROR);
    return;
  }

  setText(app.editIpaOut, utf8ToWide(ipa));
  app.setStatus(L"Converted text to IPA");
}

static bool synthIpaFromUi(AppController& app, std::vector<sample>& outSamples, std::string& outError) {
  outSamples.clear();
  outError.clear();

  if (!ensureDllDir(app)) {
    outError = "DLLs not loaded";
    return false;
  }
  if (app.packRoot.empty()) {
    outError = "Pack root not loaded";
    return false;
  }

  // Ensure runtime pack root and language.
  std::string tmp;
  app.runtime.setPackRoot(runtimePackDir(app), tmp);
  std::string langTag = selectedLangTagUtf8(app);
  if (!langTag.empty()) {
    std::string errLang;
    app.runtime.setLanguage(langTag, errLang);
  }

  bool inputIsIpa = (SendMessageW(app.chkInputIsIpa, BM_GETCHECK, 0, 0) == BST_CHECKED);
  std::wstring text = getText(app.editText);
  if (text.empty()) {
    outError = "Input is empty";
    return false;
  }

  std::string ipa;
  std::string originalText;
  if (inputIsIpa) {
    ipa = wideToUtf8(text);
    // No original text for stress correction when input is already IPA
  } else {
    originalText = wideToUtf8(text);
    std::string err;
    if (!convertTextToIpaViaPhonemizer(app, text, ipa, err)) {
      outError = err;
      return false;
    }
    setText(app.editIpaOut, utf8ToWide(ipa));
  }

  return app.runtime.synthIpa(ipa, kSampleRate, outSamples, outError, originalText);
}

static void onSpeak(AppController& app) {
  std::vector<sample> samples;
  std::string err;
  if (!synthIpaFromUi(app, samples, err)) {
    msgBox(app.wnd, L"Speak failed:\n" + utf8ToWide(err) + L"\n\nIf this mentions phonemes.yaml, make sure packs/phonemes.yaml exists.", L"TGSB Phoneme Editor", MB_ICONERROR);
    return;
  }
  playSamplesTemp(app, samples);
}

static void onSaveWav(AppController& app) {
  std::vector<sample> samples;
  std::string err;
  if (!synthIpaFromUi(app, samples, err)) {
    msgBox(app.wnd, L"Synthesis failed:\n" + utf8ToWide(err), L"TGSB Phoneme Editor", MB_ICONERROR);
    return;
  }

  std::wstring outPath;
  if (!pickSaveWav(app.wnd, outPath)) return;

  if (!tgsb_editor::writeWav16Mono(outPath, kSampleRate, samples, err)) {
    msgBox(app.wnd, L"WAV write failed:\n" + utf8ToWide(err), L"TGSB Phoneme Editor", MB_ICONERROR);
    return;
  }
  app.setStatus(L"Saved WAV: " + outPath);
}

// -------------------------
// Window proc
// -------------------------
void AppController::layout(int w, int h) {
  AppController& app = *this;
  const int margin = 8;
  const int statusH = 20;
  // Bottom panel contains two labeled multi-line edits + a control row.
  // Give it a bit more space so we don't overlap when labels are present.
  const int bottomH = 230;

  int usableH = h - statusH;
  int topH = usableH - bottomH - margin;
  if (topH < 200) topH = 200;

  int leftW = (w - margin * 3) / 2;
  int rightW = w - margin * 3 - leftW;

  // Left panel
  int xL = margin;
  int y = margin;

  const int labelH = 18;
  const int labelGap = 2;

  MoveWindow(app.lblFilter, xL, y, leftW, labelH, TRUE);
  y += labelH + labelGap;
  MoveWindow(app.editFilter, xL, y, leftW, 22, TRUE);
  y += 22 + margin;

  int btnRowH = 26;
  int btnAreaH = btnRowH + margin;

  MoveWindow(app.lblAllPhonemes, xL, y, leftW, labelH, TRUE);
  y += labelH + labelGap;
  MoveWindow(app.listPhonemes, xL, y, leftW, topH - y - btnAreaH + margin, TRUE);

  int btnY = topH - btnRowH + margin;
  int btnW = (leftW - margin * 3) / 4;
  MoveWindow(app.btnPlay, xL, btnY, btnW, btnRowH, TRUE);
  MoveWindow(app.btnClone, xL + (btnW + margin), btnY, btnW, btnRowH, TRUE);
  MoveWindow(app.btnEdit, xL + (btnW + margin) * 2, btnY, btnW, btnRowH, TRUE);
  MoveWindow(app.btnAddToLang, xL + (btnW + margin) * 3, btnY, btnW, btnRowH, TRUE);

  // Right panel
  int xR = xL + leftW + margin;
  int yR = margin;

  MoveWindow(app.lblLanguage, xR, yR, rightW, labelH, TRUE);
  yR += labelH + labelGap;
  MoveWindow(app.comboLang, xR, yR, rightW, 200, TRUE);
  yR += 26 + margin;

  MoveWindow(app.lblLangPhonemes, xR, yR, rightW, labelH, TRUE);
  yR += labelH + labelGap;
  int langPhH = 90;
  MoveWindow(app.listLangPhonemes, xR, yR, rightW, langPhH, TRUE);

  int langBtnW = (rightW - margin * 2) / 3;
  int langBtnY = yR + langPhH + margin;
  MoveWindow(app.btnLangPlay, xR, langBtnY, langBtnW, btnRowH, TRUE);
  MoveWindow(app.btnLangEdit, xR + (langBtnW + margin), langBtnY, langBtnW, btnRowH, TRUE);
  MoveWindow(app.btnLangSettings, xR + (langBtnW + margin) * 2, langBtnY, langBtnW, btnRowH, TRUE);

  int mapY = langBtnY + btnRowH + margin;
  int mapBtnH = btnRowH;
  int mapBtnAreaH = mapBtnH + margin;

  // Split the normalization area: mappings (~60%), skip rules (~40%).
  int normAvail = topH - mapY - (mapBtnAreaH * 2) - labelH - labelGap - margin;
  int mapListH = (normAvail * 6) / 10;
  int skipListH = normAvail - mapListH;

  MoveWindow(app.lblMappings, xR, mapY, rightW, labelH, TRUE);
  mapY += labelH + labelGap;
  MoveWindow(app.listMappings, xR, mapY, rightW, mapListH, TRUE);
  mapY += mapListH + margin;

  int mapBtnW = (rightW - margin * 2) / 3;
  MoveWindow(app.btnAddMap, xR, mapY, mapBtnW, mapBtnH, TRUE);
  MoveWindow(app.btnEditMap, xR + mapBtnW + margin, mapY, mapBtnW, mapBtnH, TRUE);
  MoveWindow(app.btnRemoveMap, xR + (mapBtnW + margin) * 2, mapY, mapBtnW, mapBtnH, TRUE);
  mapY += mapBtnH + margin;

  MoveWindow(app.lblSkip, xR, mapY, rightW, labelH, TRUE);
  mapY += labelH + labelGap;
  MoveWindow(app.listSkip, xR, mapY, rightW, skipListH, TRUE);
  mapY += skipListH + margin;

  int skipBtnW = (rightW - margin) / 2;
  MoveWindow(app.btnAddSkip, xR, mapY, skipBtnW, mapBtnH, TRUE);
  MoveWindow(app.btnRemoveSkip, xR + skipBtnW + margin, mapY, skipBtnW, mapBtnH, TRUE);

  // Bottom panel
  int bottomY = topH + margin * 2;
  int bottomW = w - margin * 2;

  MoveWindow(app.lblText, margin, bottomY, bottomW, labelH, TRUE);
  bottomY += labelH + labelGap;
  MoveWindow(app.editText, margin, bottomY, bottomW, 70, TRUE);

  int controlsY = bottomY + 70 + margin;
  MoveWindow(app.chkInputIsIpa, margin, controlsY, 120, 22, TRUE);
  MoveWindow(app.btnConvertIpa, margin + 130, controlsY, 140, 22, TRUE);
  MoveWindow(app.btnSpeak, margin + 280, controlsY, 120, 22, TRUE);
  MoveWindow(app.btnSaveWav, margin + 410, controlsY, 120, 22, TRUE);

  int ipaLabelY = controlsY + 22 + margin;
  MoveWindow(app.lblIpaOut, margin, ipaLabelY, bottomW, labelH, TRUE);
  int ipaY = ipaLabelY + labelH + labelGap;
  MoveWindow(app.editIpaOut, margin, ipaY, bottomW, 70, TRUE);

  // Status
  MoveWindow(app.status, 0, h - statusH, w, statusH, TRUE);
}

LRESULT AppController::HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  AppController& app = *this;

  switch (msg) {
    case WM_CREATE: {
      app.wnd = hWnd;

      // Controls
      app.lblFilter = CreateWindowW(L"STATIC", L"Filter phonemes:", WS_CHILD | WS_VISIBLE,
                                   0, 0, 100, 18, hWnd, nullptr, app.hInst, nullptr);
      app.editFilter = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                       0, 0, 100, 22, hWnd, (HMENU)IDC_EDIT_FILTER, app.hInst, nullptr);
      // Provide a cue banner as a fallback name/description for screen readers
      // that don't associate this edit with the adjacent STATIC label.
      SendMessageW(app.editFilter, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Filter phonemes"));

      app.lblAllPhonemes = CreateWindowW(L"STATIC", L"All phonemes:", WS_CHILD | WS_VISIBLE,
                                        0, 0, 100, 18, hWnd, nullptr, app.hInst, nullptr);

      app.listPhonemes = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"All phonemes", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL,
                                         0, 0, 100, 100, hWnd, (HMENU)IDC_LIST_PHONEMES, app.hInst, nullptr);
      installAccessibleNameForListView(app.listPhonemes, L"All phonemes");
      ListView_SetExtendedListViewStyle(app.listPhonemes, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
      lvAddColumn(app.listPhonemes, 0, L"All phonemes", 160);

      app.btnPlay = CreateWindowW(L"BUTTON", L"&Play", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 0, 0, 80, 24, hWnd, (HMENU)IDC_BTN_PLAY_PHONEME, app.hInst, nullptr);
      app.btnClone = CreateWindowW(L"BUTTON", L"&Clone...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  0, 0, 80, 24, hWnd, (HMENU)IDC_BTN_CLONE_PHONEME, app.hInst, nullptr);
      app.btnEdit = CreateWindowW(L"BUTTON", L"&Edit...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 0, 0, 80, 24, hWnd, (HMENU)IDC_BTN_EDIT_PHONEME, app.hInst, nullptr);
      app.btnAddToLang = CreateWindowW(L"BUTTON", L"Add to lan&guage...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                      0, 0, 130, 24, hWnd, (HMENU)IDC_BTN_ADD_TO_LANGUAGE, app.hInst, nullptr);

      app.lblLanguage = CreateWindowW(L"STATIC", L"Language:", WS_CHILD | WS_VISIBLE,
                                     0, 0, 100, 18, hWnd, nullptr, app.hInst, nullptr);
      app.comboLang = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                     0, 0, 100, 200, hWnd, (HMENU)IDC_COMBO_LANGUAGE, app.hInst, nullptr);

      app.lblLangPhonemes = CreateWindowW(L"STATIC", L"Phonemes in language:", WS_CHILD | WS_VISIBLE,
                                        0, 0, 100, 18, hWnd, nullptr, app.hInst, nullptr);

      app.listLangPhonemes = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"Phonemes in language", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL,
                                            0, 0, 100, 100, hWnd, (HMENU)IDC_LIST_LANG_PHONEMES, app.hInst, nullptr);
      installAccessibleNameForListView(app.listLangPhonemes, L"Phonemes in language");
      ListView_SetExtendedListViewStyle(app.listLangPhonemes, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
      lvAddColumn(app.listLangPhonemes, 0, L"Language phonemes", 160);

      app.btnLangPlay = CreateWindowW(L"BUTTON", L"Play from &language", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     0, 0, 130, 24, hWnd, (HMENU)IDC_BTN_LANG_PLAY_PHONEME, app.hInst, nullptr);
      app.btnLangEdit = CreateWindowW(L"BUTTON", L"E&dit phoneme in language...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     0, 0, 180, 24, hWnd, (HMENU)IDC_BTN_LANG_EDIT_PHONEME, app.hInst, nullptr);
      app.btnLangSettings = CreateWindowW(L"BUTTON", L"Language &settings...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                          0, 0, 150, 24, hWnd, (HMENU)IDC_BTN_LANG_SETTINGS, app.hInst, nullptr);

      app.lblMappings = CreateWindowW(L"STATIC", L"Normalization mappings:", WS_CHILD | WS_VISIBLE,
                                   0, 0, 160, 18, hWnd, nullptr, app.hInst, nullptr);

      app.listMappings = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"Normalization mappings", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL,
                                        0, 0, 100, 100, hWnd, (HMENU)IDC_LIST_MAPPINGS, app.hInst, nullptr);
      installAccessibleNameForListView(app.listMappings, L"Normalization mappings");
      ListView_SetExtendedListViewStyle(app.listMappings, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
      lvAddColumn(app.listMappings, 0, L"From", 120);
      lvAddColumn(app.listMappings, 1, L"To", 120);
      lvAddColumn(app.listMappings, 2, L"When", 180);

      app.btnAddMap = CreateWindowW(L"BUTTON", L"&Add mapping...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  0, 0, 120, 24, hWnd, (HMENU)IDC_BTN_ADD_MAPPING, app.hInst, nullptr);
      app.btnEditMap = CreateWindowW(L"BUTTON", L"Edit &mapping...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                   0, 0, 120, 24, hWnd, (HMENU)IDC_BTN_EDIT_MAPPING, app.hInst, nullptr);
      app.btnRemoveMap = CreateWindowW(L"BUTTON", L"&Remove mapping", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     0, 0, 130, 24, hWnd, (HMENU)IDC_BTN_REMOVE_MAPPING, app.hInst, nullptr);

      app.lblSkip = CreateWindowW(L"STATIC", L"Skip inherited rules:", WS_CHILD | WS_VISIBLE,
                                  0, 0, 160, 18, hWnd, nullptr, app.hInst, nullptr);
      app.listSkip = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"Skip inherited rules", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL,
                                     0, 0, 100, 100, hWnd, (HMENU)IDC_LIST_SKIP_RULES, app.hInst, nullptr);
      installAccessibleNameForListView(app.listSkip, L"Skip inherited rules");
      ListView_SetExtendedListViewStyle(app.listSkip, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
      lvAddColumn(app.listSkip, 0, L"From", 120);
      lvAddColumn(app.listSkip, 1, L"To (blank=any)", 130);

      app.btnAddSkip = CreateWindowW(L"BUTTON", L"Add &skip rule...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     0, 0, 130, 24, hWnd, (HMENU)IDC_BTN_ADD_SKIP_RULE, app.hInst, nullptr);
      app.btnRemoveSkip = CreateWindowW(L"BUTTON", L"Remo&ve skip rule", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                        0, 0, 130, 24, hWnd, (HMENU)IDC_BTN_REMOVE_SKIP_RULE, app.hInst, nullptr);

      app.lblText = CreateWindowW(L"STATIC", L"Input text:", WS_CHILD | WS_VISIBLE,
                                 0, 0, 100, 18, hWnd, nullptr, app.hInst, nullptr);
      app.editText = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                    0, 0, 100, 70, hWnd, (HMENU)IDC_EDIT_TEXT, app.hInst, nullptr);
      // Cue banner may not render for multi-line EDIT on all Windows versions,
      // but it helps where supported and is also exposed to some AT.
      SendMessageW(app.editText, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Type text to speak or convert to IPA"));
      SendMessageW(app.editText, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Type text to speak (or IPA if checked)"));

      app.chkInputIsIpa = CreateWindowW(L"BUTTON", L"Input is IPA", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                       0, 0, 120, 22, hWnd, (HMENU)IDC_CHK_INPUT_IS_IPA, app.hInst, nullptr);

      app.btnConvertIpa = CreateWindowW(L"BUTTON", L"Convert to &IPA", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                       0, 0, 120, 22, hWnd, (HMENU)IDC_BTN_CONVERT_IPA, app.hInst, nullptr);
      app.btnSpeak = CreateWindowW(L"BUTTON", L"Spea&k", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  0, 0, 120, 22, hWnd, (HMENU)IDC_BTN_SPEAK, app.hInst, nullptr);
      app.btnSaveWav = CreateWindowW(L"BUTTON", L"Save &WAV...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                    0, 0, 120, 22, hWnd, (HMENU)IDC_BTN_SAVE_WAV, app.hInst, nullptr);

      app.lblIpaOut = CreateWindowW(L"STATIC", L"IPA output:", WS_CHILD | WS_VISIBLE,
                                   0, 0, 100, 18, hWnd, nullptr, app.hInst, nullptr);
      app.editIpaOut = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY,
                                      0, 0, 100, 70, hWnd, (HMENU)IDC_EDIT_IPA, app.hInst, nullptr);
      SendMessageW(app.editIpaOut, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"IPA output appears here"));

      app.status = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE,
                                  0, 0, 0, 0, hWnd, nullptr, app.hInst, nullptr);

      // Load persisted paths.
      app.packRoot = readIni(L"state", L"packRoot", L"");
      app.espeakDir = readIni(L"paths", L"espeakDir", L"");
      app.dllDir = readIni(L"paths", L"dllDir", L"");

      // Try to auto-detect a portable layout when paths are missing.
      // This is silent by design: we only show errors when the user attempts
      // to synthesize and something is still misconfigured.
      auto dirHasDlls = [](const std::wstring& dir) -> bool {
        std::error_code ec;
        fs::path p(dir);
        return fs::exists(p / "speechPlayer.dll", ec) && fs::exists(p / "nvspFrontend.dll", ec);
      };
      auto rootHasPacks = [](const std::wstring& root) -> bool {
        std::error_code ec;
        return fs::is_directory(fs::path(root) / "packs", ec);
      };
      auto detectEspeakDir = [](const std::wstring& baseDir) -> std::wstring {
        const std::wstring sep = (!baseDir.empty() && baseDir.back() == L'\\') ? L"" : L"\\";
        const std::wstring cands[] = {
          baseDir,
          baseDir + sep + L"espeak",
          baseDir + sep + L"espeak ng",
          baseDir + sep + L"espeak ng\\bin",
        };
        for (const auto& d : cands) {
          if (d.empty()) continue;
          std::error_code ec;
          fs::path p(d);
          if (fs::exists(p / "espeak-ng.exe", ec) || fs::exists(p / "espeak.exe", ec)) {
            return d;
          }
        }
        return {};
      };

      // Auto-load DLLs if they live next to the EXE.
      if (app.dllDir.empty()) {
        std::wstring base = exeDir();
        if (dirHasDlls(base)) {
          std::string err;
          if (app.runtime.setDllDirectory(base, err)) {
            app.dllDir = base;
            writeIni(L"paths", L"dllDir", app.dllDir);
          }
        }
      } else {
        // Best-effort load (silent).
        std::string err;
        app.runtime.setDllDirectory(app.dllDir, err);
      }

      // Auto-detect a bundled eSpeak directory.
      if (app.espeakDir.empty()) {
        std::wstring es = detectEspeakDir(exeDir());
        if (!es.empty()) {
          app.espeakDir = es;
          writeIni(L"paths", L"espeakDir", app.espeakDir);
        }
      }

      // If packRoot isn't set yet, try the DLL dir (common portable layout)
      // and then the EXE dir.
      if (app.packRoot.empty()) {
        if (!app.dllDir.empty() && rootHasPacks(app.dllDir)) {
          app.packRoot = app.dllDir;
        } else {
          std::wstring base = exeDir();
          if (rootHasPacks(base)) {
            app.packRoot = base;
          }
        }
      }

      // Load speech settings (voice + sliders) and apply to runtime.
      app.runtime.setSpeechSettings(loadSpeechSettingsFromIni());

      // Initial layout.
      RECT rc{};
      GetClientRect(hWnd, &rc);
      app.layout(rc.right - rc.left, rc.bottom - rc.top);

      if (!app.packRoot.empty()) {
        loadPackRoot(app, app.packRoot);
      } else {
        app.setStatus(L"Use File > Open pack root... to begin.");
      }

      // Set initial focus to the filter edit box.
      app.lastFocus = app.editFilter;
      SetFocus(app.editFilter);

      return 0;
    }

    case WM_ACTIVATE: {
      // Restore focus when the window is reactivated (e.g., after Alt+Tab).
      if (LOWORD(wParam) != WA_INACTIVE) {
        HWND toFocus = app.lastFocus;
        // Validate that the saved handle is still a valid child.
        if (!toFocus || !IsWindow(toFocus) || !IsChild(hWnd, toFocus)) {
          toFocus = app.editFilter; // fallback to filter box
        }
        if (toFocus && IsWindow(toFocus)) {
          SetFocus(toFocus);
        }
      }
      return 0;
    }

    case WM_SIZE: {
      int w = LOWORD(lParam);
      int h = HIWORD(lParam);
      app.layout(w, h);
      return 0;
    }

    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      const int code = HIWORD(wParam);

      // Track focus changes from EDIT (EN_SETFOCUS), BUTTON (BN_SETFOCUS), and COMBOBOX (CBN_SETFOCUS) controls.
      HWND hwndCtl = reinterpret_cast<HWND>(lParam);
      if (hwndCtl && IsWindow(hwndCtl) && IsChild(hWnd, hwndCtl)) {
        if (code == EN_SETFOCUS || code == BN_SETFOCUS || code == CBN_SETFOCUS) {
          app.lastFocus = hwndCtl;
        }
      }

      // Some accessibility actions (e.g., UIA Invoke from a screen reader's
      // object navigation) can activate a control without moving keyboard
      // focus. That makes the UI feel like focus "disappeared" after pressing
      // a button. If the message originated from a control, ensure focus is on
      // that control.
      if (hwndCtl && IsWindow(hwndCtl)) {
        // Only force-focus on explicit *invocation* events (typically button
        // clicks). Many controls (especially EDIT) send WM_COMMAND
        // notifications like EN_CHANGE / EN_KILLFOCUS, and forcing focus for
        // those will trap keyboard navigation (Tab can't escape).
        if (code == 0) {
          wchar_t cls[64] = {0};
          GetClassNameW(hwndCtl, cls, 64);
          if (_wcsicmp(cls, L"Button") == 0) {
            SetFocus(hwndCtl);
          }
        }
      }

      if (id == IDM_FILE_OPEN_PACKROOT) {
        // Check for unsaved changes before opening a new pack root.
        if (app.phonemesDirty || app.languageDirty) {
          std::wstring msg = L"You have unsaved changes:\n";
          if (app.phonemesDirty) msg += L"  - Phonemes YAML\n";
          if (app.languageDirty) msg += L"  - Language YAML\n";
          msg += L"\nOpen a new pack root without saving?";

          int res = MessageBoxW(hWnd, msg.c_str(), L"Unsaved Changes", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
          if (res != IDYES) {
            return 0; // User cancelled.
          }
        }
        std::wstring folder;
        if (pickFolder(hWnd, L"Select the folder that contains 'packs'", folder)) {
          loadPackRoot(app, folder);
        }
        return 0;
      }
      if (id == IDM_FILE_SAVE_LANGUAGE) {
        onSaveLanguage(app);
        return 0;
      }
      if (id == IDM_FILE_SAVE_PHONEMES) {
        onSavePhonemes(app);
        return 0;
      }
      if (id == IDM_FILE_RELOAD_LANGUAGE) {
        onReloadLanguage(app);
        return 0;
      }
      if (id == IDM_FILE_RELOAD_PHONEMES) {
        onReloadPhonemes(app);
        return 0;
      }
      if (id == IDM_FILE_EXIT) {
        SendMessageW(hWnd, WM_CLOSE, 0, 0);
        return 0;
      }

      if (id == IDM_SETTINGS_ESPEAK_DIR) {
        std::wstring folder;
        if (pickFolder(hWnd, L"Select eSpeak directory (contains espeak-ng.exe or espeak.exe)", folder)) {
          app.espeakDir = folder;
          writeIni(L"paths", L"espeakDir", app.espeakDir);
          app.setStatus(L"eSpeak directory set.");
        }
        return 0;
      }
      if (id == IDM_SETTINGS_PHONEMIZER) {
        PhonemizerSettingsDialogState st;
        st.exePath = readIni(L"phonemizer", L"exe", L"");
        st.argsStdin = readIni(L"phonemizer", L"argsStdin", L"");
        st.argsCli = readIni(L"phonemizer", L"argsCli", L"");
        st.preferStdin = (readIniInt(L"phonemizer", L"preferStdin", 1) != 0);
        st.maxChunkChars = readIniInt(L"phonemizer", L"maxChunkChars", 420);

        if (ShowPhonemizerSettingsDialog(app.hInst, hWnd, st)) {
          writeIni(L"phonemizer", L"exe", st.exePath);
          writeIni(L"phonemizer", L"argsStdin", st.argsStdin);
          writeIni(L"phonemizer", L"argsCli", st.argsCli);

          writeIniInt(L"phonemizer", L"preferStdin", st.preferStdin ? 1 : 0);

          // Clamp to something sane.
          int mc = st.maxChunkChars;
          if (mc < 50) mc = 50;
          if (mc > 4000) mc = 4000;
          writeIniInt(L"phonemizer", L"maxChunkChars", mc);

          app.setStatus(L"Phonemizer settings saved.");
        }
        return 0;
      }

      if (id == IDM_SETTINGS_DLL_DIR) {
        std::wstring folder;
        if (pickFolder(hWnd, L"Select DLL directory (contains speechPlayer.dll and nvspFrontend.dll)", folder)) {
          app.dllDir = folder;
          writeIni(L"paths", L"dllDir", app.dllDir);
          // Try loading immediately.
          std::string err;
          if (!app.runtime.setDllDirectory(app.dllDir, err)) {
            msgBox(hWnd, L"DLL load failed:\n" + utf8ToWide(err), L"TGSB Phoneme Editor", MB_ICONERROR);
          } else {
            app.setStatus(L"DLL directory set and loaded.");
            // Convenience: if packs live alongside the DLLs (portable layout),
            // automatically treat this folder as the pack root.
            if (app.packRoot.empty()) {
              std::error_code ec;
              if (fs::is_directory(fs::path(folder) / "packs", ec)) {
                loadPackRoot(app, folder);
              }
            }
            if (!app.packRoot.empty()) {
              std::string tmp;
              app.runtime.setPackRoot(runtimePackDir(app), tmp);
              std::string lt = selectedLangTagUtf8(app);
              if (!lt.empty()) {
                std::string tmp2;
                app.runtime.setLanguage(lt, tmp2);
              }
            }
          }
        }
        return 0;
      }

      if (id == IDM_SETTINGS_SPEECH_SETTINGS) {
        SpeechSettingsDialogState st;
        st.settings = app.runtime.getSpeechSettings();
        st.paramNames = std::vector<std::string>(TgsbRuntime::frameParamNames().begin(), TgsbRuntime::frameParamNames().end());
        st.voicingParamNames = std::vector<std::string>(TgsbRuntime::voicingParamNames().begin(), TgsbRuntime::voicingParamNames().end());
        st.frameExParamNames = std::vector<std::string>(TgsbRuntime::frameExParamNames().begin(), TgsbRuntime::frameExParamNames().end());
        if (st.settings.frameParams.size() != st.paramNames.size()) {
          st.settings.frameParams.assign(st.paramNames.size(), 50);
        }
        if (st.settings.voicingParams.size() != st.voicingParamNames.size()) {
          st.settings.voicingParams.assign(st.voicingParamNames.size(), 50);
        }
        if (st.settings.frameExParams.size() != st.frameExParamNames.size()) {
          // Default: creakiness/breathiness/jitter/shimmer=0, sharpness=50
          st.settings.frameExParams.assign(st.frameExParamNames.size(), 0);
          if (st.settings.frameExParams.size() >= 5) st.settings.frameExParams[4] = 50;
        }
        st.runtime = &app.runtime;
        
        // Discover voice profiles from phonemes.yaml
        st.voiceProfiles = app.runtime.discoverVoiceProfiles();

        ShowSpeechSettingsDialog(app.hInst, hWnd, st);
        if (st.ok) {
          app.runtime.setSpeechSettings(st.settings);
          saveSpeechSettingsToIni(st.settings);
          app.setStatus(L"Updated speech settings.");
        }
        return 0;
      }

      if (id == IDM_EDITOR_DICT) {
        if (app.packsDir.empty()) {
          msgBox(hWnd, L"Open a pack root first.", L"Dictionary Editor", MB_ICONINFORMATION);
          return 0;
        }
        tgsb_editor::DictionaryEditorState dst;
        dst.packDir = app.packsDir;
        // Discover available languages from YAML files.
        {
          fs::path langDirPath = fs::path(app.packsDir) / "lang";
          if (fs::exists(langDirPath)) {
            std::vector<std::string> langs;
            for (auto& e : fs::directory_iterator(langDirPath)) {
              if (!e.is_regular_file()) continue;
              auto p = e.path();
              if (p.extension() == ".yaml" || p.extension() == ".yml") {
                if (p.stem().u8string() != "default")
                  langs.push_back(p.stem().u8string());
              }
            }
            std::sort(langs.begin(), langs.end());
            dst.availableLangs = std::move(langs);
          }
        }
        dst.initialLang = selectedLangTagUtf8(app);
        // Wire up IPA conversion callback.
        dst.convertToIpa = [&app](const std::wstring& text, std::string& ipa, std::string& err) -> bool {
          return convertTextToIpaViaPhonemizer(app, text, ipa, err);
        };
        // Wire up phoneme key list callback.
        dst.getPhonemeKeys = [&app]() -> std::vector<tgsb_editor::PhonemeKeyInfo> {
          std::vector<tgsb_editor::PhonemeKeyInfo> result;
          for (const auto& k : app.phonemeKeys) {
            tgsb_editor::PhonemeKeyInfo info;
            info.key = k;
            // Look up the phoneme class from YAML flags.
            const auto* node = app.phonemes.getPhonemeNode(k);
            if (node) {
              auto flag = [&](const char* f) -> bool {
                auto it = node->map.find(f);
                if (it == node->map.end()) return false;
                bool v = false;
                return it->second.asBool(v) && v;
              };
              if (flag("_isVowel")) info.cls = "vowel";
              else if (flag("_isStop")) info.cls = "stop";
              else if (flag("_isNasal")) info.cls = "nasal";
              else if (flag("_isLiquid")) info.cls = "liquid";
              else info.cls = "consonant";
            }
            result.push_back(std::move(info));
          }
          return result;
        };
        // Wire up phoneme preview callback.
        dst.previewPhoneme = [&app](const std::string& phonemeKey) {
          if (!ensureDllDir(app)) return;
          tgsb_editor::Node* node = app.phonemes.getPhonemeNode(phonemeKey);
          if (!node || !node->isMap()) return;
          std::vector<sample> samples;
          std::string err;
          if (app.runtime.synthPreviewPhoneme(*node, kSampleRate, samples, err))
            playSamplesTemp(app, samples);
        };
        tgsb_editor::ShowDictionaryEditorDialog(app.hInst, hWnd, dst);
        return 0;
      }

      if (id == IDM_EDITOR_VOICES) {
        if (app.packsDir.empty()) {
          msgBox(hWnd, L"Open a pack root first.", L"Voice Profiles", MB_ICONINFORMATION);
          return 0;
        }
        
        std::wstring yamlPath = app.packsDir;
        if (!yamlPath.empty() && yamlPath.back() != L'\\' && yamlPath.back() != L'/') {
          yamlPath += L'\\';
        }
        yamlPath += L"phonemes.yaml";
        
        tgsb_editor::VoiceProfilesDialogState vpst;
        vpst.phonemesYamlPath = yamlPath;
        
        std::string loadErr;
        if (!tgsb_editor::loadVoiceProfilesFromYaml(yamlPath, vpst.profiles, loadErr)) {
          msgBox(hWnd, (L"Could not load voice profiles: " + utf8ToWide(loadErr)).c_str(), L"Voice Profiles", MB_ICONERROR);
          return 0;
        }
        
        if (tgsb_editor::ShowVoiceProfilesDialog(app.hInst, hWnd, vpst) && vpst.ok && vpst.modified) {
          std::string saveErr;
          if (tgsb_editor::saveVoiceProfilesToYaml(yamlPath, vpst.profiles, saveErr)) {
            app.setStatus(L"Saved voice profiles to phonemes.yaml.");
          } else {
            msgBox(hWnd, (L"Could not save voice profiles: " + utf8ToWide(saveErr)).c_str(), L"Voice Profiles", MB_ICONERROR);
          }
        }
        return 0;
      }

      if (id == IDM_EDITOR_ALLOPHONES) {
        if (!app.language.isLoaded()) {
          msgBox(hWnd, L"Load a language first.", L"Allophone Rules", MB_ICONINFORMATION);
          return 0;
        }
        tgsb_editor::AllophoneRulesDialogState arst;
        arst.rules = app.language.allophoneRules();
        arst.language = &app.language;
        if (tgsb_editor::ShowAllophoneRulesDialog(app.hInst, hWnd, arst) && arst.modified) {
          app.languageDirty = true;
          app.setStatus(L"Updated allophone rules.");
        }
        return 0;
      }

      if (id == IDM_EDITOR_SPECIAL_COARTIC) {
        if (!app.language.isLoaded()) {
          msgBox(hWnd, L"Load a language first.", L"Special Coarticulation", MB_ICONINFORMATION);
          return 0;
        }
        tgsb_editor::SpecialCoarticDialogState scst;
        scst.rules = app.language.specialCoarticRules();
        scst.language = &app.language;
        if (tgsb_editor::ShowSpecialCoarticDialog(app.hInst, hWnd, scst) && scst.modified) {
          app.languageDirty = true;
          app.setStatus(L"Updated special coarticulation rules.");
        }
        return 0;
      }

      if (id == IDM_HELP_ABOUT) {
        msgBox(hWnd,
               L"TGSpeechBox Phoneme Editor (Win32)\n\n"
               L"Keyboard shortcuts:\n"
               L"  Ctrl+O       Open pack root\n"
               L"  Ctrl+S       Save language YAML\n"
               L"  Ctrl+P       Save phonemes YAML\n"
               L"  F5           Reload language YAML\n"
               L"  Shift+F5     Reload phonemes YAML\n\n"
               L"Notes:\n"
               L"  - This editor rewrites YAML (comments are not preserved).\n"
               L"  - Preview audio uses speechPlayer.dll.\n"
               L"  - Text->IPA uses eSpeak if configured.",
               L"About", MB_OK | MB_ICONINFORMATION);
        return 0;
      }

      // Control notifications
      if (id == IDC_EDIT_FILTER && code == EN_CHANGE) {
        wchar_t buf[512];
        GetWindowTextW(app.editFilter, buf, 512);
        populatePhonemeList(app, buf);
        return 0;
      }

      if (id == IDC_COMBO_LANGUAGE && code == CBN_SELCHANGE) {
        // Check for unsaved language changes before switching.
        if (app.languageDirty) {
          int res = MessageBoxW(hWnd, 
            L"You have unsaved changes to the current language YAML.\n\nSwitch to a different language without saving?",
            L"Unsaved Changes", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
          if (res != IDYES) {
            // Revert combo selection to current language.
            // Note: We don't track the previous index, so just leave as-is.
            // The user should save or the change won't persist anyway.
            return 0;
          }
        }
        int sel = static_cast<int>(SendMessageW(app.comboLang, CB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < static_cast<int>(app.languageFiles.size())) {
          loadLanguage(app, app.languageFiles[static_cast<size_t>(sel)]);
        }
        return 0;
      }

      switch (id) {
        case IDC_BTN_PLAY_PHONEME:
          onPlaySelectedPhoneme(app, false);
          return 0;
        case IDC_BTN_CLONE_PHONEME:
          onClonePhoneme(app);
          return 0;
        case IDC_BTN_EDIT_PHONEME:
          onEditSelectedPhoneme(app, false);
          return 0;
        case IDC_BTN_ADD_TO_LANGUAGE: {
          std::string key = getSelectedPhonemeKey(app.listPhonemes);
          if (key.empty()) {
            msgBox(hWnd, L"Select a phoneme first.", L"TGSB Phoneme Editor", MB_ICONINFORMATION);
            return 0;
          }
          onAddMapping(app, key);
          return 0;
        }
        case IDC_BTN_LANG_PLAY_PHONEME:
          onPlaySelectedPhoneme(app, true);
          return 0;
        case IDC_BTN_LANG_EDIT_PHONEME:
          onEditSelectedPhoneme(app, true);
          return 0;
        case IDC_BTN_LANG_SETTINGS:
          onEditLanguageSettings(app);
          return 0;
        case IDC_BTN_ADD_MAPPING:
          onAddMapping(app);
          return 0;
        case IDC_BTN_EDIT_MAPPING:
          onEditSelectedMapping(app);
          return 0;
        case IDC_BTN_REMOVE_MAPPING:
          onRemoveSelectedMapping(app);
          return 0;
        case IDC_BTN_ADD_SKIP_RULE:
          onAddSkipRule(app);
          return 0;
        case IDC_BTN_REMOVE_SKIP_RULE:
          onRemoveSelectedSkipRule(app);
          return 0;
        case IDC_BTN_CONVERT_IPA:
          onConvertIpa(app);
          return 0;
        case IDC_BTN_SPEAK:
          onSpeak(app);
          return 0;
        case IDC_BTN_SAVE_WAV:
          onSaveWav(app);
          return 0;
        default:
          break;
      }

      break;
    }

    case WM_NOTIFY: {
      NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);
      if (hdr && hdr->code == NM_SETFOCUS) {
        // Track focus for restoration on WM_ACTIVATE.
        if (hdr->hwndFrom && IsChild(hWnd, hdr->hwndFrom)) {
          app.lastFocus = hdr->hwndFrom;
        }

        wchar_t cls[64] = {0};
        GetClassNameW(hdr->hwndFrom, cls, 64);
        if (_wcsicmp(cls, WC_LISTVIEWW) == 0 || _wcsicmp(cls, L"SysListView32") == 0) {
          EnsureListViewHasSelection(hdr->hwndFrom);
        }
      }
      return 0;
    }

    case WM_CLOSE: {
      // Check for unsaved changes.
      if (app.phonemesDirty || app.languageDirty) {
        std::wstring msg = L"You have unsaved changes:\n";
        if (app.phonemesDirty) msg += L"  - Phonemes YAML\n";
        if (app.languageDirty) msg += L"  - Language YAML\n";
        msg += L"\nDo you want to quit without saving?";

        int res = MessageBoxW(hWnd, msg.c_str(), L"Unsaved Changes", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (res != IDYES) {
          return 0; // User cancelled, don't close.
        }
      }
      DestroyWindow(hWnd);
      return 0;
    }

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }

  return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// -------------------------
// Keyboard focus / tab order support
//
// This app uses a normal top-level window (not a dialog). In that setup, the
// Win32 dialog manager does NOT automatically move focus between WS_TABSTOP
// controls when the user presses Tab / Shift+Tab.
//
// We implement a small, predictable tab-navigation handler here so all
// controls are reachable by keyboard, which is important for screen readers.
static bool handleTabNavigation(HWND hWnd, const MSG& msg) {
  if (msg.message != WM_KEYDOWN || msg.wParam != VK_TAB) return false;

  // Only handle Tab when the message is destined for our main window or one of
  // its child controls.
  if (!(msg.hwnd == hWnd || IsChild(hWnd, msg.hwnd))) return false;

  const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

  HWND focused = GetFocus();
  if (!(focused && (focused == hWnd || IsChild(hWnd, focused)))) {
    focused = nullptr;
  }

  HWND next = GetNextDlgTabItem(hWnd, focused, shift ? TRUE : FALSE);

  // Defensive fallback: in case the dialog-manager helper doesn't return a
  // control (it is documented for dialogs, though it generally works for
  // any parent window), we enumerate WS_TABSTOP children manually.
  if (!next) {
    std::vector<HWND> tabStops;
    for (HWND child = GetWindow(hWnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
      LONG style = GetWindowLongW(child, GWL_STYLE);
      if ((style & WS_TABSTOP) && (style & WS_VISIBLE) && IsWindowEnabled(child)) {
        tabStops.push_back(child);
      }
    }

    // Child enumeration returns windows in Z-order (topmost first). Tab order
    // is generally the reverse (older controls first), so reverse to keep a
    // natural, creation-order traversal.
    std::reverse(tabStops.begin(), tabStops.end());

    if (tabStops.empty()) return false;

    auto it = std::find(tabStops.begin(), tabStops.end(), focused);
    if (it == tabStops.end()) {
      next = shift ? tabStops.back() : tabStops.front();
    } else {
      const ptrdiff_t idx = it - tabStops.begin();
      const ptrdiff_t n = static_cast<ptrdiff_t>(tabStops.size());
      const ptrdiff_t nextIdx = shift ? ((idx - 1 + n) % n) : ((idx + 1) % n);
      next = tabStops[static_cast<size_t>(nextIdx)];
    }
  }

  if (!next) return false;

  SetFocus(next);
  return true;
}

// Enable Ctrl+A (Select All) in EDIT controls.
// The standard Win32 EDIT control does not implement this shortcut by default,
// so we provide it to make text selection predictable.
static bool handleCtrlASelectAll(HWND hWnd, const MSG& msg) {
  if (msg.message != WM_KEYDOWN) return false;
  if ((GetKeyState(VK_CONTROL) & 0x8000) == 0) return false;
  if (msg.wParam != 'A' && msg.wParam != 'a') return false;

  // Only handle when the focused control is one of our children.
  HWND focused = GetFocus();
  if (!(focused && (focused == hWnd || IsChild(hWnd, focused)))) return false;

  wchar_t cls[32] = {0};
  GetClassNameW(focused, cls, 32);
  if (_wcsicmp(cls, L"Edit") != 0) return false;

  SendMessageW(focused, EM_SETSEL, 0, -1);
  return true;
}

// Handle Alt+key shortcuts for button actions.
// This must be done in the message loop because WM_SYSKEYDOWN doesn't reliably
// reach the window procedure when child controls have focus.
static bool handleAltShortcuts(HWND hWnd, const MSG& msg) {
  if (msg.message != WM_SYSKEYDOWN) return false;

  // Check if Alt is held down.
  if ((GetKeyState(VK_MENU) & 0x8000) == 0) return false;

  // Only handle when the message is for our main window or one of its children.
  if (!(msg.hwnd == hWnd || IsChild(hWnd, msg.hwnd))) return false;

  int cmdId = 0;
  switch (msg.wParam) {
    case 'P': cmdId = IDC_BTN_PLAY_PHONEME; break;      // Alt+P: Play
    case 'C': cmdId = IDC_BTN_CLONE_PHONEME; break;     // Alt+C: Clone
    case 'E': cmdId = IDC_BTN_EDIT_PHONEME; break;      // Alt+E: Edit
    case 'G': cmdId = IDC_BTN_ADD_TO_LANGUAGE; break;   // Alt+G: Add to language
    case 'L': cmdId = IDC_BTN_LANG_PLAY_PHONEME; break; // Alt+L: Play from language
    case 'D': cmdId = IDC_BTN_LANG_EDIT_PHONEME; break; // Alt+D: Edit phoneme in language
    case 'S': cmdId = IDC_BTN_LANG_SETTINGS; break;     // Alt+S: Language settings
    case 'A': cmdId = IDC_BTN_ADD_MAPPING; break;       // Alt+A: Add mapping
    case 'M': cmdId = IDC_BTN_EDIT_MAPPING; break;      // Alt+M: Edit mapping
    case 'R': cmdId = IDC_BTN_REMOVE_MAPPING; break;    // Alt+R: Remove mapping
    case 'I': cmdId = IDC_BTN_CONVERT_IPA; break;       // Alt+I: Convert to IPA
    case 'K': cmdId = IDC_BTN_SPEAK; break;             // Alt+K: Speak
    case 'W': cmdId = IDC_BTN_SAVE_WAV; break;          // Alt+W: Save WAV
    default: return false;
  }

  // Send the button click command to the main window.
  HWND btn = GetDlgItem(hWnd, cmdId);
  if (btn && IsWindowEnabled(btn)) {
    SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(cmdId, BN_CLICKED), (LPARAM)btn);
    return true;
  }
  return false;
}


// -------------------------
// WinMain
// -------------------------