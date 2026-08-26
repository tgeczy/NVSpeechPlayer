// Svarabhakti vowel-quality inheritance (#113 second gem, 2026-08-26).
//
// Language packs insert a svarabhakti vocoid (ᵊ) around taps — Spanish:
// intervocalic ɾ → ᵊɾ, word-final ɾ → ɾ_wf ᵊ ɾ_wf. It rendered as ONE
// fixed neutral schwa (≈500/1400, ~37 ms, full voice), so far/fer/fir/
// for/fur all measured as the same vowel (~410/1510) — "far" read as
// "farə". Per the phonetics literature the element replicates the
// nuclear vowel's quality at ~1/4 its duration (da Silva 2024,
// DOI 10.20396/joss.v13i00.19957) with carryover direction (Recasens
// 1991, DOI 10.1016/s0095-4470(19)30344-4).
//
// Pins (frame-level, es-mx fixture — inherits es.yaml's opt-in):
//   1. "far" vs "for": the ᵊ echo frames differ by hundreds of Hz and
//      each sits near ITS donor vowel, at reduced duration/amplitude.
//   2. Word-final tap holds are redirected: no voiced schwa-ish frame
//      longer than ~30 ms after the nuclear vowel in a single r-final
//      word (the old 42 ms held tap + 37 ms schwa are both gone).

#include <cmath>
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

std::vector<CapturedFrame> emitFrames(nvspFrontend_handle_t h,
                                      const std::string& ipa) {
    std::vector<CapturedFrame> frames;
    const int rc = nvspFrontend_queueIPA_Ex(h, ipa.c_str(), 1.0, 140.0, 0.5,
                                            ".", 0, &captureCallback, &frames);
    REQUIRE_MESSAGE(rc != 0, "queueIPA_Ex failed for '" << ipa << "'");
    return frames;
}

// The svarabhakti echo in a d/f + V + tap word: a voiced frame between
// the (louder) nuclear vowel and the (quieter) tap material, at reduced
// amplitude — va in (0.6, 0.85) with vowel-sized F1 uniquely identifies
// it in these words.
const CapturedFrame* findEcho(const std::vector<CapturedFrame>& fs) {
    for (const auto& c : fs)
        if (c.hasFrame && c.f.voiceAmplitude > 0.6 &&
            c.f.voiceAmplitude < 0.85 && c.f.cf1 > 250.0)
            return &c;
    return nullptr;
}

}  // namespace

TEST_CASE_FIXTURE(tgsb_test::HandleFixture,
                  "svarabhakti: the vocoid echoes ITS vowel, briefly") {
    auto far = emitFrames(handle, "fˈaɾ");
    auto forr = emitFrames(handle, "fˈoɾ");
    const auto* eA = findEcho(far);
    const auto* eO = findEcho(forr);
    REQUIRE(eA != nullptr);
    REQUIRE(eO != nullptr);

    // Each echo lives near its donor: /a/-context F1 well above
    // /o/-context F1, F2s separated — NOT one shared neutral schwa.
    CHECK(eA->f.cf1 > eO->f.cf1 + 120.0);
    CHECK(std::abs(eA->f.cf2 - eO->f.cf2) > 250.0);

    // Svarabhakti proportions: a brief echo, not a syllable (the old
    // fixed schwa ran ~37.5 ms at va 0.90).
    CHECK(eA->durationMs < 25.0);
    CHECK(eA->f.voiceAmplitude < 0.85);
}

TEST_CASE_FIXTURE(tgsb_test::HandleFixture,
                  "svarabhakti: final-tap hold redirected to the nucleus") {
    auto fs = emitFrames(handle, "amˈoɾ");
    // Find the nuclear /o/ (loud, F1 near 490) and everything after it.
    int nucleus = -1;
    for (int i = 0; i < int(fs.size()); ++i)
        if (fs[i].hasFrame && fs[i].f.voiceAmplitude > 0.85 &&
            fs[i].f.cf1 > 400.0 && fs[i].f.cf1 < 600.0)
            nucleus = i;  // last such frame
    REQUIRE(nucleus >= 0);
    // No post-nucleus voiced frame may run ≥30 ms: the word-final hold
    // lands on the vowel now, not on the tap's static constriction.
    for (int i = nucleus + 1; i < int(fs.size()); ++i) {
        if (!fs[i].hasFrame) continue;
        if (fs[i].f.voiceAmplitude > 0.3)
            CHECK(fs[i].durationMs < 30.0);
    }
}
