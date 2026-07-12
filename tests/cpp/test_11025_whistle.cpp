// Issue #104: 11025 Hz near-Nyquist whistle regression test.
//
// gregodejesus2 reported (#104, 2026-07-12): 11025 Hz on Android produces a
// steady whistling/hissing sound. Root cause: the b6 Nyquist clamp
// (kResonatorNyquistClampRatio = 0.475) parked pf6 at ~5237 Hz = 0.95 of
// Nyquist, where the bilinear warp makes a single parallel pole ring as a
// narrow ~+25 dB peak riding all frication. Fixed by relocating pf6 to
// kParallelPf6NyquistSpreadRatio (0.435*sr ≈ 4800 Hz) — see dspCommon.h.
//
// This test renders vowel-heavy words at 11025 and asserts the near-Nyquist
// band contains no narrow tonal spike. Vowel-heavy material is deliberate:
// the whistle is most exposed against clean voiced harmonics, while
// sibilant-heavy words partially mask it (broadband noise hides a tone) —
// per Tomi's ear finding during the 2026-07-12 investigation.
//
// Pre-fix prominence on these words: +21 to +29 dB. Post-fix: +8 to +12 dB.
// Healthy 22050 renders measure +4 to +7 dB. Threshold 15 dB splits the
// populations with margin on both sides.

#include "doctest.h"
#include "audio_capture.h"
#include "pack_fixture.h"
#include "spectrum_helpers.h"

#include <algorithm>
#include <cmath>
#include <vector>

using tgsb_test::HandleFixture;
using tgsb_test::synthesizeToPcmWithTrace;
using tgsb_test::computeMagnitudeSpectrum;

namespace {

// Hop-averaged power spectrum in dB (Welch-style), mirroring the Python
// probe used to characterize the bug (tools history: whistle_probe.py).
std::vector<double> averagedSpectrumDb(const std::vector<std::int16_t>& pcm,
                                       std::size_t window, std::size_t hop) {
    std::vector<double> acc;
    std::size_t frames = 0;
    for (std::size_t start = 0; start + window <= pcm.size(); start += hop) {
        const auto mag = computeMagnitudeSpectrum(pcm, start, window);
        if (acc.empty()) acc.assign(mag.size(), 0.0);
        for (std::size_t i = 0; i < mag.size(); ++i) acc[i] += mag[i] * mag[i];
        ++frames;
    }
    for (auto& v : acc) {
        v = 10.0 * std::log10(v / static_cast<double>(std::max<std::size_t>(frames, 1)) + 1e-30);
    }
    return acc;
}

double medianOf(std::vector<double> v) {
    if (v.empty()) return 0.0;
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

// Peak dB in [peakLo, peakHi] minus median dB of the wider [bandLo, peakHi]
// reference band — the "narrow tonal spike" metric from the investigation.
double nearNyquistProminenceDb(const std::vector<std::int16_t>& pcm,
                               int sampleRate,
                               double bandLo, double peakLo, double peakHi) {
    constexpr std::size_t kWindow = 2048;
    constexpr std::size_t kHop = 512;
    const auto spec = averagedSpectrumDb(pcm, kWindow, kHop);
    const double binHz = static_cast<double>(sampleRate) / kWindow;

    double peak = -1e30;
    std::vector<double> band;
    for (std::size_t i = 0; i < spec.size(); ++i) {
        const double f = binHz * static_cast<double>(i);
        if (f >= bandLo && f <= peakHi) band.push_back(spec[i]);
        if (f >= peakLo && f <= peakHi) peak = std::max(peak, spec[i]);
    }
    return peak - medianOf(band);
}

} // namespace

TEST_CASE_FIXTURE(HandleFixture,
                  "11025 Hz: no near-Nyquist whistle in frication (#104)") {
    struct Word { const char* name; const char* ipa; };
    // IPA from `espeak-ng -v es-419 -q --ipa <word>` — the reporter's
    // language and the words used to pin the bug down.
    const Word words[] = {
        {"espanol",    "espaɲˈol"},
        {"suspendido", "suspendˈiðo"},
    };

    constexpr int kSr = 11025;
    // Prominence measured over the top of the 11025 band: reference band
    // 4600 Hz up to Nyquist, spike search window 5300-5510 Hz.
    constexpr double kBandLo = 4600.0, kPeakLo = 5300.0, kPeakHi = 5510.0;
    constexpr double kMaxProminenceDb = 15.0;

    for (const auto& w : words) {
        const std::string name(w.name);
        auto res = synthesizeToPcmWithTrace(handle, w.ipa, 1.0, 140.0, 0.5, kSr);
        REQUIRE_MESSAGE(!res.pcm.empty(), name << ": synthesis produced no PCM");

        const double prom = nearNyquistProminenceDb(res.pcm, kSr,
                                                    kBandLo, kPeakLo, kPeakHi);
        CHECK_MESSAGE(prom < kMaxProminenceDb,
                      name << " @ 11025 Hz: near-Nyquist prominence "
                           << prom << " dB >= " << kMaxProminenceDb
                           << " dB — the #104 whistle is back");
        MESSAGE(name << " @ 11025 Hz: near-Nyquist prominence "
                     << prom << " dB (threshold " << kMaxProminenceDb << ")");
    }
}
