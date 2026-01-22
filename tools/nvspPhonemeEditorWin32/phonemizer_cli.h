#pragma once

#include <string>

namespace nvsp_editor {

// A "CLI phonemizer" is any executable that can convert text -> IPA/phonemes
// and writes that conversion to stdout.
//
// This tool supports two ways of feeding text:
// - stdin: preferred (handles long text safely, avoids quoting issues)
// - args:  fallback (some tools don't read stdin)
struct CliPhonemizerConfig {
  std::wstring exePath;

  // Command-line templates. Supported placeholders:
  //   {lang}      selected BCP-47-ish language tag (e.g. "en", "pt-br")
  //   {qlang}     same, but quoted for CreateProcess command lines
  //   {text}      chunk text (UNQUOTED)
  //   {qtext}     chunk text quoted for CreateProcess command lines
  //   {espeakDir} configured eSpeak directory (UNQUOTED)
  //   {qespeakDir} configured eSpeak directory (QUOTED)
  //   {dataDir}   resolved eSpeak data "home" dir (UNQUOTED)
  //   {qdataDir}  resolved eSpeak data "home" dir (QUOTED)
  //   {pathArg}   expands to "--path=\"...\" " or "" when dataDir is missing
  std::wstring argsStdinTemplate;
  std::wstring argsCliTemplate;

  bool preferStdin = true;
  size_t maxChunkChars = 420;

  // Optional context for placeholder expansion.
  std::wstring espeakDir;
  std::wstring espeakDataDir;
};

// Convert Unicode text to IPA/phonemes (UTF-8) using a configured CLI phonemizer.
// This function:
// - chunks text to keep invocations sane
// - prefers stdin, but can fall back to args
// - concatenates per-chunk results into one IPA string
bool phonemizeTextToIpa(
  const CliPhonemizerConfig& cfg,
  const std::string& langTagUtf8,
  const std::wstring& text,
  std::string& outIpaUtf8,
  std::string& outError
);

} // namespace nvsp_editor
