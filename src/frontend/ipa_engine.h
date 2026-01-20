#ifndef NVSP_FRONTEND_IPA_ENGINE_H
#define NVSP_FRONTEND_IPA_ENGINE_H

#include <string>
#include <vector>

#include "pack.h"
#include "nvspFrontend.h"

namespace nvsp_frontend {

struct Token {
  // If def is null, this token is "silence" (no frame).
  const PhonemeDef* def = nullptr;

  // Per-token field values (only valid where setMask has the bit set).
  std::uint64_t setMask = 0;
  double field[kFrameFieldCount] = {0.0};

  // Meta.
  bool silence = false;
  bool preStopGap = false;
  bool clusterGap = false;
  bool postStopAspiration = false;
  bool vowelHiatusGap = false;

  bool wordStart = false;
  bool syllableStart = false;
  int stress = 0; // 0 none, 1 primary, 2 secondary

  bool tiedTo = false;
  bool tiedFrom = false;
  bool lengthened = false;

  // Base char used for some tweaks (like Hungarian short vowel checks).
  char32_t baseChar = 0;

  // Timing (ms) computed later.
  double durationMs = 0.0;
  double fadeMs = 0.0;

  // Tonal marker captured for this syllable start (UTF-32 string), if any.
  std::u32string tone;
};

// Convert IPA -> tokens.
// This runs:
//  1) normalization (pack rules)
//  2) phoneme mapping
//  3) gap insertion
//  4) copy-adjacent correction
//  5) transforms
//  6) timing + pitch
//
// On success, tokens are ready to be converted to nvspFrontend_Frame.
bool convertIpaToTokens(
  const PackSet& pack,
  const std::string& ipaUtf8,
  double speed,
  double basePitch,
  double inflection,
  char clauseType,
  std::vector<Token>& outTokens,
  std::string& outError
);

// Convert tokens -> callback frames.
void emitFrames(
  const PackSet& pack,
  const std::vector<Token>& tokens,
  int userIndexBase,
  nvspFrontend_FrameCallback cb,
  void* userData
);

} // namespace nvsp_frontend

#endif
