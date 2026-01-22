#pragma once

#include <string>
#include <vector>

namespace nvsp_editor {

struct TextChunk {
  std::wstring text;
  // True if this chunk ends a sentence (punctuation or a hard boundary like a newline).
  // This is used to re-inject clause breaks into the IPA stream so speech does not sound
  // like one long run-on utterance after chunking.
  bool endsSentence = false;
};

// Sentence-aware chunking for "speak window" style text.
//
// Why we do this:
// - Many CLI phonemizers are awkward with very long command-lines.
// - Even when using stdin, chunking keeps the UI responsive and makes
//   failures easier to localize.
//
// maxChars is a soft limit; chunks may be slightly smaller, but should not
// exceed maxChars unless a single "token" (e.g., a very long word) exceeds it.
std::vector<TextChunk> chunkTextForPhonemizer(
  const std::wstring& text,
  size_t maxChars
);

} // namespace nvsp_editor
