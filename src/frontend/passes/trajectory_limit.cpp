#include "trajectory_limit.h"

#include <algorithm>
#include <cmath>

namespace nvsp_frontend::passes {

namespace {

static inline bool tokIsSilenceOrMissing(const Token& t) {
  return t.silence || !t.def;
}

static inline double getResolvedField(const Token& t, int idx) {
  const uint64_t bit = 1ULL << idx;
  if (t.setMask & bit) return t.field[idx];
  if (t.def && (t.def->setMask & bit)) return t.def->field[idx];
  return 0.0;
}

static inline void clampFade(Token& t) {
  if (t.fadeMs < 0.0) t.fadeMs = 0.0;
  if (t.durationMs < 0.0) t.durationMs = 0.0;
  if (t.fadeMs > t.durationMs) t.fadeMs = t.durationMs;
}

}  // namespace

bool runTrajectoryLimit(PassContext& ctx, std::vector<Token>& tokens, std::string& outError) {
  (void)outError;

  const auto& lang = ctx.pack.lang;
  if (!lang.trajectoryLimitEnabled) return true;
  if (tokens.size() < 2) return true;

  const double sp = (ctx.speed > 0.0) ? ctx.speed : 1.0;
  const double win = std::max(0.0, lang.trajectoryLimitWindowMs) / sp;
  if (win <= 0.0) return true;

  const std::uint64_t mask = lang.trajectoryLimitApplyMask;
  if (mask == 0) return true;

  for (size_t i = 1; i < tokens.size(); ++i) {
    const Token& prev = tokens[i - 1];
    Token& cur = tokens[i];

    if (tokIsSilenceOrMissing(prev) || tokIsSilenceOrMissing(cur)) {
      continue;
    }

    // Optional: do not smooth across word boundaries.
    if (!lang.trajectoryLimitApplyAcrossWordBoundary && cur.wordStart) {
      continue;
    }

    // The fade belongs to the *incoming* token (cur) and is the time over
    // which its targets are interpolated from the previous token.
    const double curFade = std::max(0.001, cur.fadeMs);

    double neededFade = 0.0;

    for (int idx = 0; idx < static_cast<int>(kFrameFieldCount); ++idx) {
      if ((mask & (1ULL << idx)) == 0) continue;

      const double maxHzPerMs = lang.trajectoryLimitMaxHzPerMs[static_cast<size_t>(idx)];
      if (maxHzPerMs <= 0.0) continue;

      const double a = getResolvedField(prev, idx);
      const double b = getResolvedField(cur, idx);
      if (a <= 0.0 || b <= 0.0) continue;

      const double delta = std::abs(b - a);
      if (delta <= 1e-6) continue;

      const double required = delta / maxHzPerMs;
      if (required > neededFade) neededFade = required;
    }

    // Only act if current fade is shorter than what we need.
    if (neededFade > curFade) {
      // Do not extend beyond the configured window.
      const double target = std::min(neededFade, win);
      if (target > cur.fadeMs) {
        cur.fadeMs = target;
        clampFade(cur);
      }
    }
  }

  return true;
}

}  // namespace nvsp_frontend::passes
