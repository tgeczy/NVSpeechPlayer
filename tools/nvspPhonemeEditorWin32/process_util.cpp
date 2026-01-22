#include "process_util.h"

#include <filesystem>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace nvsp_editor {

static std::wstring quoteArg(const std::wstring& s) {
  // Simple quoting for CreateProcess command lines.
  // This is not a full Windows command-line escaping implementation, but is
  // sufficient for paths and simple arguments for this tool.
  if (s.empty()) return L"\"\"";
  bool needs = false;
  for (wchar_t c : s) {
    if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\v' || c == L'\"') {
      needs = true;
      break;
    }
  }
  if (!needs) return s;

  std::wstring out = L"\"";
  for (wchar_t c : s) {
    if (c == L'\"') out += L"\\\"";
    else out.push_back(c);
  }
  out += L"\"";
  return out;
}

bool runProcessCaptureStdout(
  const std::wstring& exePath,
  const std::wstring& args,
  std::string& outStdoutUtf8,
  std::string& outError
) {
  outStdoutUtf8.clear();
  outError.clear();

  if (exePath.empty()) {
    outError = "Executable path is empty";
    return false;
  }

  HANDLE hRead = NULL;
  HANDLE hWrite = NULL;

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
    outError = "CreatePipe failed";
    return false;
  }

  // Ensure the read handle is not inherited.
  SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

  // GUI apps often don't have valid STDIN. Give the child a readable handle.
  HANDLE hNullIn = CreateFileW(
    L"NUL",
    GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    &sa,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    NULL
  );
  if (hNullIn == INVALID_HANDLE_VALUE) {
    hNullIn = GetStdHandle(STD_INPUT_HANDLE);
  }

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = (hNullIn && hNullIn != INVALID_HANDLE_VALUE) ? hNullIn : NULL;
  si.hStdOutput = hWrite;
  si.hStdError = hWrite;

  PROCESS_INFORMATION pi{};

  std::wstring cmd = quoteArg(exePath);
  if (!args.empty()) {
    cmd += L" ";
    cmd += args;
  }

  // CreateProcess wants a writable buffer.
  std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
  cmdBuf.push_back(L'\0');

  // Some eSpeak builds are sensitive to current directory when locating data.
  // Use the executable directory as the working directory.
  std::wstring cwd;
  try {
    cwd = fs::path(exePath).parent_path().wstring();
  } catch (...) {
    cwd.clear();
  }

  BOOL ok = CreateProcessW(
    exePath.c_str(),
    cmdBuf.data(),
    NULL,
    NULL,
    TRUE,
    CREATE_NO_WINDOW,
    NULL,
    cwd.empty() ? NULL : cwd.c_str(),
    &si,
    &pi
  );

  // Parent doesn't write.
  CloseHandle(hWrite);
  hWrite = NULL;

  if (hNullIn && hNullIn != INVALID_HANDLE_VALUE && hNullIn != GetStdHandle(STD_INPUT_HANDLE)) {
    CloseHandle(hNullIn);
    hNullIn = NULL;
  }

  if (!ok) {
    DWORD e = GetLastError();
    CloseHandle(hRead);
    std::ostringstream oss;
    oss << "CreateProcess failed (" << static_cast<unsigned long>(e) << ")";
    outError = oss.str();
    return false;
  }

  // Read all output.
  std::string buf;
  char tmp[4096];
  DWORD read = 0;
  while (true) {
    BOOL r = ReadFile(hRead, tmp, sizeof(tmp), &read, NULL);
    if (!r || read == 0) break;
    buf.append(tmp, tmp + read);
  }

  CloseHandle(hRead);
  hRead = NULL;

  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exitCode = 0;
  GetExitCodeProcess(pi.hProcess, &exitCode);

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  outStdoutUtf8 = std::move(buf);

  // Trim trailing CR/LF.
  while (!outStdoutUtf8.empty() && (outStdoutUtf8.back() == '\n' || outStdoutUtf8.back() == '\r')) {
    outStdoutUtf8.pop_back();
  }

  if (exitCode != 0) {
    std::ostringstream oss;
    oss << "Process exit code " << static_cast<unsigned long>(exitCode)
        << " (0x" << std::hex << static_cast<unsigned long>(exitCode) << std::dec << ")";

    if (!outStdoutUtf8.empty()) {
      // Include a short snippet of output to help debugging.
      std::string snippet = outStdoutUtf8;
      if (snippet.size() > 600) {
        snippet.resize(600);
        snippet += "...";
      }
      oss << "\n\nOutput:\n" << snippet;
    }

    outError = oss.str();
    return false;
  }

  return true;
}

bool runProcessCaptureStdoutWithStdin(
  const std::wstring& exePath,
  const std::wstring& args,
  const std::string& stdinUtf8,
  std::string& outStdoutUtf8,
  std::string& outError
) {
  outStdoutUtf8.clear();
  outError.clear();

  if (exePath.empty()) {
    outError = "Executable path is empty";
    return false;
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE hOutRead = NULL;
  HANDLE hOutWrite = NULL;
  if (!CreatePipe(&hOutRead, &hOutWrite, &sa, 0)) {
    outError = "CreatePipe(stdout) failed";
    return false;
  }
  // Ensure the read handle is not inherited.
  SetHandleInformation(hOutRead, HANDLE_FLAG_INHERIT, 0);

  HANDLE hInRead = NULL;
  HANDLE hInWrite = NULL;
  if (!CreatePipe(&hInRead, &hInWrite, &sa, 0)) {
    CloseHandle(hOutRead);
    CloseHandle(hOutWrite);
    outError = "CreatePipe(stdin) failed";
    return false;
  }
  // Ensure the write handle is not inherited (parent-only).
  SetHandleInformation(hInWrite, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = hInRead;
  si.hStdOutput = hOutWrite;
  si.hStdError = hOutWrite;

  PROCESS_INFORMATION pi{};

  std::wstring cmd = quoteArg(exePath);
  if (!args.empty()) {
    cmd += L" ";
    cmd += args;
  }

  // CreateProcess wants a writable buffer.
  std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
  cmdBuf.push_back(L'\0');

  // Use the executable directory as the working directory.
  std::wstring cwd;
  try {
    cwd = fs::path(exePath).parent_path().wstring();
  } catch (...) {
    cwd.clear();
  }

  BOOL ok = CreateProcessW(
    exePath.c_str(),
    cmdBuf.data(),
    NULL,
    NULL,
    TRUE,
    CREATE_NO_WINDOW,
    NULL,
    cwd.empty() ? NULL : cwd.c_str(),
    &si,
    &pi
  );

  // Parent doesn't use these ends.
  CloseHandle(hOutWrite);
  hOutWrite = NULL;
  CloseHandle(hInRead);
  hInRead = NULL;

  if (!ok) {
    DWORD e = GetLastError();
    CloseHandle(hOutRead);
    CloseHandle(hInWrite);
    std::ostringstream oss;
    oss << "CreateProcess failed (" << static_cast<unsigned long>(e) << ")";
    outError = oss.str();
    return false;
  }

  // Write stdin (then close to signal EOF).
  bool writeOk = true;
  if (!stdinUtf8.empty()) {
    const char* p = stdinUtf8.data();
    size_t remaining = stdinUtf8.size();
    while (remaining > 0) {
      DWORD wrote = 0;
      DWORD toWrite = (remaining > 65535u) ? 65535u : static_cast<DWORD>(remaining);
      BOOL w = WriteFile(hInWrite, p, toWrite, &wrote, NULL);
      if (!w) {
        writeOk = false;
        break;
      }
      p += wrote;
      remaining -= wrote;
      if (wrote == 0) break;
    }
  }
  CloseHandle(hInWrite);
  hInWrite = NULL;

  // Read all stdout/stderr.
  std::string buf;
  char tmp[4096];
  DWORD read = 0;
  while (true) {
    BOOL r = ReadFile(hOutRead, tmp, sizeof(tmp), &read, NULL);
    if (!r || read == 0) break;
    buf.append(tmp, tmp + read);
  }

  CloseHandle(hOutRead);
  hOutRead = NULL;

  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exitCode = 0;
  GetExitCodeProcess(pi.hProcess, &exitCode);

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  outStdoutUtf8 = std::move(buf);

  // Trim trailing CR/LF.
  while (!outStdoutUtf8.empty() && (outStdoutUtf8.back() == '\n' || outStdoutUtf8.back() == '\r')) {
    outStdoutUtf8.pop_back();
  }

  if (!writeOk) {
    // Treat write failures as an error (the child may have exited early).
    outError = "Failed to write stdin to child process";
    return false;
  }

  if (exitCode != 0) {
    std::ostringstream oss;
    oss << "Process exit code " << static_cast<unsigned long>(exitCode)
        << " (0x" << std::hex << static_cast<unsigned long>(exitCode) << std::dec << ")";

    if (!outStdoutUtf8.empty()) {
      std::string snippet = outStdoutUtf8;
      if (snippet.size() > 600) {
        snippet.resize(600);
        snippet += "...";
      }
      oss << "\n\nOutput:\n" << snippet;
    }

    outError = oss.str();
    return false;
  }

  return true;
}

std::wstring findEspeakExe(const std::wstring& espeakDir) {
  if (espeakDir.empty()) return L"";

  fs::path base(espeakDir);

  fs::path ng = base / "espeak-ng.exe";
  if (fs::exists(ng)) return ng.wstring();

  fs::path legacy = base / "espeak.exe";
  if (fs::exists(legacy)) return legacy.wstring();

  return L"";
}

// Returns the "data home" directory to pass to espeak_Initialize/--path.
// According to speak_lib.h, this should be the directory that *contains*
// the espeak-ng-data (or espeak-data) directory.
//
// Examples:
//   C:\eSpeak NG\            -> contains espeak-ng-data  => return C:\eSpeak NG\ (home)
//   C:\eSpeak NG\bin\       -> parent contains data     => return C:\eSpeak NG\ (home)
//   C:\eSpeak NG\espeak-ng-data\ -> base is data dir    => return C:\eSpeak NG\ (home)
std::wstring findEspeakDataDir(const std::wstring& espeakDir) {
  if (espeakDir.empty()) return L"";

  fs::path base(espeakDir);
  std::error_code ec;

  auto hasDataDir = [&](const fs::path& home) -> bool {
    if (home.empty()) return false;
    const fs::path ng = home / "espeak-ng-data";
    const fs::path legacy = home / "espeak-data";
    if (fs::exists(ng, ec) && fs::is_directory(ng, ec)) return true;
    if (fs::exists(legacy, ec) && fs::is_directory(legacy, ec)) return true;
    return false;
  };

  // If the user picked the actual data directory, return its parent.
  {
    const std::wstring leaf = base.filename().wstring();
    if (_wcsicmp(leaf.c_str(), L"espeak-ng-data") == 0 || _wcsicmp(leaf.c_str(), L"espeak-data") == 0) {
      return base.parent_path().wstring();
    }
  }

  // Common: <base> contains espeak-ng-data/espeak-data.
  if (hasDataDir(base)) return base.wstring();

  // Some layouts: <base>/share contains the data directory.
  {
    const fs::path share = base / "share";
    if (hasDataDir(share)) return share.wstring();
  }

  // If the user picked a bin folder, the parent might contain the data.
  {
    const fs::path parent = base.parent_path();
    if (hasDataDir(parent)) return parent.wstring();
  }

  return L"";
}
} // namespace nvsp_editor
