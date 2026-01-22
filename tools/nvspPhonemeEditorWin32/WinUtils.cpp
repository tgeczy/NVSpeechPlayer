#define UNICODE
#define _UNICODE

#include "WinUtils.h"

#include <commdlg.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <string>

std::wstring utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (len <= 1) return {};
  std::wstring out(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
  out.resize(static_cast<size_t>(len - 1));
  return out;
}

std::string wideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return {};
  std::string out(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
  out.resize(static_cast<size_t>(len - 1));
  return out;
}

std::wstring exeDir() {
  wchar_t buf[MAX_PATH] = {0};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  PathRemoveFileSpecW(buf);
  return buf;
}

std::wstring iniPath() {
  std::wstring p = exeDir();
  if (!p.empty() && p.back() != L'\\') p += L'\\';
  p += L"nvspPhonemeEditor.ini";
  return p;
}

std::wstring readIni(const wchar_t* section, const wchar_t* key, const wchar_t* def) {
  wchar_t buf[2048] = {0};
  GetPrivateProfileStringW(section, key, def, buf, 2048, iniPath().c_str());
  return buf;
}

void writeIni(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
  WritePrivateProfileStringW(section, key, value.c_str(), iniPath().c_str());
}

int readIniInt(const wchar_t* section, const wchar_t* key, int defVal) {
  return static_cast<int>(GetPrivateProfileIntW(section, key, defVal, iniPath().c_str()));
}

void writeIniInt(const wchar_t* section, const wchar_t* key, int value) {
  wchar_t buf[64];
  _itow_s(value, buf, 10);
  WritePrivateProfileStringW(section, key, buf, iniPath().c_str());
}

void msgBox(HWND owner, const std::wstring& text, const std::wstring& title, UINT flags) {
  // Preserve keyboard focus across modal message boxes.
  HWND prevFocus = GetFocus();
  MessageBoxW(owner, text.c_str(), title.c_str(), flags);
  if (prevFocus && IsWindow(prevFocus) && IsWindowEnabled(prevFocus) && IsWindowVisible(prevFocus)) {
    SetFocus(prevFocus);
  }
}

bool pickFolder(HWND owner, const wchar_t* title, std::wstring& outFolder) {
  outFolder.clear();

  // Preserve keyboard focus across modal dialogs.
  HWND prevFocus = GetFocus();
  auto restoreFocus = [&]() {
    if (prevFocus && IsWindow(prevFocus) && IsWindowEnabled(prevFocus) && IsWindowVisible(prevFocus)) {
      SetFocus(prevFocus);
    }
  };

  IFileDialog* pDlg = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
  if (FAILED(hr) || !pDlg) {
    restoreFocus();
    return false;
  }

  DWORD opts = 0;
  pDlg->GetOptions(&opts);
  pDlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  pDlg->SetTitle(title);

  hr = pDlg->Show(owner);
  if (FAILED(hr)) {
    pDlg->Release();
    restoreFocus();
    return false;
  }

  IShellItem* pItem = nullptr;
  hr = pDlg->GetResult(&pItem);
  if (SUCCEEDED(hr) && pItem) {
    PWSTR pszPath = nullptr;
    hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
    if (SUCCEEDED(hr) && pszPath) {
      outFolder = pszPath;
      CoTaskMemFree(pszPath);
    }
    pItem->Release();
  }

  pDlg->Release();
  restoreFocus();
  return !outFolder.empty();
}



bool pickOpenExe(HWND owner, std::wstring& outPath) {
  outPath.clear();

  // Preserve keyboard focus across modal dialogs.
  HWND prevFocus = GetFocus();
  auto restoreFocus = [&]() {
    if (prevFocus && IsWindow(prevFocus) && IsWindowEnabled(prevFocus) && IsWindowVisible(prevFocus)) {
      SetFocus(prevFocus);
    }
  };

  wchar_t fileBuf[MAX_PATH] = {0};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFile = fileBuf;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = L"Executable files (*.exe)\0*.exe\0All files\0*.*\0";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (!GetOpenFileNameW(&ofn)) {
    restoreFocus();
    return false;
  }

  outPath = fileBuf;
  restoreFocus();
  return !outPath.empty();
}

bool pickSaveWav(HWND owner, std::wstring& outPath) {
  outPath.clear();

  // Preserve keyboard focus across modal dialogs.
  HWND prevFocus = GetFocus();
  auto restoreFocus = [&]() {
    if (prevFocus && IsWindow(prevFocus) && IsWindowEnabled(prevFocus) && IsWindowVisible(prevFocus)) {
      SetFocus(prevFocus);
    }
  };

  wchar_t fileBuf[MAX_PATH] = {0};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFile = fileBuf;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = L"WAV files (*.wav)\0*.wav\0All files\0*.*\0";
  ofn.lpstrDefExt = L"wav";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
  if (!GetSaveFileNameW(&ofn)) {
    restoreFocus();
    return false;
  }
  outPath = fileBuf;
  restoreFocus();
  return true;
}

void EnsureListViewHasSelection(HWND lv) {
  if (!lv) return;
  int count = ListView_GetItemCount(lv);
  if (count <= 0) return;

  int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
  if (sel < 0) sel = 0;

  // Ensure something is both selected and focused so users don't tab into a
  // list that appears empty to assistive tech.
  ListView_SetItemState(lv, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  ListView_EnsureVisible(lv, sel, FALSE);
}
