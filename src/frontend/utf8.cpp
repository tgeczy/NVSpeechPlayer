/*
TGSpeechBox — UTF-8 encoding and decoding utilities.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include "utf8.h"

#include <cstdint>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace nvsp_frontend {

static inline char32_t kReplacementChar = 0xFFFD;

std::u32string utf8ToU32(std::string_view s) {
  std::u32string out;
  out.reserve(s.size());

  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
  const unsigned char* end = p + s.size();

  while (p < end) {
    uint32_t cp = 0;
    unsigned char c0 = *p++;

    if (c0 < 0x80) {
      cp = c0;
    } else if ((c0 >> 5) == 0x6) {
      // 110xxxxx 10xxxxxx
      if (p >= end) {
        out.push_back(kReplacementChar);
        break;
      }
      unsigned char c1 = *p++;
      if ((c1 & 0xC0) != 0x80) {
        out.push_back(kReplacementChar);
        continue;
      }
      cp = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
      if (cp < 0x80) {
        out.push_back(kReplacementChar);
        continue;
      }
    } else if ((c0 >> 4) == 0xE) {
      // 1110xxxx 10xxxxxx 10xxxxxx
      if (p + 1 >= end) {
        out.push_back(kReplacementChar);
        break;
      }
      unsigned char c1 = *p++;
      unsigned char c2 = *p++;
      if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) {
        out.push_back(kReplacementChar);
        continue;
      }
      cp = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
      if (cp < 0x800) {
        out.push_back(kReplacementChar);
        continue;
      }
      // UTF-16 surrogate halves are not valid Unicode scalar values.
      if (cp >= 0xD800 && cp <= 0xDFFF) {
        out.push_back(kReplacementChar);
        continue;
      }
    } else if ((c0 >> 3) == 0x1E) {
      // 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
      if (p + 2 >= end) {
        out.push_back(kReplacementChar);
        break;
      }
      unsigned char c1 = *p++;
      unsigned char c2 = *p++;
      unsigned char c3 = *p++;
      if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) {
        out.push_back(kReplacementChar);
        continue;
      }
      cp = ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
      if (cp < 0x10000 || cp > 0x10FFFF) {
        out.push_back(kReplacementChar);
        continue;
      }
    } else {
      out.push_back(kReplacementChar);
      continue;
    }

    out.push_back(static_cast<char32_t>(cp));
  }

  return out;
}

std::string u32ToUtf8(std::u32string_view s) {
  std::string out;
  out.reserve(s.size());

  for (char32_t ch : s) {
    uint32_t cp = static_cast<uint32_t>(ch);
    if (cp <= 0x7F) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  return out;
}

std::string normalizeLangTag(std::string_view tag) {
  std::string out;
  out.reserve(tag.size());
  for (char c : tag) {
    if (c >= 'A' && c <= 'Z') {
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else if (c == '_') {
      out.push_back('-');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// ── Invisible character stripping ────────────────────────────────────────────

std::string stripInvisible(std::string_view s) {
  auto u32 = utf8ToU32(s);
  std::u32string out;
  out.reserve(u32.size());
  for (char32_t c : u32) {
    switch (c) {
      case 0x200B:  // ZERO WIDTH SPACE
      case 0x200C:  // ZERO WIDTH NON-JOINER
      case 0x200D:  // ZERO WIDTH JOINER
      case 0x2060:  // WORD JOINER
      case 0xFEFF:  // BOM / ZERO WIDTH NO-BREAK SPACE
      case 0x00AD:  // SOFT HYPHEN
      case 0x034F:  // COMBINING GRAPHEME JOINER
      case 0x061C:  // ARABIC LETTER MARK
      case 0x180E:  // MONGOLIAN VOWEL SEPARATOR
        continue;
      // Normalize Unicode space variants to ASCII space.
      // iOS clock uses U+202F between time and AM/PM; web content uses
      // various typographic spaces that break word tokenization.
      case 0x00A0:  // NO-BREAK SPACE
      case 0x2002:  // EN SPACE
      case 0x2003:  // EM SPACE
      case 0x2004:  // THREE-PER-EM SPACE
      case 0x2005:  // FOUR-PER-EM SPACE
      case 0x2006:  // SIX-PER-EM SPACE
      case 0x2007:  // FIGURE SPACE
      case 0x2008:  // PUNCTUATION SPACE
      case 0x2009:  // THIN SPACE
      case 0x200A:  // HAIR SPACE
      case 0x202F:  // NARROW NO-BREAK SPACE
      case 0x205F:  // MEDIUM MATHEMATICAL SPACE
      case 0x3000:  // IDEOGRAPHIC SPACE
        out.push_back(' ');
        continue;
      default:
        if (c >= 0x200E && c <= 0x200F) continue;  // LRM, RLM
        if (c >= 0x202A && c <= 0x202E) continue;  // bidi embeddings
        if (c >= 0x2066 && c <= 0x2069) continue;  // bidi isolates
        out.push_back(c);
    }
  }
  if (out.size() == u32.size()) return std::string(s);
  return u32ToUtf8(out);
}

// ── NFKC normalization ──────────────────────────────────────────────────────

#ifdef _WIN32

std::string normalizeNFKC(const std::string& utf8) {
  if (utf8.empty()) return utf8;

  // Resolve NormalizeString dynamically to avoid link-time dependency
  // on normaliz.lib.  Available since Vista: in normaliz.dll on Vista/7,
  // also exported from kernel32.dll on Win8+.
  using Fn = int(WINAPI*)(int, LPCWSTR, int, LPWSTR, int);
  static Fn pfn = [] {
    Fn f = nullptr;
    HMODULE mod = GetModuleHandleW(L"kernel32.dll");
    if (mod) f = reinterpret_cast<Fn>(GetProcAddress(mod, "NormalizeString"));
    if (!f) {
      mod = LoadLibraryW(L"normaliz.dll");
      if (mod) f = reinterpret_cast<Fn>(GetProcAddress(mod, "NormalizeString"));
    }
    return f;
  }();
  if (!pfn) return utf8;

  // UTF-8 -> UTF-16.
  int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                 static_cast<int>(utf8.size()), nullptr, 0);
  if (wlen <= 0) return utf8;
  std::wstring wide(static_cast<size_t>(wlen), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                      static_cast<int>(utf8.size()), &wide[0], wlen);

  // NormalizationKC = 5.
  int normLen = pfn(5, wide.c_str(), wlen, nullptr, 0);
  if (normLen <= 0) return utf8;
  std::wstring norm(static_cast<size_t>(normLen), L'\0');
  normLen = pfn(5, wide.c_str(), wlen, &norm[0], normLen);
  if (normLen <= 0) return utf8;
  norm.resize(static_cast<size_t>(normLen));

  // UTF-16 -> UTF-8.
  int u8len = WideCharToMultiByte(CP_UTF8, 0, norm.c_str(), normLen,
                                  nullptr, 0, nullptr, nullptr);
  if (u8len <= 0) return utf8;
  std::string result(static_cast<size_t>(u8len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, norm.c_str(), normLen,
                      &result[0], u8len, nullptr, nullptr);
  return result;
}

#elif defined(__APPLE__)

std::string normalizeNFKC(const std::string& utf8) {
  if (utf8.empty()) return utf8;

  CFStringRef cfStr = CFStringCreateWithBytes(
      kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(utf8.data()),
      static_cast<CFIndex>(utf8.size()),
      kCFStringEncodingUTF8, false);
  if (!cfStr) return utf8;

  CFMutableStringRef mutStr =
      CFStringCreateMutableCopy(kCFAllocatorDefault, 0, cfStr);
  CFRelease(cfStr);
  if (!mutStr) return utf8;

  CFStringNormalize(mutStr, kCFStringNormalizationFormKC);

  CFIndex len = CFStringGetLength(mutStr);
  CFIndex maxSize =
      CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
  std::string result(static_cast<size_t>(maxSize), '\0');
  if (!CFStringGetCString(mutStr, &result[0],
                          maxSize, kCFStringEncodingUTF8)) {
    CFRelease(mutStr);
    return utf8;
  }
  result.resize(std::strlen(result.c_str()));
  CFRelease(mutStr);
  return result;
}

#else

// Linux/Android: normalize at the platform layer (Java Normalizer, Python
// unicodedata.normalize) before calling into C++.  The C++ frontend is a
// safety net — if text arrives un-normalized, dictionary lookups may miss
// but synthesis still works.
std::string normalizeNFKC(const std::string& utf8) { return utf8; }

#endif

char32_t foldCodepointLower(char32_t c) {
  if (c >= 0x41 && c <= 0x5A) return c + 0x20;                       // A-Z
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;          // Latin-1 (not x)
  if (c == 0x130) return 0x69;                                       // Turkish dotted I
  if (c == 0x178) return 0xFF;                                       // Y-diaeresis
  if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))     // Latin Ext-A, even=upper
    return (c % 2 == 0) ? c + 1 : c;
  if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))     // Latin Ext-A, odd=upper
    return (c % 2 == 1) ? c + 1 : c;
  if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 0x20;       // Greek
  if (c >= 0x400 && c <= 0x40F) return c + 0x50;                     // Cyrillic Ѐ-Џ (Є І Ї ...)
  if (c >= 0x410 && c <= 0x42F) return c + 0x20;                     // Cyrillic А-Я
  if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF))     // Cyrillic Ext (Ґ ...)
    return (c % 2 == 0) ? c + 1 : c;
  return c;
}

std::string normalizeText(const std::string& s) {
  return normalizeNFKC(stripInvisible(s));
}

} // namespace nvsp_frontend
