#define UNICODE
#define _UNICODE

#include "AppController.h"

#include "AccessibilityUtils.h"
#include "Dialogs.h"
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
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

// yaml_edit.* and nvsp_runtime.* live in the nvsp_editor namespace.
using nvsp_editor::ReplacementRule;
using nvsp_editor::ReplacementWhen;
using nvsp_editor::NvspRuntime;

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
  wc.lpszClassName = L"NvspPhonemeEditorWnd";
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

  RegisterClassW(&wc);

  HMENU hMenu = LoadMenuW(hInstance, MAKEINTRESOURCEW(IDR_MAINMENU));

  wnd = CreateWindowExW(
    0,
    wc.lpszClassName,
    L"NV Speech Player - Phoneme Editor",
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

static void refreshLanguageDerivedLists(AppController& app) {
  app.usedPhonemeKeys = extractUsedPhonemes(app, app.repls);
  populateMappingsList(app);
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
    L"NVSP Phoneme Editor",
    MB_YESNO | MB_ICONQUESTION
  );

  if (res != IDYES) return true; // allow editor to still work

  try {
    fs::copy_file(good, phonemes, fs::copy_options::overwrite_existing);
    return true;
  } catch (...) {
    msgBox(owner, L"Failed to copy phonemes-good.yaml to phonemes.yaml.", L"NVSP Phoneme Editor", MB_ICONERROR);
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
    msgBox(app.wnd, L"Failed to load phonemes YAML:\n" + utf8ToWide(err), L"NVSP Phoneme Editor", MB_ICONERROR);
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
    msgBox(app.wnd, L"Failed to load language YAML:\n" + utf8ToWide(err), L"NVSP Phoneme Editor", MB_ICONERROR);
    return false;
  }

  app.repls = app.language.replacements();
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
    msgBox(app.wnd, L"That folder doesn't contain a 'packs' subfolder.", L"NVSP Phoneme Editor", MB_ICONERROR);
    return false;
  }

  app.packRoot = root;
  app.packsDir = packs.wstring();
  app.langDir = (packs / "lang").wstring();

  maybeCopyGoodPhonemesToExpected(app.wnd, app.packsDir);

  if (!loadPhonemes(app, app.packsDir)) {
    msgBox(app.wnd, L"Couldn't find phonemes.yaml or phonemes-good.yaml under packs/.", L"NVSP Phoneme Editor", MB_ICONERROR);
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
    msgBox(app.wnd, L"DLL load failed:\n" + utf8ToWide(err) + L"\n\nUse Settings > Set DLL directory...", L"NVSP Phoneme Editor", MB_ICONERROR);
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
    msgBox(app.wnd, L"No audio samples were generated.", L"NVSP Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  std::wstring wavPath = nvsp_editor::makeTempWavPath(L"nvp");
  std::string err;
  if (!nvsp_editor::writeWav16Mono(wavPath, kSampleRate, samples, err)) {
    msgBox(app.wnd, L"WAV write failed:\n" + utf8ToWide(err), L"NVSP Phoneme Editor", MB_ICONERROR);
    return;
  }

  PlaySoundW(wavPath.c_str(), NULL, SND_FILENAME | SND_ASYNC);
}

static void onPlaySelectedPhoneme(AppController& app, bool fromLanguageList) {
  if (!ensureDllDir(app)) return;

  std::string key = fromLanguageList ? getSelectedPhonemeKey(app.listLangPhonemes) : getSelectedPhonemeKey(app.listPhonemes);
  if (key.empty()) {
    msgBox(app.wnd, L"Select a phoneme first.", L"NVSP Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  nvsp_editor::Node* node = app.phonemes.getPhonemeNode(key);
  if (!node || !node->isMap()) {
    msgBox(app.wnd, L"Phoneme not found in phonemes.yaml.", L"NVSP Phoneme Editor", MB_ICONERROR);
    return;
  }

  std::vector<sample> samples;
  std::string err;
  if (!app.runtime.synthPreviewPhoneme(*node, kSampleRate, samples, err)) {
    msgBox(app.wnd, L"Preview failed:\n" + utf8ToWide(err), L"NVSP Phoneme Editor", MB_ICONERROR);
    return;
  }

  playSamplesTemp(app, samples);
}

// -------------------------
// Mapping operations
// -------------------------
static void onAddMapping(AppController& app, const std::string& defaultTo = {}) {
  if (!app.language.isLoaded()) {
    msgBox(app.wnd, L"Load a language first.", L"NVSP Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  AddMappingDialogState st;
  st.rule.to = defaultTo;
  st.classNames = app.classNames;

  ShowAddMappingDialog(app.hInst, app.wnd, st);
  if (!st.ok) return;

  app.repls.push_back(st.rule);
  app.language.setReplacements(app.repls);
  app.languageDirty = true;
  refreshLanguageDerivedLists(app);
}

static void onEditSelectedMapping(AppController& app) {
  int sel = lvSelectedIndex(app.listMappings);
  if (sel < 0 || sel >= static_cast<int>(app.repls.size())) {
    msgBox(app.wnd, L"Select a mapping first.", L"NVSP Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  AddMappingDialogState st;
  st.rule = app.repls[static_cast<size_t>(sel)];
  st.classNames = app.classNames;

  ShowAddMappingDialog(app.hInst, app.wnd, st);
  if (!st.ok) return;

  app.repls[static_cast<size_t>(sel)] = st.rule;
  app.language.setReplacements(app.repls);
  app.languageDirty = true;
  refreshLanguageDerivedLists(app);
}

static void onRemoveSelectedMapping(AppController& app) {
  int sel = lvSelectedIndex(app.listMappings);
  if (sel < 0 || sel >= static_cast<int>(app.repls.size())) {
    msgBox(app.wnd, L"Select a mapping first.", L"NVSP Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  app.repls.erase(app.repls.begin() + sel);
  app.language.setReplacements(app.repls);
  app.languageDirty = true;
  refreshLanguageDerivedLists(app);
}

// -------------------------
// Language settings
// -------------------------
static std::vector<std::string> knownLanguageSettingKeys() {
  return {
    // Alphabetically sorted list of all language pack settings
    "applyLengthenedScaleToVowelsOnly",
    "autoDiphthongOffglideToSemivowel",
    "autoTieDiphthongs",
    "boundarySmoothingEnabled",
    "boundarySmoothingStopToVowelFadeMs",
    "boundarySmoothingVowelToFricFadeMs",
    "boundarySmoothingVowelToStopFadeMs",
    "coarticulationAdjacencyMaxConsonants",
    "coarticulationAlveolarF2Locus",
    "coarticulationEnabled",
    "coarticulationFadeIntoConsonants",
    "coarticulationGraduated",
    "coarticulationLabialF2Locus",
    "coarticulationStrength",
    "coarticulationTransitionExtent",
    "coarticulationVelarF2Locus",
    "coarticulationVelarPinchEnabled",
    "coarticulationVelarPinchF2Scale",
    "coarticulationVelarPinchF3",
    "coarticulationVelarPinchThreshold",
    "coarticulationWordInitialFadeScale",
    "defaultGlottalOpenQuotient",
    "defaultOutputGain",
    "defaultPreFormantGain",
    "defaultVibratoPitchOffset",
    "defaultVibratoSpeed",
    "defaultVoiceTurbulenceAmplitude",
    "englishLongUKey",
    "englishLongUShortenEnabled",
    "englishLongUWordFinalScale",
    "huShortAVowelEnabled",
    "huShortAVowelKey",
    "huShortAVowelScale",
    "legacyPitchInflectionScale",
    "legacyPitchMode",
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
    "microprosodyMinVowelMs",
    "microprosodyVoicedF0LowerDeltaHz",
    "microprosodyVoicedF0LowerEnabled",
    "microprosodyVoicelessF0RaiseDeltaHz",
    "microprosodyVoicelessF0RaiseEnabled",
    "microprosodyVoicelessF0RaiseEndDeltaHz",
    "nasalizationAnticipatoryAmplitude",
    "nasalizationAnticipatoryBlend",
    "nasalizationAnticipatoryEnabled",
    "nasalizationAnticipatoryMs",
    "phraseFinalLengtheningEnabled",
    "phraseFinalLengtheningFinalSyllableScale",
    "phraseFinalLengtheningNucleusOnlyMode",
    "phraseFinalLengtheningPenultimateSyllableScale",
    "phraseFinalLengtheningQuestionScale",
    "phraseFinalLengtheningStatementScale",
    "positionalAllophonesEnabled",
    "positionalAllophonesGlottalReinforcementDurationMs",
    "positionalAllophonesGlottalReinforcementEnabled",
    "positionalAllophonesLateralDarkF2TargetHz",
    "positionalAllophonesLateralDarknessPostVocalic",
    "positionalAllophonesLateralDarknessPreVocalic",
    "positionalAllophonesLateralDarknessSyllabic",
    "positionalAllophonesStopAspirationIntervocalic",
    "positionalAllophonesStopAspirationWordFinal",
    "positionalAllophonesStopAspirationWordInitial",
    "positionalAllophonesStopAspirationWordInitialStressed",
    "postStopAspirationEnabled",
    "postStopAspirationPhoneme",
    "primaryStressDiv",
    "rateReductionEnabled",
    "rateReductionSchwaMinDurationMs",
    "rateReductionSchwaReductionThreshold",
    "rateReductionSchwaScale",
    "secondaryStressDiv",
    "segmentBoundaryFadeMs",
    "segmentBoundaryGapMs",
    "segmentBoundarySkipVowelToLiquid",
    "segmentBoundarySkipVowelToVowel",
    "semivowelOffglideScale",
    "spellingDiphthongMode",
    "stopClosureAfterNasalsEnabled",
    "stopClosureClusterFadeMs",
    "stopClosureClusterGapMs",
    "stopClosureClusterGapsEnabled",
    "stopClosureMode",
    "stopClosureVowelFadeMs",
    "stopClosureVowelGapMs",
    "stopClosureWordBoundaryClusterFadeMs",
    "stopClosureWordBoundaryClusterGapMs",
    "stressedVowelHiatusFadeMs",
    "stressedVowelHiatusGapMs",
    "stripAllophoneDigits",
    "stripHyphen",
    "tonal",
    "toneContoursAbsolute",
    "toneContoursMode",
    "toneDigitsEnabled",
    "trajectoryLimitApplyAcrossWordBoundary",
    "trajectoryLimitApplyTo",
    "trajectoryLimitEnabled",
    "trajectoryLimitMaxHzPerMsCf2",
    "trajectoryLimitMaxHzPerMsCf3",
    "trajectoryLimitWindowMs",
    "trillModulationFadeMs",
    "trillModulationMs",
  };
}

// -------------------------
// Speech settings (voice + sliders)
// -------------------------

static void onEditLanguageSettings(AppController& app) {
  if (!app.language.isLoaded()) {
    msgBox(app.wnd, L"Load a language first.", L"NVSP Phoneme Editor", MB_ICONINFORMATION);
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
    msgBox(app.wnd, L"Clone failed:\n" + utf8ToWide(err), L"NVSP Phoneme Editor", MB_ICONERROR);
    return;
  }

  // Reload list.
  app.phonemeKeys = app.phonemes.phonemeKeysSorted();
  rebuildPhonemeKeysU32(app);
  populatePhonemeList(app, L"");
  app.phonemesDirty = true;

  msgBox(app.wnd, L"Cloned phoneme. Remember to save phonemes YAML (Ctrl+P).", L"NVSP Phoneme Editor", MB_ICONINFORMATION);
}

static void onEditSelectedPhoneme(AppController& app, bool fromLanguageList) {
  std::string key = fromLanguageList ? getSelectedPhonemeKey(app.listLangPhonemes) : getSelectedPhonemeKey(app.listPhonemes);
  if (key.empty()) {
    msgBox(app.wnd, L"Select a phoneme first.", L"NVSP Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  nvsp_editor::Node* node = app.phonemes.getPhonemeNode(key);
  if (!node || !node->isMap()) {
    msgBox(app.wnd, L"Phoneme not found in phonemes.yaml.", L"NVSP Phoneme Editor", MB_ICONERROR);
    return;
  }

  EditPhonemeDialogState st;
  st.phonemeKey = key;
  st.original = *node;
  st.working = *node;
  st.runtime = &app.runtime;
  st.runtime = &app.runtime;

  ShowEditPhonemeDialog(app.hInst, app.wnd, st);
  if (!st.ok) return;

  *node = st.working;
  app.phonemesDirty = true;
  msgBox(app.wnd, L"Phoneme updated. Remember to save phonemes YAML (Ctrl+P).", L"NVSP Phoneme Editor", MB_ICONINFORMATION);
}

// -------------------------
// Save YAML
// -------------------------
static void onSaveLanguage(AppController& app) {
  if (!app.language.isLoaded()) {
    msgBox(app.wnd, L"No language YAML loaded.", L"NVSP Phoneme Editor", MB_ICONINFORMATION);
    return;
  }
  std::string err;
  if (!app.language.save(err)) {
    msgBox(app.wnd, L"Save failed:\n" + utf8ToWide(err), L"NVSP Phoneme Editor", MB_ICONERROR);
    return;
  }

  // Reload from disk to sync with any external changes (e.g., edits made in a text editor).
  std::string langPath = app.language.path();
  if (!app.language.load(langPath, err)) {
    msgBox(app.wnd, L"Warning: Failed to reload language YAML after save:\n" + utf8ToWide(err), L"NVSP Phoneme Editor", MB_ICONWARNING);
  } else {
    app.repls = app.language.replacements();
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

static void onSavePhonemes(AppController& app) {
  if (!app.phonemes.isLoaded()) {
    msgBox(app.wnd, L"No phonemes YAML loaded.", L"NVSP Phoneme Editor", MB_ICONINFORMATION);
    return;
  }
  std::string err;
  if (!app.phonemes.save(err)) {
    msgBox(app.wnd, L"Save failed:\n" + utf8ToWide(err), L"NVSP Phoneme Editor", MB_ICONERROR);
    return;
  }

  // Reload from disk to sync with any external changes (e.g., edits made in a text editor).
  std::string phonemesPath = app.phonemes.path();
  if (!app.phonemes.load(phonemesPath, err)) {
    msgBox(app.wnd, L"Warning: Failed to reload phonemes YAML after save:\n" + utf8ToWide(err), L"NVSP Phoneme Editor", MB_ICONWARNING);
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
    msgBox(app.wnd, L"No language YAML loaded.", L"NVSP Phoneme Editor", MB_ICONINFORMATION);
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
    msgBox(app.wnd, L"Failed to reload language YAML:\n" + utf8ToWide(err), L"NVSP Phoneme Editor", MB_ICONERROR);
    return;
  }

  app.repls = app.language.replacements();
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
    msgBox(app.wnd, L"No phonemes YAML loaded.", L"NVSP Phoneme Editor", MB_ICONINFORMATION);
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
    msgBox(app.wnd, L"Failed to reload phonemes YAML:\n" + utf8ToWide(err), L"NVSP Phoneme Editor", MB_ICONERROR);
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

  // Config lives in nvspPhonemeEditor.ini.
  //
  // If [phonemizer].exe is empty, we use the configured eSpeak directory and call
  // espeak-ng.exe/espeak.exe.
  //
  // This is intentionally CLI-only (no DLL loading) to keep licensing simpler and
  // to let advanced users point the tool at other phonemizers.
  nvsp_editor::CliPhonemizerConfig cfg;
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
    cfg.espeakDataDir = nvsp_editor::findEspeakDataDir(app.espeakDir);

    cfg.exePath = nvsp_editor::findEspeakExe(app.espeakDir);
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

  return nvsp_editor::phonemizeTextToIpa(cfg, langTag, text, outIpaUtf8, outError);
}

static void onConvertIpa(AppController& app) {
  std::wstring text = getText(app.editText);
  if (text.empty()) {
    msgBox(app.wnd, L"Enter some text first.", L"NVSP Phoneme Editor", MB_ICONINFORMATION);
    return;
  }

  std::string ipa;
  std::string err;
  if (!convertTextToIpaViaPhonemizer(app, text, ipa, err)) {
    msgBox(app.wnd, L"IPA conversion failed:\n" + utf8ToWide(err) + L"\n\nTip: you can also tick 'Input is IPA' and paste IPA directly.", L"NVSP Phoneme Editor", MB_ICONERROR);
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
  if (inputIsIpa) {
    ipa = wideToUtf8(text);
  } else {
    std::string err;
    if (!convertTextToIpaViaPhonemizer(app, text, ipa, err)) {
      outError = err;
      return false;
    }
    setText(app.editIpaOut, utf8ToWide(ipa));
  }

  return app.runtime.synthIpa(ipa, kSampleRate, outSamples, outError);
}

static void onSpeak(AppController& app) {
  std::vector<sample> samples;
  std::string err;
  if (!synthIpaFromUi(app, samples, err)) {
    msgBox(app.wnd, L"Speak failed:\n" + utf8ToWide(err) + L"\n\nIf this mentions phonemes.yaml, make sure packs/phonemes.yaml exists.", L"NVSP Phoneme Editor", MB_ICONERROR);
    return;
  }
  playSamplesTemp(app, samples);
}

static void onSaveWav(AppController& app) {
  std::vector<sample> samples;
  std::string err;
  if (!synthIpaFromUi(app, samples, err)) {
    msgBox(app.wnd, L"Synthesis failed:\n" + utf8ToWide(err), L"NVSP Phoneme Editor", MB_ICONERROR);
    return;
  }

  std::wstring outPath;
  if (!pickSaveWav(app.wnd, outPath)) return;

  if (!nvsp_editor::writeWav16Mono(outPath, kSampleRate, samples, err)) {
    msgBox(app.wnd, L"WAV write failed:\n" + utf8ToWide(err), L"NVSP Phoneme Editor", MB_ICONERROR);
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

  MoveWindow(app.lblMappings, xR, mapY, rightW, labelH, TRUE);
  mapY += labelH + labelGap;
  MoveWindow(app.listMappings, xR, mapY, rightW, topH - mapY - mapBtnAreaH + margin, TRUE);

  int mapBtnY = topH - mapBtnH + margin;
  int mapBtnW = (rightW - margin * 2) / 3;
  MoveWindow(app.btnAddMap, xR, mapBtnY, mapBtnW, mapBtnH, TRUE);
  MoveWindow(app.btnEditMap, xR + mapBtnW + margin, mapBtnY, mapBtnW, mapBtnH, TRUE);
  MoveWindow(app.btnRemoveMap, xR + (mapBtnW + margin) * 2, mapBtnY, mapBtnW, mapBtnH, TRUE);

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
            msgBox(hWnd, L"DLL load failed:\n" + utf8ToWide(err), L"NVSP Phoneme Editor", MB_ICONERROR);
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
        st.paramNames = std::vector<std::string>(NvspRuntime::frameParamNames().begin(), NvspRuntime::frameParamNames().end());
        if (st.settings.frameParams.size() != st.paramNames.size()) {
          st.settings.frameParams.assign(st.paramNames.size(), 50);
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

      if (id == IDM_HELP_ABOUT) {
        msgBox(hWnd,
               L"NV Speech Player Phoneme Editor (Win32)\n\n"
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
            msgBox(hWnd, L"Select a phoneme first.", L"NVSP Phoneme Editor", MB_ICONINFORMATION);
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
