#pragma once

#include "pass_common.h"

namespace nvsp_frontend::passes {

// Boundary smoothing / crossfade.
//
// Adjusts per-token fadeMs at harsh boundaries (e.g. vowel->stop) without
// changing durations or phoneme targets.
bool runBoundarySmoothing(PassContext& ctx, std::vector<Token>& tokens, std::string& outError);

}  // namespace nvsp_frontend::passes
