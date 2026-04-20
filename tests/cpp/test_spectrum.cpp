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
                  "spectrum: DIAGNOSTIC — /aɣa/ vs /ala/ F1 peak separation at DSP output") {
    // The Token-level /ɣ/-vs-/l/ cf1 gap is ~100 Hz (450 Hz vs 350 Hz per
    // PhonemeDef). This test measures what survives through the DSP.
    //
    // CURRENT MEASUREMENT: ~30 Hz separation (/ɣ/≈463 Hz, /l/≈433 Hz)
    // at 2048-sample FFT window, 150 Hz smoothing kernel.
    //
    // Interpretation requires care. Three possible explanations:
    //   1. DSP crossfades genuinely compress the F1 distinction in word
    //      context — the #84/#95 word-context "sounds like /l/" bug.
    //   2. The analysis window (~93 ms at 2048/22050) is too wide and
    //      catches significant energy from the flanking /a/ frames
    //      (F1~730 Hz), pulling both measurements upward and
    //      compressing their gap. /ɣ/ is only ~30 ms at speed 1.0.
    //   3. Naive FFT + smoothing peak-find just isn't precise enough
    //      for short consonants — LPC would be more accurate.
    //
    // Assertion threshold is set at 20 Hz — half the currently-measured
    // value — to catch regressions where this already-narrow gap
    // collapses further, while being lenient enough to remain stable
    // against normal DSP retuning. Until we disambiguate (2) and (3)
    // via LPC or short-window analysis, we can't tighten this to
    // claim there IS a DSP bug with confidence.
    auto g_pcm = synthesizeToPcm(handle, "aɣa", 1.0);
    auto l_pcm = synthesizeToPcm(handle, "ala", 1.0);
    REQUIRE(!g_pcm.empty());
    REQUIRE(!l_pcm.empty());

    const int sampleRate = 22050;
    const std::size_t fftLen = 2048;

    auto g_ws = smoothedEnvelopeAt(g_pcm, g_pcm.size() / 2, sampleRate, fftLen, 150.0);
    auto l_ws = smoothedEnvelopeAt(l_pcm, l_pcm.size() / 2, sampleRate, fftLen, 150.0);

    auto g_peaks = findFormantPeaks(g_ws.magnitude, sampleRate, g_ws.fftLength,
                                    200.0, 1200.0, 2);
    auto l_peaks = findFormantPeaks(l_ws.magnitude, sampleRate, l_ws.fftLength,
                                    200.0, 1200.0, 2);
    REQUIRE(!g_peaks.empty());
    REQUIRE(!l_peaks.empty());

    const double g_f1 = g_peaks[0].freqHz;
    const double l_f1 = l_peaks[0].freqHz;
    const double delta = std::abs(g_f1 - l_f1);
    INFO("/ɣ/ middle F1 = " << g_f1 << "   /l/ middle F1 = " << l_f1
         << "   delta = " << delta << " Hz");
    INFO("Token-level cf1 says ɣ=450 / l=350 — so DSP+analysis is reporting "
         << "narrower gap than expected.");
    CHECK(delta > 20.0);
}
