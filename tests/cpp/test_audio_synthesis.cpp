// First real acoustic tests: synthesize via the actual DSP, inspect the
// emitted PCM samples. No FFT yet — these tests just prove the audio-
// capture harness works end-to-end and pin basic audio invariants
// (non-silence, reasonable amplitude, expected duration).
//
// Once these pass, FFT-based spectral assertions become possible in a
// follow-up file.

#include "doctest.h"
#include "audio_capture.h"
#include "pack_fixture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>

using tgsb_test::HandleFixture;
using tgsb_test::synthesizeToPcm;

// RMS amplitude as a fraction of full-scale int16 (32767).
static double rmsLevel(const std::vector<std::int16_t>& pcm) {
    if (pcm.empty()) return 0.0;
    double sumSq = 0.0;
    for (std::int16_t s : pcm) {
        const double x = static_cast<double>(s) / 32767.0;
        sumSq += x * x;
    }
    return std::sqrt(sumSq / static_cast<double>(pcm.size()));
}

// Peak absolute amplitude as a fraction of full-scale.
static double peakLevel(const std::vector<std::int16_t>& pcm) {
    std::int16_t maxAbs = 0;
    for (std::int16_t s : pcm) {
        const std::int16_t a = s < 0 ? static_cast<std::int16_t>(-s) : s;
        if (a > maxAbs) maxAbs = a;
    }
    return static_cast<double>(maxAbs) / 32767.0;
}

TEST_CASE_FIXTURE(HandleFixture,
                  "audio: /aɣa/ synthesis produces non-silent PCM") {
    auto pcm = synthesizeToPcm(handle, "aɣa", /*speed*/ 1.0);
    REQUIRE_MESSAGE(!pcm.empty(),
                    "synthesizeToPcm returned empty buffer — frontend or "
                    "DSP error on simple input");
    INFO("samples=" << pcm.size()
         << "  rms=" << rmsLevel(pcm)
         << "  peak=" << peakLevel(pcm));
    // /aɣa/ at speed 1.0 should be ~150-200 ms = 3300-4400 samples at 22050 Hz
    CHECK(pcm.size() > 1000);
    CHECK(rmsLevel(pcm) > 0.02);   // at least 2% RMS — audible
    CHECK(peakLevel(pcm) > 0.1);   // peak reaches at least 10% FS
    CHECK(peakLevel(pcm) < 1.0);   // not slamming the limiter
}

TEST_CASE_FIXTURE(HandleFixture,
                  "audio: /aɣa/ vs /ala/ produce distinguishable waveforms") {
    auto g_pcm = synthesizeToPcm(handle, "aɣa", 1.0);
    auto l_pcm = synthesizeToPcm(handle, "ala", 1.0);
    REQUIRE(!g_pcm.empty());
    REQUIRE(!l_pcm.empty());

    // Exhaustive waveform comparison isn't meaningful — two utterances
    // with the same vowels differ in microsecond-scale phase details even
    // when both sound the same. But if they were IDENTICAL (same
    // checksum, same length), something is deeply broken upstream.
    INFO("ɣa samples=" << g_pcm.size() << " rms=" << rmsLevel(g_pcm));
    INFO("ala samples=" << l_pcm.size() << " rms=" << rmsLevel(l_pcm));

    // Different length OR different RMS to at least some degree → they're
    // distinguishable numerically even before we look at spectra.
    const bool lengthDiffers = g_pcm.size() != l_pcm.size();
    const bool rmsDiffers = std::abs(rmsLevel(g_pcm) - rmsLevel(l_pcm)) > 0.001;
    CHECK((lengthDiffers || rmsDiffers));
}

TEST_CASE_FIXTURE(HandleFixture,
                  "audio: silence input produces all-zero PCM") {
    // An empty IPA string should produce zero (or near-zero) output.
    // If this ever generates audible samples, somewhere state is leaking
    // from a previous utterance.
    auto pcm = synthesizeToPcm(handle, "", 1.0);
    // Frontend may return empty buffer for empty input, OR a short buffer
    // of silence. Either is acceptable as long as RMS is near zero.
    if (!pcm.empty()) {
        const double rms = rmsLevel(pcm);
        INFO("empty-input PCM: " << pcm.size() << " samples, RMS=" << rms);
        CHECK(rms < 0.001);  // near-silence
    }
}

TEST_CASE_FIXTURE(HandleFixture,
                  "audio: faster speed produces shorter waveform") {
    auto slow_pcm = synthesizeToPcm(handle, "aɣa", /*speed*/ 1.0);
    auto fast_pcm = synthesizeToPcm(handle, "aɣa", /*speed*/ 2.0);
    REQUIRE(!slow_pcm.empty());
    REQUIRE(!fast_pcm.empty());

    INFO("speed=1.0: " << slow_pcm.size() << " samples");
    INFO("speed=2.0: " << fast_pcm.size() << " samples");

    // 2x speed → roughly half the samples. Allow wide tolerance for
    // tail / silence padding.
    CHECK(fast_pcm.size() < slow_pcm.size());
    const double ratio = static_cast<double>(fast_pcm.size()) / slow_pcm.size();
    CHECK(ratio > 0.3);
    CHECK(ratio < 0.9);
}
