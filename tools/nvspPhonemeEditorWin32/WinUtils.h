#pragma once

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commctrl.h>

#include <string>

// -------------------------
// Small Win32 helpers
// -------------------------

std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& w);

std::wstring exeDir();

// INI storage in nvspPhonemeEditor.ini next to the EXE.
std::wstring iniPath();
std::wstring readIni(const wchar_t* section, const wchar_t* key, const wchar_t* def = L"");
void writeIni(const wchar_t* section, const wchar_t* key, const std::wstring& value);

int readIniInt(const wchar_t* section, const wchar_t* key, int defVal);
void writeIniInt(const wchar_t* section, const wchar_t* key, int value);

// Keyboard-focus-friendly message box.
void msgBox(
  HWND owner,
  const std::wstring& text,
  const std::wstring& title = L"NV Speech Player Phoneme Editor",
  UINT flags = MB_OK
);

// Folder picker (IFileDialog).
bool pickFolder(HWND owner, const wchar_t* title, std::wstring& outFolder);

// File picker for an executable (GetOpenFileName).
bool pickOpenExe(HWND owner, std::wstring& outPath);

// Save WAV path picker.
bool pickSaveWav(HWND owner, std::wstring& outPath);

// When a list view receives focus, ensure there's a focused+selected item.
void EnsureListViewHasSelection(HWND lv);
