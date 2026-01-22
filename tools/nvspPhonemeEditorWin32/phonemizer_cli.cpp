#include "phonemizer_cli.h"

#include "chunking.h"
#include "process_util.h"
#include "WinUtils.h" // wideToUtf8 / utf8ToWide

#include <cwchar>
#include <sstream>
#include <vector>

namespace nvsp_editor {

static bool endsWithCaseInsensitive(const std::wstring& s, const std::wstring& suffix) {
  if (suffix.size() > s.size()) return false;
  const size_t off = s.size() - suffix.size();
  return _wcsicmp(s.c_str() + off, suffix.c_str()) == 0;
}

static bool isEspeakExePath(const std::wstring& exePath) {
  // eSpeak/eSpeak-NG stdin parsing is a bit quirky: it reads ahead and may not
  // fully flush the final token at end-of-input unless it sees at least one
  // more non-space character.
  //
  // We detect eSpeak here so we can append a harmless " _" terminator when
  // writing stdin. (Underscore is treated as a separator/ignored in eSpeak IPA
  // output and is also stripped by our frontend's IPA normalization.)
  return endsWithCaseInsensitive(exePath, L"espeak-ng.exe") ||
         endsWithCaseInsensitive(exePath, L"espeak.exe");
}

static std::string clauseMarkerTokenForText(const std::wstring& textW) {
  // Determine punctuation at the end of a text chunk.
  // Mirrors the NVDA driver behavior (see NV Speech Player synth driver):
  // - "..." is treated as '.' for prosody, but still gets a strong pause.
  // - If no punctuation is present (e.g. newline boundary), default to '.'.
  if (textW.empty()) return ".";

  // Trim trailing whitespace.
  size_t end = textW.size();
  while (end > 0) {
    wchar_t c = textW[end - 1];
    if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\v' || c == L'\f') {
      --end;
      continue;
    }
    break;
  }
if (end == 0) return ".";

// Strip common trailing closing quotes/brackets after punctuation.
// Example: Hello."  -> treat it as ending with '.'
while (end > 0) {
  wchar_t c = textW[end - 1];
  if (c == L'\"' || c == L'\'' ||
      c == 0x201D || c == 0x2019 || // ” ’
      c == L')' || c == L']' || c == L'}' ||
      c == 0x00BB || c == 0x203A) { // » ›
    --end;
    continue;
  }
  break;
}
if (end == 0) return ".";

// Ellipsis: "..." or single-character '…'

  if (end >= 3 && textW[end - 3] == L'.' && textW[end - 2] == L'.' && textW[end - 1] == L'.') {
    return "...";
  }
  if (textW[end - 1] == 0x2026) { // …
    return "...";
  }

  wchar_t last = textW[end - 1];
  // Normalize some common fullwidth punctuation.
  switch (last) {
    case 0x3002: last = L'.'; break; // 。
    case 0xFF01: last = L'!'; break; // ！
    case 0xFF1F: last = L'?'; break; // ？
    case 0xFF1A: last = L':'; break; // ：
    case 0xFF1B: last = L';'; break; // ；
    case 0xFF0C: last = L','; break; // ，
    default: break;
  }

  switch (last) {
    case L'.': return ".";
    case L'!': return "!";
    case L'?': return "?";
    case L',': return ",";
    case L':': return ":";
    case L';': return ";";
    default: break;
  }

  return ".";
}

static std::wstring quoteArg(const std::wstring& s) {
  // Simple quoting for CreateProcess command lines.
  // This matches the approach in process_util.cpp.
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

static void replaceAll(std::wstring& s, const std::wstring& from, const std::wstring& to) {
  if (from.empty()) return;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::wstring::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

static std::wstring buildArgsFromTemplate(
  const CliPhonemizerConfig& cfg,
  const std::wstring& templ,
  const std::wstring& langW,
  const std::wstring& textW
) {
  std::wstring out = templ;

  const std::wstring qLang = quoteArg(langW);
  const std::wstring qText = quoteArg(textW);

  const std::wstring& esDir = cfg.espeakDir;
  const std::wstring& dataDir = cfg.espeakDataDir;

  const std::wstring qEsDir = quoteArg(esDir);
  const std::wstring qDataDir = quoteArg(dataDir);

  // Helpful for eSpeak templates.
  std::wstring pathArg;
  if (!dataDir.empty()) {
    pathArg = L"--path=" + qDataDir + L" ";
  }

  replaceAll(out, L"{lang}", langW);
  replaceAll(out, L"{qlang}", qLang);
  replaceAll(out, L"{text}", textW);
  replaceAll(out, L"{qtext}", qText);
  replaceAll(out, L"{espeakDir}", esDir);
  replaceAll(out, L"{qespeakDir}", qEsDir);
  replaceAll(out, L"{dataDir}", dataDir);
  replaceAll(out, L"{qdataDir}", qDataDir);
  replaceAll(out, L"{pathArg}", pathArg);

  // Cleanup: collapse repeated spaces (keep it simple).
  // (This avoids templates producing double spaces when {pathArg} is empty.)
  while (out.find(L"  ") != std::wstring::npos) {
    replaceAll(out, L"  ", L" ");
  }

  // Trim.
  while (!out.empty() && (out.front() == L' ' || out.front() == L'\t')) out.erase(out.begin());
  while (!out.empty() && (out.back() == L' ' || out.back() == L'\t')) out.pop_back();

  return out;
}

static std::string trimAscii(const std::string& s) {
  size_t a = 0;
  while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  size_t b = s.size();
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return s.substr(a, b - a);
}

bool phonemizeTextToIpa(
  const CliPhonemizerConfig& cfg,
  const std::string& langTagUtf8,
  const std::wstring& text,
  std::string& outIpaUtf8,
  std::string& outError
) {
  outIpaUtf8.clear();
  outError.clear();

  if (cfg.exePath.empty()) {
    outError = "Phonemizer executable path is empty";
    return false;
  }

  // Chunking keeps CLI calls sane.
  std::vector<TextChunk> chunks = chunkTextForPhonemizer(text, cfg.maxChunkChars);
  if (chunks.empty()) {
    outError = "Input is empty";
    return false;
  }

  const std::wstring langW = utf8ToWide(langTagUtf8);

  std::string joined;
  bool first = true;

  for (size_t i = 0; i < chunks.size(); ++i) {
    const std::wstring& chunkW = chunks[i].text;
    const bool endsSentence = chunks[i].endsSentence;

    std::string chunkErrStdin;
    std::string chunkErrCli;
    std::string chunkOut;

    auto tryStdin = [&]() -> bool {
      if (!cfg.preferStdin) return false;
      if (cfg.argsStdinTemplate.empty()) return false;

      const std::wstring args = buildArgsFromTemplate(cfg, cfg.argsStdinTemplate, langW, chunkW);
      std::string stdinBytes = wideToUtf8(chunkW);
      // NOTE:
      // - Many CLI phonemizers are line-based and expect a trailing newline.
      // - eSpeak/eSpeak-NG in particular reads ahead and may not flush the last
      //   character properly unless it sees at least one more non-space char.
      //   Appending " _" is a common workaround; underscore is ignored/treated
      //   as a separator and does not affect speech content.
      if (isEspeakExePath(cfg.exePath)) {
        stdinBytes += " _\n";
      } else {
        if (stdinBytes.empty() || stdinBytes.back() != '\n') stdinBytes.push_back('\n');
      }

      std::string out;
      std::string err;
      if (!runProcessCaptureStdoutWithStdin(cfg.exePath, args, stdinBytes, out, err)) {
        chunkErrStdin = err;
        return false;
      }

      out = trimAscii(out);
      if (out.empty()) {
        chunkErrStdin = "Phonemizer produced empty output";
        return false;
      }

      chunkOut = std::move(out);
      return true;
    };

    auto tryCli = [&]() -> bool {
      if (cfg.argsCliTemplate.empty()) return false;
      const std::wstring args = buildArgsFromTemplate(cfg, cfg.argsCliTemplate, langW, chunkW);

      std::string out;
      std::string err;
      if (!runProcessCaptureStdout(cfg.exePath, args, out, err)) {
        chunkErrCli = err;
        return false;
      }

      out = trimAscii(out);
      if (out.empty()) {
        chunkErrCli = "Phonemizer produced empty output";
        return false;
      }

      chunkOut = std::move(out);
      return true;
    };

    bool ok = false;
    if (tryStdin()) {
      ok = true;
    } else if (tryCli()) {
      ok = true;
    }

    if (!ok) {
      std::ostringstream oss;
      oss << "Phonemizer failed on chunk " << (i + 1) << " of " << chunks.size() << ".";

      if (!chunkErrStdin.empty()) {
        oss << "\n\nSTDIN attempt:\n" << chunkErrStdin;
      }
      if (!chunkErrCli.empty()) {
        oss << "\n\nCLI attempt:\n" << chunkErrCli;
      }

      // Include a short preview (UTF-8) to help debug, but don't spam.
      std::string preview = wideToUtf8(chunkW);
      if (preview.size() > 200) {
        preview.resize(200);
        preview += "...";
      }
      oss << "\n\nChunk preview:\n" << preview;

      outError = oss.str();
      return false;
    }

    if (!first) joined.push_back(' ');
    joined += chunkOut;
    first = false;

// Re-inject clause markers between sentence chunks so we can later insert
// real (time-domain) pauses during synthesis.
//
// NOTE: Most phonemizers do not emit punctuation in their IPA output.
// We therefore carry punctuation *from the original text* as standalone
// tokens like ".", "?", "!", ":" and ";". These are removed before IPA
// is fed to nvspFrontend, but are used to:
// - set clauseType (intonation hints)
// - insert optional micro-pauses between clauses
if (endsSentence && (i + 1) < chunks.size()) {
  const std::string tok = clauseMarkerTokenForText(chunkW);

  // Avoid doubling if we already have the same marker as the last token.
  bool already = false;
  if (!joined.empty() && !tok.empty()) {
    size_t pos = joined.find_last_of(" \t\r\n");
    std::string last = (pos == std::string::npos) ? joined : joined.substr(pos + 1);
    already = (last == tok);
  }
  if (!already) {
    joined.push_back(' ');
    joined += tok;
  }
}
  }

  outIpaUtf8 = std::move(joined);
  return true;
}

} // namespace nvsp_editor
