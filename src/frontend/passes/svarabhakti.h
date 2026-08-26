/*
TGSpeechBox — Svarabhakti vowel-quality inheritance pass interface.
Copyright 2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSB_FRONTEND_PASSES_SVARABHAKTI_H
#define TGSB_FRONTEND_PASSES_SVARABHAKTI_H

#include "pass_common.h"

namespace nvsp_frontend::passes {

// Inserted svarabhakti vocoids (the ᵊ that language packs place around
// taps/trills) inherit the neighboring vowel's formant quality and shrink
// toward their natural length, instead of rendering as a fixed neutral
// schwa. Per-language opt-in (lang.svarabhaktiInheritEnabled).
bool runSvarabhakti(
  PassContext& ctx,
  std::vector<Token>& tokens,
  std::string& outError
);

} // namespace nvsp_frontend::passes

#endif // TGSB_FRONTEND_PASSES_SVARABHAKTI_H
