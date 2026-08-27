// Steady-state vowel emission (#113, 2026-08-22).
//
// A coartic-shaped vowel used to render as ONE frame ramping onset→exit
// for its whole duration, so the canonical vowel never appeared: Spanish
// "dos" rendered /o/ as F2 1129→1105 Hz (canonical 910) — heard as [ø]
// ("like a Turkish person learning Spanish", #113; "körchete", Tomi).
// With lang.coarticulationSteadyState on, frame_emit splits the vowel
// into onset-transition → canonical steady → exit-transition, matching
// the Eloquence-lineage reference shape (transition ~35 ms, real steady
// mid-vowel, exit transition into the coda).
//
// Pinned structurally (no ears, no absolute-Hz pack pins):
//   1. "dos" /o/ emits ≥3 vowel frames; the middle one HOLDS (endCf NaN)
//      at a genuinely backer F2 than the onset (the steady state exists),
//      and every seam is continuous (no crossfade phasing possible).
//   2. The exit frame ramps away from steady (the VC cue survives).
//   3. At 2.0× rate the vowel is under the 45 ms floor and falls back to
//      the single-frame path (rate guard).

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

// The /o/ region of "dos": voiced frames with a vowel-sized F1.  The /d/
// pre-voicing frames sit at cf1≈170 and the /s/ is unvoiced, so this
// selector is unambiguous for this word.
std::vector<CapturedFrame> vowelFrames(const std::vector<CapturedFrame>& all) {
    std::vector<CapturedFrame> out;
    for (const auto& c : all)
        if (c.hasFrame && c.f.voiceAmplitude > 0.1 && c.f.cf1 > 350.0)
            out.push_back(c);
    return out;
}

}  // namespace

TEST_CASE_FIXTURE(tgsb_test::HandleFixture,
                  "es 'dos': /o/ renders a real canonical steady state") {
    auto vf = vowelFrames(emitFrames(handle, "dˈos", 1.0));
    REQUIRE_MESSAGE(vf.size() >= 3,
                    "expected onset/steady/exit vowel frames, got "
                    << vf.size());

    const auto& onset = vf.front();
    const auto& steady = vf[vf.size() - 2];
    const auto& exit = vf.back();

    // Onset transitions TOWARD the steady value it hands off to.
    CHECK(std::isfinite(onset.fx.endCf2));
    CHECK(onset.fx.endCf2 == doctest::Approx(steady.f.cf2).epsilon(0.001));

    // The steady frame HOLDS (no ramp target) at a genuinely backer F2
    // than the fronted onset — this is the frame b8 never emitted.
    CHECK_FALSE(std::isfinite(steady.fx.endCf2));
    CHECK(steady.f.cf2 < onset.f.cf2 - 100.0);

    // Exit frame starts at the steady value (continuous seam) and ramps
    // toward the /s/ locus (the #108 VC cue survives the split).
    CHECK(exit.f.cf2 == doctest::Approx(steady.f.cf2).epsilon(0.001));
    CHECK(std::isfinite(exit.fx.endCf2));
    CHECK(exit.fx.endCf2 > steady.f.cf2 + 100.0);

    // Internal frames must not re-fire pitch-model commands.
    CHECK(steady.fx.fujisakiReset == 0.0);
    CHECK(exit.fx.fujisakiReset == 0.0);
}

TEST_CASE_FIXTURE(tgsb_test::HandleFixture,
                  "es 'dos' at 2x rate: fast vowels keep their steady state") {
    // The floor is rate-relative (#113 follow-up: an absolute 45 ms floor
    // silently restored the [ø] above ~1.7x — caught by a native tester at
    // "61% speed" on every platform within hours of release).
    auto vf = vowelFrames(emitFrames(handle, "dˈos", 2.0));
    REQUIRE(vf.size() >= 3);
    CHECK_FALSE(std::isfinite(vf[vf.size() - 2].fx.endCf2));  // held steady
}

TEST_CASE_FIXTURE(tgsb_test::HandleFixture,
                  "es 'dos' at 4x rate: below the absolute minimum, single frame") {
    // Under ~22 ms (about three pitch periods) a three-segment split stops
    // making acoustic sense; the single-frame path remains the fallback.
    auto vf = vowelFrames(emitFrames(handle, "dˈos", 4.0));
    REQUIRE(vf.size() == 1);
    CHECK(std::isfinite(vf[0].fx.endCf2));
}
