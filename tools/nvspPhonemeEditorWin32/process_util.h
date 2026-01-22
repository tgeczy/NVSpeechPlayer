#pragma once

#include <string>
#include <vector>

#include <windows.h>

namespace nvsp_editor {

// Run a process and capture its stdout as UTF-8 bytes.
// exePath: full path to the exe.
// args: command line arguments (without the exe name).
// Returns true on success.
bool runProcessCaptureStdout(
  const std::wstring& exePath,
  const std::wstring& args,
  std::string& outStdoutUtf8,
  std::string& outError
);

// Run a process, write UTF-8 bytes to its stdin, and capture its stdout as UTF-8.
// exePath: full path to the exe.
// args: command line arguments (without the exe name).
// stdinUtf8: bytes written to the child process stdin (then stdin is closed).
// Returns true on success.
bool runProcessCaptureStdoutWithStdin(
  const std::wstring& exePath,
  const std::wstring& args,
  const std::string& stdinUtf8,
  std::string& outStdoutUtf8,
  std::string& outError
);

// Find espeak-ng.exe or espeak.exe inside a directory.
std::wstring findEspeakExe(const std::wstring& espeakDir);

// Find an eSpeak data directory within the configured eSpeak directory.
// Returns a full path to either espeak-ng-data or espeak-data if present.
std::wstring findEspeakDataDir(const std::wstring& espeakDir);

} // namespace nvsp_editor
