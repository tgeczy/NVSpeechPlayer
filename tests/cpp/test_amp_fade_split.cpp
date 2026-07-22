// Trip-wire tests for the amplitude/formant fade split.
//
// Inside long fades (the slow-rate territory the frontend's 130ms fade cap
// still allows), amplitude sources (voice/aspiration/frication/turbulence
// amplitudes + preFormantGain) complete their old->new move within
// kAmpSpanMs (45ms) while spectral parameters glide across the full fade —
// the Klatt-lineage rule that amplitude changes are near-step events while
// formant transitions take articulatory time. Fades shorter than the span
// are untouched, so normal-rate output is bit-identical (verified against
// the pre-change build when this landed).
//
// Measured at 7% speed when this landed (2026-07-23, v-310 b7):
//   voicing onset rise (10->90%) after /b/ in "able": 37.7ms -> 20.3ms
//   utterance-final decay (50%->5% of peak):           78.4ms -> 31.9ms
// Ear-validated en/hu/es; Tomi's observation: word-final /l/ "came on more
// clearly and didn't smear out while ending".

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "doctest.h"
#include "pack_fixture.h"
#include "audio_capture.h"

#include "nvspFrontend.h"

namespace {

// Short-window RMS envelope: window 128 samples, hop 64 (5.8/2.9ms @22050).
std::vector<double> rmsEnvelope(const std::vector<std::int16_t>& pcm,
                                std::size_t win = 128, std::size_t hop = 64) {
    std::vector<double> env;
    for (std::size_t i = 0; i + win <= pcm.size(); i += hop) {
        double acc = 0.0;
        for (std::size_t j = i; j < i + win; ++j) {
            const double s = pcm[j] / 32768.0;
            acc += s * s;
        }
        env.push_back(std::sqrt(acc / win));
    }
    return env;
}

double hopMs(int sampleRate, std::size_t hop = 64) {
    return 1000.0 * hop / sampleRate;
}

struct EnUsHandleFixture : tgsb_test::HandleFixture {
    EnUsHandleFixture() : tgsb_test::HandleFixture("en-us") {}
};

void fadeCaptureCallback(void* userData, const nvspFrontend_Frame* /*f*/,
                         const nvspFrontend_FrameEx* /*fEx*/,
                         double /*durationMs*/, double fadeMs, int /*userIndex*/) {
    auto* fades = static_cast<std::vector<double>*>(userData);
    fades->push_back(fadeMs);
}

}  // namespace

TEST_CASE_FIXTURE(EnUsHandleFixture,
                  "amp split: utterance-final decay stays tight at slow rate") {
    // "able" at 7%: the final /l/ must end decisively. Pre-split the ending
    // smeared across the full 130ms trailing fade (measured 78.4ms decay);
    // post-split the amplitude completes early (measured 31.9ms).
    const auto pcm = tgsb_test::synthesizeToPcm(handle, "ˈeɪbəl", 0.07);
    REQUIRE(!pcm.empty());
    const auto env = rmsEnvelope(pcm);
    REQUIRE(env.size() > 10);

    const double peak = *std::max_element(env.begin(), env.end());
    REQUIRE(peak > 0.0);

    std::size_t iEnd = 0;
    for (std::size_t i = 0; i < env.size(); ++i)
        if (env[i] > 0.5 * peak) iEnd = i;
    std::size_t iQuiet = env.size() - 1;
    for (std::size_t i = iEnd; i < env.size(); ++i) {
        if (env[i] < 0.05 * peak) { iQuiet = i; break; }
    }
    const double decayMs = (iQuiet - iEnd) * hopMs(22050);
    INFO("final decay (50% -> 5% of peak) = " << decayMs << "ms");
    CHECK_MESSAGE(decayMs <= 55.0,
                  "slow-rate utterance ending smears again — amplitude fades "
                  "are riding the full stretched fade instead of kAmpSpanMs");
}

TEST_CASE_FIXTURE(EnUsHandleFixture,
                  "amp split: voicing onset after closure rises fast at slow rate") {
    // Rise from the /b/ closure floor into the schwa. Pre-split 37.7ms,
    // post-split 20.3ms; threshold 30ms sits between with margin. If the
    // utterance's timing shape changes enough to move the closure, the
    // located minimum will still be the quietest interior point — update
    // the expectations here rather than the mechanism.
    const auto pcm = tgsb_test::synthesizeToPcm(handle, "ˈeɪbəl", 0.07);
    REQUIRE(!pcm.empty());
    const auto env = rmsEnvelope(pcm);
    REQUIRE(env.size() > 20);

    // Quietest point in the interior 20-80% of the utterance = closure.
    const std::size_t lo = env.size() / 5, hi = env.size() * 4 / 5;
    std::size_t iMin = lo;
    for (std::size_t i = lo; i < hi; ++i)
        if (env[i] < env[iMin]) iMin = i;

    // Post-closure level: max within 600ms after the minimum.
    const std::size_t span = static_cast<std::size_t>(600.0 / hopMs(22050));
    const std::size_t end = std::min(env.size(), iMin + span);
    double post = 0.0;
    for (std::size_t i = iMin; i < end; ++i) post = std::max(post, env[i]);
    REQUIRE(post > env[iMin]);

    const double floor10 = env[iMin] + 0.1 * (post - env[iMin]);
    const double floor90 = env[iMin] + 0.9 * (post - env[iMin]);
    std::size_t i10 = end, i90 = end;
    for (std::size_t i = iMin; i < end; ++i) {
        if (i10 == end && env[i] > floor10) i10 = i;
        if (env[i] > floor90) { i90 = i; break; }
    }
    const double riseMs = (i90 - i10) * hopMs(22050);
    INFO("voicing onset rise (10% -> 90%) = " << riseMs << "ms");
    CHECK_MESSAGE(riseMs <= 30.0,
                  "slow-rate voicing onset smears again — amplitude clock "
                  "is not compressing inside long fades");
}

TEST_CASE_FIXTURE(EnUsHandleFixture,
                  "amp split: normal speed never reaches the 45ms span (documents baseline)") {
    // Across the b7 survey words, the longest fade at speed 1.0 is 26.8ms —
    // under kAmpSpanMs, so the split cannot engage and 1.0 output is
    // bit-identical to the pre-split build. If fades at 1.0 ever grow past
    // 45ms, this fires: either the timing model changed intentionally
    // (re-verify normal-rate output by ear) or something started stretching
    // fades that shouldn't.
    for (const char* ipa : {"ˈeɪbəl", "bɛst"}) {
        CAPTURE(ipa);
        std::vector<double> fades;
        const int rc = nvspFrontend_queueIPA_Ex(handle, ipa, 1.0, 140.0, 0.5,
                                                ".", 0, &fadeCaptureCallback,
                                                &fades);
        REQUIRE(rc != 0);
        REQUIRE(!fades.empty());
        const double m = *std::max_element(fades.begin(), fades.end());
        INFO("max fade at speed 1.0 for " << ipa << " = " << m << "ms");
        CHECK(m <= 45.0);
    }
}
