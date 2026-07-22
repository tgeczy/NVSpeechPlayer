// Trip-wire tests for closure-filler FrameEx inheritance.
//
// Voice bar (voiced stop closures) and coda noise taper (fricative→stop)
// frames inherit the previous phoneme's FrameEx to keep the Fujisaki pitch
// model alive across the closure. They must NOT inherit its endCf/endPf
// formant ramp targets: in the DSP, a finite endCf overrides the frame's
// own formant values entirely (per-sample exponential ramp toward the
// target), so a stale target pins the closure's spectrum to the phoneme
// it just left. At normal rates that's a ~20 ms smear; at 5-15% speech
// rate the stretched closure renders a loud sustained ghost of the prior
// phoneme ("able" = quiet copy of /eɪ/ filling the /b/ closure).
//
// Found + fixed 2026-07-22 (v-310 b7 cycle). The diphthong is the classic
// trigger because its macro-frame FrameEx carries offset targets in
// endCf1-3, and the ghost is loudest when vowel formants sit far from the
// closure's locus (front vowel → labial: F2 1900 vs 1100).

#include <cmath>
#include <string>
#include <vector>

#include "doctest.h"
#include "pack_fixture.h"

#include "nvspFrontend.h"

namespace {

struct CapturedFrame {
    nvspFrontend_Frame f{};
    nvspFrontend_FrameEx fx{};
    bool hasFrame = false;
    double durationMs = 0.0;
    double fadeMs = 0.0;
};

void captureCallback(void* userData, const nvspFrontend_Frame* f,
                     const nvspFrontend_FrameEx* fEx,
                     double durationMs, double fadeMs, int /*userIndex*/) {
    auto* out = static_cast<std::vector<CapturedFrame>*>(userData);
    CapturedFrame c;
    if (f) { c.f = *f; c.hasFrame = true; }
    if (fEx) c.fx = *fEx;
    c.durationMs = durationMs;
    c.fadeMs = fadeMs;
    out->push_back(c);
}

std::vector<CapturedFrame> emitFrames(nvspFrontend_handle_t h,
                                      const std::string& ipa, double speed) {
    std::vector<CapturedFrame> frames;
    const int rc = nvspFrontend_queueIPA_Ex(h, ipa.c_str(), speed, 140.0, 0.5,
                                            ".", 0, &captureCallback, &frames);
    REQUIRE_MESSAGE(rc != 0, "queueIPA_Ex failed for '" << ipa << "'");
    return frames;
}

bool hasRampTargets(const nvspFrontend_FrameEx& fx) {
    return std::isfinite(fx.endCf1) || std::isfinite(fx.endCf2) ||
           std::isfinite(fx.endCf3) || std::isfinite(fx.endPf1) ||
           std::isfinite(fx.endPf2) || std::isfinite(fx.endPf3);
}

struct EnUsHandleFixture : tgsb_test::HandleFixture {
    EnUsHandleFixture() : tgsb_test::HandleFixture("en-us") {}
};

}  // namespace

TEST_CASE_FIXTURE(EnUsHandleFixture,
                  "closure fillers: voice bar frames carry no formant ramp targets") {
    // "able": /eɪ/ (whose FrameEx carries diphthong offset targets in
    // endCf1-3) followed by a voiced-stop closure. The voice bar is the only
    // emitted frame with voiceAmplitude > 0 and preFormantGain < 1.0.
    // Checked at normal speed and at the slow rate where the bug was audible.
    for (double speed : {1.0, 0.07}) {
        CAPTURE(speed);
        auto frames = emitFrames(handle, "ˈeɪbəl", speed);
        int voiceBars = 0;
        for (const auto& c : frames) {
            if (!c.hasFrame) continue;
            if (c.f.voiceAmplitude > 0.0 && c.f.preFormantGain < 1.0) {
                ++voiceBars;
                CHECK_MESSAGE(!hasRampTargets(c.fx),
                              "voice bar inherited stale endCf/endPf (endCf2="
                              << c.fx.endCf2 << ") — closure spectrum will pin "
                              "to the prior phoneme (slow-rate ghost/echo)");
            }
        }
        REQUIRE_MESSAGE(voiceBars > 0,
                        "no voice bar frame found in 'able' — emission shape "
                        "changed; update this test's filler signature");
    }
}

TEST_CASE_FIXTURE(EnUsHandleFixture,
                  "closure fillers: coda fric->stop taper frames carry no formant ramp targets") {
    // "best": /s/→/t/ coda triggers the noise taper. Taper frames are the
    // unvoiced fillers with preFormantGain < 1.0 (phoneme frames use >= 2.0).
    // Pre-fix they inherited the /s/'s coarticulation endCf, silently
    // overriding the taper's own 40% place blend toward the stop.
    auto frames = emitFrames(handle, "bɛst", 1.0);
    int taperFrames = 0;
    for (const auto& c : frames) {
        if (!c.hasFrame) continue;
        if (c.f.voiceAmplitude == 0.0 && c.f.preFormantGain < 1.0) {
            ++taperFrames;
            CHECK_MESSAGE(!hasRampTargets(c.fx),
                          "coda taper inherited stale endCf/endPf (endCf2="
                          << c.fx.endCf2 << ") — its place blending toward the "
                          "stop is silently overridden");
        }
    }
    REQUIRE_MESSAGE(taperFrames > 0,
                    "no coda taper frames found in 'best' — emission shape "
                    "changed; update this test's filler signature");
}
