#include "pass_pipeline.h"

#include "allophones.h"
#include "coarticulation.h"
#include "microprosody.h"
#include "nasalization.h"
#include "prosody.h"
#include "reduction.h"
#include "liquid_dynamics.h"
#include "length_contrast.h"
#include "boundary_smoothing.h"
#include "trajectory_limit.h"

namespace nvsp_frontend {

namespace {

const PassDesc kPasses[] = {
    {"nasalization", PassStage::PreTiming, &passes::runNasalization},
    {"allophones", PassStage::PreTiming, &passes::runAllophones},

    {"coarticulation", PassStage::PostTiming, &passes::runCoarticulation},
    {"prosody", PassStage::PostTiming, &passes::runProsody},
    {"reduction", PassStage::PostTiming, &passes::runReduction},
    {"liquid_dynamics", PassStage::PostTiming, &passes::runLiquidDynamics},
    {"length_contrast", PassStage::PostTiming, &passes::runLengthContrast},

    {"boundary_smoothing", PassStage::PostTiming, &passes::runBoundarySmoothing},
    {"trajectory_limit", PassStage::PostTiming, &passes::runTrajectoryLimit},

    {"microprosody", PassStage::PostPitch, &passes::runMicroprosody},
};

}  // namespace

bool runPasses(
    PassContext& ctx,
    PassStage stage,
    std::vector<Token>& tokens,
    std::string& outError) {
  for (const auto& pass : kPasses) {
    if (pass.fn == nullptr) continue;
    if (pass.stage != stage) continue;

    std::string err;
    if (!pass.fn(ctx, tokens, err)) {
      outError = std::string("pass '") + pass.name + "' failed: " + err;
      return false;
    }
  }
  return true;
}

}  // namespace nvsp_frontend
