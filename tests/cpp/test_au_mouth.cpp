// en-au MOUTH diphthong closure (Tomi's "th-ou-au-sand" report, 2026-08-26).
//
// Broad Australian MOUTH is [æɔ] — a fronted open onset gliding to a
// ROUNDED, CLOSER back target. The ɔ_oz offglide sat at 560/1000: the
// rendered glide moved F1 by ~34 Hz (594→560), so the diphthong never
// closed and read as two open vowels back to back — "separated, washy".
// The offglide now lands at 470/940. Ear-validated on thousand /
// two thousand / about / down / house / now.
//
// Pin: the collapsed MOUTH glide in "thousand" must actually close —
// its final micro-frame F1 clearly below its onset F1, and below 510 Hz.

#include <string>
#include <vector>

#include "doctest.h"
#include "pack_fixture.h"

#include "nvspFrontend.h"

namespace {

struct CapturedFrame {
    nvspFrontend_Frame f{};
    bool hasFrame = false;
    double durationMs = 0.0;
};

void captureCallback(void* userData, const nvspFrontend_Frame* f,
                     const nvspFrontend_FrameEx* /*fEx*/,
                     double durationMs, double /*fadeMs*/, int /*userIndex*/) {
    auto* out = static_cast<std::vector<CapturedFrame>*>(userData);
    CapturedFrame c;
    if (f) { c.f = *f; c.hasFrame = true; }
    c.durationMs = durationMs;
    out->push_back(c);
}

struct EnAuHandleFixture : tgsb_test::HandleFixture {
    EnAuHandleFixture() : HandleFixture("en-au") {}
};

}  // namespace

TEST_CASE_FIXTURE(EnAuHandleFixture,
                  "en-au MOUTH: the glide closes instead of staying open") {
    std::vector<CapturedFrame> frames;
    const int rc = nvspFrontend_queueIPA_Ex(handle, "θˈaʊzənd", 1.0, 140.0,
                                            0.5, ".", 0, &captureCallback,
                                            &frames);
    REQUIRE(rc != 0);

    // The MOUTH glide = the leading run of voiced frames with open-vowel
    // F1 (>450) before the /z/; collect it.
    std::vector<const CapturedFrame*> glide;
    for (const auto& c : frames) {
        if (!c.hasFrame) continue;
        if (c.f.voiceAmplitude > 0.3 && c.f.cf1 > 300.0) {
            glide.push_back(&c);
        } else if (!glide.empty()) {
            break;  // glide ended (frication/closure follows)
        }
    }
    REQUIRE_MESSAGE(glide.size() >= 3, "expected a multi-frame MOUTH glide");

    const double onsetF1 = glide.front()->f.cf1;
    const double endF1 = glide.back()->f.cf1;
    const double endF2 = glide.back()->f.cf2;

    // Closure: the offglide arrives clearly below the onset and below the
    // open-vowel zone (old behavior: 594 -> 560, a 34 Hz "glide").
    CHECK(endF1 < onsetF1 - 60.0);
    CHECK(endF1 < 510.0);
    // And it stays a BACK rounded target (F2 near 1 kHz, not centralized).
    CHECK(endF2 < 1150.0);
}
