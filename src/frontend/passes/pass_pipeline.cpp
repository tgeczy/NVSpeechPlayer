/*
TGSpeechBox — Pass pipeline registration and execution.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include "pass_pipeline.h"

#include "allophones.h"
#include "syllable_marking.h"
#include "coarticulation.h"
#include "microprosody.h"
#include "nasalization.h"
#include "prosody.h"
#include "rate_compensation.h"
#include "liquid_dynamics.h"
#include "length_contrast.h"
#include "diphthong_collapse.h"
#include "boundary_smoothing.h"
#include "trajectory_limit.h"
#include "cluster_timing.h"
#include "cluster_blend.h"
#include "special_coartic.h"
#include "prominence.h"

#include "../utf8.h"

namespace nvsp_frontend {

namespace {

// Capture the post-pass acoustic state of every non-silence token.
// Resolves each field from Token.field if setMask bit is set, otherwise
// falls back to the PhonemeDef default. Silence tokens (def == nullptr)
// are skipped — they have no acoustic state to snapshot.
void snapshotTokens(const char* passName,
                    const std::vector<Token>& tokens,
                    std::vector<tgsb_data::PassSnapshot>& sink) {
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const Token& t = tokens[i];
    if (!t.def) continue;

    auto resolve = [&](FieldId id) -> double {
      const int idx = static_cast<int>(id);
      const std::uint64_t bit = 1ULL << idx;
      if (t.setMask & bit) return t.field[idx];
      if (t.def->setMask & bit) return t.def->field[idx];
      return 0.0;
    };

    tgsb_data::PassSnapshot s;
    s.passName = passName;
    s.tokenIndex = static_cast<int>(i);
    s.phonemeKey = u32ToUtf8(t.def->key);
    s.cf1 = resolve(FieldId::cf1);
    s.cf2 = resolve(FieldId::cf2);
    s.cf3 = resolve(FieldId::cf3);
    s.pf1 = resolve(FieldId::pf1);
    s.pf2 = resolve(FieldId::pf2);
    s.pf3 = resolve(FieldId::pf3);
    s.voiceAmplitude = resolve(FieldId::voiceAmplitude);
    s.aspirationAmplitude = resolve(FieldId::aspirationAmplitude);
    s.fricationAmplitude = resolve(FieldId::fricationAmplitude);
    s.durationMs = t.durationMs;
    s.fadeMs = t.fadeMs;
    sink.push_back(std::move(s));
  }
}

const PassDesc kPasses[] = {
    {"syllable_marking", PassStage::PreTiming, &passes::runSyllableMarking},
    {"nasalization", PassStage::PreTiming, &passes::runNasalization},
    {"allophones", PassStage::PreTiming, &passes::runAllophones},

    {"coarticulation", PassStage::PostTiming, &passes::runCoarticulation},
    {"special_coartic", PassStage::PostTiming, &passes::runSpecialCoarticulation},
    {"cluster_timing", PassStage::PostTiming, &passes::runClusterTiming},
    {"cluster_blend", PassStage::PostTiming, &passes::runClusterBlend},
    {"prominence", PassStage::PostTiming, &passes::runProminence},
    {"prosody", PassStage::PostTiming, &passes::runProsody},
    {"rate_compensation", PassStage::PostTiming, &passes::runRateCompensation},
    {"liquid_dynamics", PassStage::PostTiming, &passes::runLiquidDynamics},
    {"length_contrast", PassStage::PostTiming, &passes::runLengthContrast},
    {"diphthong_collapse", PassStage::PostTiming, &passes::runDiphthongCollapse},

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

    // Post-pass instrumentation. Nullptr sink (production path) is a single
    // predictable branch per pass — no snapshot work performed.
    if (ctx.passTraceSink) {
      snapshotTokens(pass.name, tokens, *ctx.passTraceSink);
    }
  }
  return true;
}

}  // namespace nvsp_frontend
