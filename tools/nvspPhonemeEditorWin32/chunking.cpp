#include "chunking.h"

#include <algorithm>

namespace nvsp_editor {

static bool isSentenceEnd(wchar_t c) {
  switch (c) {
    case L'.':
    case L'!':
    case L'?':
    case L';':
    case L':':
    case 0x3002: // 。
    case 0xFF01: // ！
    case 0xFF1F: // ？
      return true;
    default:
      return false;
  }
}

static bool isSpaceLike(wchar_t c) {
  return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\v' || c == L'\f';
}

static std::wstring trimAndCollapseSpaces(const std::wstring& in) {
  std::wstring out;
  out.reserve(in.size());

  bool inSpace = true; // trim leading
  for (wchar_t c : in) {
    if (isSpaceLike(c)) {
      if (!inSpace) {
        out.push_back(L' ');
        inSpace = true;
      }
      continue;
    }

    out.push_back(c);
    inSpace = false;
  }

  while (!out.empty() && out.back() == L' ') out.pop_back();
  return out;
}

static void splitSentences(const std::wstring& text, std::vector<std::wstring>& outSentences) {
  outSentences.clear();

  std::wstring current;
  current.reserve(text.size());

  for (size_t i = 0; i < text.size(); ++i) {
    wchar_t c = text[i];
    current.push_back(c);

    // Treat new lines as hard-ish boundaries.
    if (c == L'\n') {
      std::wstring s = trimAndCollapseSpaces(current);
      if (!s.empty()) outSentences.push_back(std::move(s));
      current.clear();
      continue;
    }

    if (isSentenceEnd(c)) {
      // Include trailing quotes/brackets after punctuation.
      while (i + 1 < text.size()) {
        wchar_t n = text[i + 1];
        if (n == L'\"' || n == L'\'' || n == L')' || n == L']' || n == L'}') {
          current.push_back(n);
          ++i;
          continue;
        }
        break;
      }

      std::wstring s = trimAndCollapseSpaces(current);
      if (!s.empty()) outSentences.push_back(std::move(s));
      current.clear();
    }
  }

  std::wstring s = trimAndCollapseSpaces(current);
  if (!s.empty()) outSentences.push_back(std::move(s));
}

static void splitLongSentenceOnSpaces(
  const std::wstring& sentence,
  size_t maxChars,
  std::vector<std::wstring>& out
) {
  std::wstring s = trimAndCollapseSpaces(sentence);
  if (s.size() <= maxChars || maxChars == 0) {
    if (!s.empty()) out.push_back(std::move(s));
    return;
  }

  size_t start = 0;
  while (start < s.size()) {
    size_t remaining = s.size() - start;
    if (remaining <= maxChars) {
      std::wstring part = trimAndCollapseSpaces(s.substr(start));
      if (!part.empty()) out.push_back(std::move(part));
      break;
    }

    // Try to cut at a space before maxChars.
    size_t cut = start + maxChars;
    size_t best = s.rfind(L' ', cut);
    if (best == std::wstring::npos || best <= start) {
      // No spaces; hard cut.
      best = cut;
    }

    std::wstring part = trimAndCollapseSpaces(s.substr(start, best - start));
    if (!part.empty()) out.push_back(std::move(part));

    start = best;
    while (start < s.size() && s[start] == L' ') ++start;
  }
}

std::vector<TextChunk> chunkTextForPhonemizer(const std::wstring& text, size_t maxChars) {
  // Keep bounds sane.
  if (maxChars < 80) maxChars = 80;
  if (maxChars > 4000) maxChars = 4000;

  std::vector<std::wstring> sentences;
  splitSentences(text, sentences);

  // First, ensure no single sentence is gigantic.
  // We also keep per-sentence boundaries so we can re-inject clause breaks into the
  // IPA stream later (otherwise speech sounds like one long run-on).
  std::vector<TextChunk> parts;
  parts.reserve(sentences.size());
  for (const auto& s : sentences) {
    std::vector<std::wstring> splitParts;
    splitParts.reserve(4);
    splitLongSentenceOnSpaces(s, maxChars, splitParts);
    for (size_t i = 0; i < splitParts.size(); ++i) {
      TextChunk tc;
      tc.text = std::move(splitParts[i]);
      tc.endsSentence = (i + 1 == splitParts.size());
      if (!tc.text.empty()) parts.push_back(std::move(tc));
    }
  }

  // Now pack parts into chunks up to maxChars, BUT never cross a sentence boundary.
  // (If we merged multiple sentences into a single phonemizer call, we couldn't
  // reliably re-inject clause breaks into the IPA output.)
  std::vector<TextChunk> chunks;
  std::wstring current;
  bool currentEndsSentence = false;
  current.reserve(maxChars + 16);

  auto flush = [&]() {
    std::wstring c = trimAndCollapseSpaces(current);
    if (!c.empty()) {
      TextChunk out;
      out.text = std::move(c);
      out.endsSentence = currentEndsSentence;
      chunks.push_back(std::move(out));
    }
    current.clear();
    currentEndsSentence = false;
  };

  for (const auto& part : parts) {
    if (part.text.empty()) continue;

    if (current.empty()) {
      current = part.text;
      currentEndsSentence = part.endsSentence;
    } else if (current.size() + 1 + part.text.size() <= maxChars) {
      current.push_back(L' ');
      current += part.text;
      currentEndsSentence = part.endsSentence;
    } else {
      // Forced split mid-sentence due to length.
      flush();
      current = part.text;
      currentEndsSentence = part.endsSentence;
    }

    // Never carry over into the next sentence.
    if (part.endsSentence) {
      flush();
    }
  }

  flush();

  return chunks;
}

} // namespace nvsp_editor
