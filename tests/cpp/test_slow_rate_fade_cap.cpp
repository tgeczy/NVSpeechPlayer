// Trip-wire tests for the slow-rate absolute fade cap.
//
// Transitions are articulatory events, not rate-elastic ones: the Klatt
// lineage keeps transition spans in absolute time instead of scaling them
// with 1/speed. Before the cap, 5-15% speech rates stretched fades to
// 150-360 ms, smearing every segment's onset across its neighbor (and the
// utterance-final fade to 357 ms). generateAcousticEvents now clamps every
// emitted fade to kSlowRateFadeCapMs (130 ms) when speed < 1.0; speeds at
// or above 1.0 are untouched by construction.
//
// Ear-validated 2026-07-22 on en/hu/es at 7% and 15% rates (v-310 b7).

#include <algorithm>
#include <string>
#include <vector>

#include "doctest.h"
#include "pack_fixture.h"

#include "nvspFrontend.h"

namespace {

constexpr double kCapMs = 130.0;

struct EmittedFade {
    double fadeMs;
    double durationMs;
};

void fadeCaptureCallback(void* userData, const nvspFrontend_Frame* /*f*/,
                         const nvspFrontend_FrameEx* /*fEx*/,
                         double durationMs, double fadeMs, int /*userIndex*/) {
    static_cast<std::vector<EmittedFade>*>(userData)
        ->push_back({fadeMs, durationMs});
}

std::vector<EmittedFade> emitFades(nvspFrontend_handle_t h,
                                   const std::string& ipa, double speed) {
    std::vector<EmittedFade> fades;
    const int rc = nvspFrontend_queueIPA_Ex(h, ipa.c_str(), speed, 140.0, 0.5,
                                            ".", 0, &fadeCaptureCallback, &fades);
    REQUIRE_MESSAGE(rc != 0, "queueIPA_Ex failed for '" << ipa << "'");
    REQUIRE(!fades.empty());
    return fades;
}

double maxFade(const std::vector<EmittedFade>& v) {
    double m = 0.0;
    for (const auto& e : v) m = std::max(m, e.fadeMs);
    return m;
}

struct EnUsHandleFixture : tgsb_test::HandleFixture {
    EnUsHandleFixture() : tgsb_test::HandleFixture("en-us") {}
};

struct EsMxHandleFixture : tgsb_test::HandleFixture {
    EsMxHandleFixture() : tgsb_test::HandleFixture("es-mx") {}
};

}  // namespace

TEST_CASE_FIXTURE(EnUsHandleFixture,
                  "fade cap: no emitted fade exceeds 130ms at slow rates (en)") {
    for (double speed : {0.5, 0.15, 0.07}) {
        CAPTURE(speed);
        const auto fades = emitFades(handle, "ˈeɪbəl", speed);
        const double m = maxFade(fades);
        INFO("max emitted fade at speed " << speed << " = " << m << "ms");
        CHECK_MESSAGE(m <= kCapMs + 0.001,
                      "slow-rate fade escaped the absolute cap — transition "
                      "spans must not scale with 1/speed (segment-onset smear)");
    }
}

TEST_CASE_FIXTURE(EsMxHandleFixture,
                  "fade cap: no emitted fade exceeds 130ms at slow rates (es)") {
    // Real word with lenited /ɣ/ and coda /ɾ/ — the es ear-test word.
    for (double speed : {0.15, 0.07}) {
        CAPTURE(speed);
        const auto fades = emitFades(handle, "diˈaloɣaɾ", speed);
        const double m = maxFade(fades);
        INFO("max emitted fade at speed " << speed << " = " << m << "ms");
        CHECK_MESSAGE(m <= kCapMs + 0.001,
                      "slow-rate fade escaped the absolute cap");
    }
}

TEST_CASE_FIXTURE(EnUsHandleFixture,
                  "fade cap: speed 1.0 never reaches cap territory (documents baseline)") {
    // At normal speed the longest fades observed are ~25-30ms; the cap's
    // speed < 1.0 gate means 1.0 output is bit-identical with or without
    // the cap. If normal-rate fades ever grow past 130ms, this fires and
    // forces a review — either the timing model changed intentionally, or
    // something started rate-scaling that shouldn't.
    const auto fades = emitFades(handle, "ˈeɪbəl", 1.0);
    const double m = maxFade(fades);
    INFO("max emitted fade at speed 1.0 = " << m << "ms");
    CHECK(m < kCapMs);
}

TEST_CASE_FIXTURE(EnUsHandleFixture,
                  "fade cap: durations still scale with rate (only fades are capped)") {
    // The elastic part of slow speech is segment duration, and that must
    // keep stretching. Total utterance time at 7% should be several times
    // the 1.0 total (floors and fixed-length events keep it under a full
    // 14x, so the bound is deliberately loose).
    auto total = [](const std::vector<EmittedFade>& v) {
        double t = 0.0;
        for (const auto& e : v) t += e.durationMs;
        return t;
    };
    const double t100 = total(emitFades(handle, "ˈeɪbəl", 1.0));
    const double t007 = total(emitFades(handle, "ˈeɪbəl", 0.07));
    INFO("total ms at 1.0 = " << t100 << ", at 0.07 = " << t007);
    CHECK(t007 > t100 * 5.0);
}
