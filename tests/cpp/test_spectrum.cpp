// First acoustic-perceptual tests: synthesize real DSP audio, extract
// formant peaks via FFT, assert they land where the PhonemeDef said they
// should. This is the "sense without ears" layer working end-to-end.
//
// If ANY of these start failing, the DSP is doing something to the
// phoneme that the static YAML parameters don't predict — exactly the
// kind of discrepancy that could explain user-reported "X sounds like Y"
// regressions that the Token-level tests can't catch.

#include "doctest.h"
#include "audio_capture.h"
#include "pack_fixture.h"
#include "spectrum_helpers.h"

#include <algorithm>

using tgsb_test::computeMagnitudeSpectrum;
using tgsb_test::findFormantPeaks;
using tgsb_test::FormantPeak;
using tgsb_test::HandleFixture;
using tgsb_test::smoothedEnvelopeAt;
using tgsb_test::synthesizeToPcm;

// Return the frequency of the nearest peak to `expectedHz` in `peaks`.
// Returns 0 if no peak is within `toleranceHz`.
static double nearestPeakHz(const std::vector<FormantPeak>& peaks,
                             double expectedHz, double toleranceHz) {
    double best = 0.0;
    double bestDelta = toleranceHz + 1.0;
    for (const auto& p : peaks) {
        const double d = std::abs(p.freqHz - expectedHz);
        if (d < bestDelta) {
            bestDelta = d;
            best = p.freqHz;
        }
    }
    return bestDelta <= toleranceHz ? best : 0.0;
}

TEST_CASE_FIXTURE(HandleFixture,
                  "spectrum: /a/ at steady-state shows F1 peak near 700-900 Hz") {
    // Spanish /a/ has F1 around 700-900 Hz (somewhat lower than English
    // /ɑ/). Synthesize a simple /aaa/ (extended vowel via repeated key)
    // and look at the spectrum at the middle. The first prominent peak
    // above 150 Hz should be F1.
    auto pcm = synthesizeToPcm(handle, "a", /*speed*/ 1.0);
    REQUIRE(!pcm.empty());

    const int sampleRate = 22050;
    const std::size_t fftLen = 4096;
    const std::size_t center = pcm.size() / 2;

    auto ws = smoothedEnvelopeAt(pcm, center, sampleRate, fftLen, /*smoothHz*/ 150.0);
    const auto peaks = findFormantPeaks(ws.magnitude, sampleRate, ws.fftLength,
                                        /*minHz*/ 200.0, /*maxHz*/ 1500.0,
                                        /*maxPeaks*/ 3);

    REQUIRE_MESSAGE(!peaks.empty(),
                    "no formant peaks found in /a/ spectrum between 200-1500 Hz — "
                    "either the audio is silent, or the DSP produced a spectrally "
                    "empty signal");

    // The single strongest peak in the 200-1500 Hz range should be F1.
    // Spanish /a/ F1 is typically 700-900 Hz.
    const double f1 = peaks[0].freqHz;
    INFO("peaks: " << peaks.size());
    for (const auto& p : peaks) {
        INFO("  " << p.freqHz << " Hz  magnitude=" << p.magnitude);
    }
    CHECK(f1 > 500.0);
    CHECK(f1 < 1100.0);
}

TEST_CASE_FIXTURE(HandleFixture,
                  "spectrum: /i/ F1 is significantly lower than /a/ F1") {
    // Basic vowel-space sanity: /i/ (high front) has low F1, /a/ (low
    // central) has high F1. They MUST differ by hundreds of Hz or the
    // engine isn't producing distinguishable vowels at all.
    auto a_pcm = synthesizeToPcm(handle, "a", 1.0);
    auto i_pcm = synthesizeToPcm(handle, "i", 1.0);
    REQUIRE(!a_pcm.empty());
    REQUIRE(!i_pcm.empty());

    const int sampleRate = 22050;
    const std::size_t fftLen = 4096;

    auto a_ws = smoothedEnvelopeAt(a_pcm, a_pcm.size() / 2, sampleRate, fftLen, 150.0);
    auto i_ws = smoothedEnvelopeAt(i_pcm, i_pcm.size() / 2, sampleRate, fftLen, 150.0);

    auto a_peaks = findFormantPeaks(a_ws.magnitude, sampleRate, a_ws.fftLength,
                                    200.0, 1500.0, 3);
    auto i_peaks = findFormantPeaks(i_ws.magnitude, sampleRate, i_ws.fftLength,
                                    200.0, 1500.0, 3);
    REQUIRE(!a_peaks.empty());
    REQUIRE(!i_peaks.empty());

    const double a_f1 = a_peaks[0].freqHz;
    const double i_f1 = i_peaks[0].freqHz;
    INFO("/a/ F1 = " << a_f1 << "   /i/ F1 = " << i_f1);

    // /a/ F1 should be at least 200 Hz higher than /i/ F1. Typical
    // separation is 400-600 Hz; a 200 Hz floor catches any collapse
    // without being tight enough to false-fire on tuning shifts.
    CHECK(a_f1 - i_f1 > 200.0);
}

TEST_CASE_FIXTURE(HandleFixture,
                  "spectrum: /ɣ/ vs /l/ F1 gap at DSP output matches PhonemeDef prediction") {
    // Analysis parameters tuned from test_spectrum_sweep diagnostics:
    //   - FFT window 512 samples (~23 ms) fits entirely inside the ~30 ms
    //     consonant duration, so flanking /a/ doesn't contaminate.
    //   - Analysis center at 45% of the word (slightly before midpoint):
    //     exact-middle positions land on harmonic-peak alignments that
    //     mis-report formants. 45% is empirically the cleanest position.
    //   - 120 Hz smoothing kernel (rather than 150) because the narrower
    //     window has lower frequency resolution; over-smoothing would
    //     wash out the formant peak.
    //
    // With these settings, measurements match PhonemeDef predictions:
    //   /ɣ/ in /aɣa/:  F1 ≈ 426 Hz  (YAML cf1 = 450)
    //   /l/ in /ala/:  F1 ≈ 298 Hz  (YAML cf1 = 350)
    //   Delta ≈ 128 Hz  — LARGER than the 100 Hz YAML gap predicts.
    //
    // This test pins that the DSP faithfully reproduces the F1
    // distinction. If the engine ever regresses to compressing /ɣ/ and
    // /l/'s F1 together, this fires. Threshold at 60 Hz leaves headroom
    // for legitimate retuning of cf1 defaults while catching collapse.
    //
    // History: initial 2048-window/50%-center measurements showed only
    // 30 Hz separation, which looked like a DSP bug. Sweep diagnostics
    // (test_spectrum_sweep.cpp) revealed that was a window/position
    // artifact: wide windows catch flanking /a/ (F1~700) and exact-middle
    // positions find harmonic peaks instead of formants. The DSP is fine.
    auto g_pcm = synthesizeToPcm(handle, "aɣa", 1.0);
    auto l_pcm = synthesizeToPcm(handle, "ala", 1.0);
    REQUIRE(!g_pcm.empty());
    REQUIRE(!l_pcm.empty());

    const int sampleRate = 22050;
    const std::size_t fftLen = 512;
    const double smoothHz = 120.0;

    const std::size_t g_center = static_cast<std::size_t>(g_pcm.size() * 0.45);
    const std::size_t l_center = static_cast<std::size_t>(l_pcm.size() * 0.45);

    auto g_ws = smoothedEnvelopeAt(g_pcm, g_center, sampleRate, fftLen, smoothHz);
    auto l_ws = smoothedEnvelopeAt(l_pcm, l_center, sampleRate, fftLen, smoothHz);

    auto g_peaks = findFormantPeaks(g_ws.magnitude, sampleRate, g_ws.fftLength,
                                    200.0, 1200.0, 2);
    auto l_peaks = findFormantPeaks(l_ws.magnitude, sampleRate, l_ws.fftLength,
                                    200.0, 1200.0, 2);
    REQUIRE(!g_peaks.empty());
    REQUIRE(!l_peaks.empty());

    const double g_f1 = g_peaks[0].freqHz;
    const double l_f1 = l_peaks[0].freqHz;
    const double delta = std::abs(g_f1 - l_f1);
    INFO("/ɣ/ F1 = " << g_f1 << " Hz   /l/ F1 = " << l_f1
         << " Hz   delta = " << delta << " Hz");
    CHECK(g_f1 > 380.0);   // /ɣ/ should land near 450 Hz
    CHECK(g_f1 < 520.0);
    CHECK(l_f1 < 380.0);   // /l/ should land near 350 Hz or lower
    CHECK(delta > 60.0);   // clear F1 separation
}
