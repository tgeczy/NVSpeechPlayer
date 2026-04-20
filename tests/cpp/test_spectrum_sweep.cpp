// Diagnostic sweep: vary FFT window size AND center position, report /ɣ/
// and /l/ F1 at each combination. If /l/'s "F1" stays at ~433 Hz across
// short windows at different positions, it's a real DSP measurement
// (the engine is genuinely producing F1≈433 Hz for /l/ in /ala/ context).
// If it drops toward 350 Hz at shorter windows or positions closer to
// the consonant's onset, our earlier measurement was a window artifact
// (the 93 ms window was catching flanking /a/ energy).
//
// These use doctest MESSAGE so values always print, not just on failure.
// Run with:
//   tgsb_unit_tests.exe --test-case='sweep*' --no-skip

#include "doctest.h"
#include "audio_capture.h"
#include "pack_fixture.h"
#include "spectrum_helpers.h"

#include <sstream>
#include <vector>

using tgsb_test::findFormantPeaks;
using tgsb_test::HandleFixture;
using tgsb_test::smoothedEnvelopeAt;
using tgsb_test::synthesizeToPcm;

static double firstF1(const std::vector<std::int16_t>& pcm, int sampleRate,
                      std::size_t centerSample, std::size_t fftLen,
                      double smoothHz)
{
    auto ws = smoothedEnvelopeAt(pcm, centerSample, sampleRate, fftLen, smoothHz);
    auto peaks = findFormantPeaks(ws.magnitude, sampleRate, ws.fftLength,
                                  200.0, 1200.0, 2);
    return peaks.empty() ? 0.0 : peaks[0].freqHz;
}

TEST_CASE_FIXTURE(HandleFixture,
                  "sweep: /ɣ/ and /l/ F1 across window sizes (diagnostic)") {
    // Keep center fixed at midpoint. Vary window: 256 / 512 / 1024 / 2048 / 4096
    // samples (12 / 23 / 46 / 93 / 186 ms at 22050 Hz).
    // /ɣ/ and /l/ native durations are ~30 ms — windows <= 512 samples fit
    // entirely inside the consonant (if positioned right); larger windows
    // pick up flanking /a/ energy.
    auto g_pcm = synthesizeToPcm(handle, "aɣa", 1.0);
    auto l_pcm = synthesizeToPcm(handle, "ala", 1.0);
    REQUIRE(!g_pcm.empty());
    REQUIRE(!l_pcm.empty());

    const int sr = 22050;
    const std::size_t gc = g_pcm.size() / 2;
    const std::size_t lc = l_pcm.size() / 2;
    const std::size_t windows[] = {256, 512, 1024, 2048, 4096};

    std::ostringstream report;
    report << "\n  window  /ɣ/ F1   /l/ F1   delta\n";
    for (std::size_t w : windows) {
        // Smooth kernel must be proportionally smaller on narrow windows
        // (otherwise the kernel swallows the whole spectrum).
        const double smoothHz = (w < 1024) ? 120.0 : 150.0;
        const double g_f1 = firstF1(g_pcm, sr, gc, w, smoothHz);
        const double l_f1 = firstF1(l_pcm, sr, lc, w, smoothHz);
        const double d = std::abs(g_f1 - l_f1);
        report << "  " << w << "\t  " << (int)g_f1 << " Hz\t  "
               << (int)l_f1 << " Hz\t  " << (int)d << " Hz\n";
    }
    MESSAGE(report.str());
    CHECK(true);  // diagnostic only, no assertion
}

TEST_CASE_FIXTURE(HandleFixture,
                  "sweep: /ɣ/ and /l/ F1 across analysis positions (diagnostic)") {
    // Fixed window = 512 samples (23 ms — short enough to fit mostly in the
    // 30 ms consonant). Vary the center sample from 30% through 70% of the
    // word duration. Onset vs midpoint vs offset tells us whether F1 is
    // stable (DSP is producing it) or sweeping (coarticulation to flanking
    // vowels is driving the measurement).
    auto g_pcm = synthesizeToPcm(handle, "aɣa", 1.0);
    auto l_pcm = synthesizeToPcm(handle, "ala", 1.0);
    REQUIRE(!g_pcm.empty());
    REQUIRE(!l_pcm.empty());

    const int sr = 22050;
    const std::size_t fftLen = 512;
    const double smoothHz = 120.0;
    const double positions[] = {0.30, 0.40, 0.45, 0.50, 0.55, 0.60, 0.70};

    std::ostringstream report;
    report << "\n  pos %   /ɣ/ F1   /l/ F1   delta    (window=512 samples ≈23ms)\n";
    for (double p : positions) {
        const std::size_t gc = static_cast<std::size_t>(g_pcm.size() * p);
        const std::size_t lc = static_cast<std::size_t>(l_pcm.size() * p);
        const double g_f1 = firstF1(g_pcm, sr, gc, fftLen, smoothHz);
        const double l_f1 = firstF1(l_pcm, sr, lc, fftLen, smoothHz);
        const double d = std::abs(g_f1 - l_f1);
        report << "  " << (int)(p * 100) << "%\t  " << (int)g_f1 << " Hz\t  "
               << (int)l_f1 << " Hz\t  " << (int)d << " Hz\n";
    }
    MESSAGE(report.str());
    CHECK(true);
}
