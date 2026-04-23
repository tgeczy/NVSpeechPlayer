// Deeper acoustic hunt: we've ruled out F1 collapse. Now check F2, F3,
// amplitude envelopes, and the real word context /entɾeɣaðo/ vs
// /entɾelaðo/ — looking for which acoustic dimension ACTUALLY compresses
// /ɣ/ toward /l/ at the DSP output, if any.
//
// Uses the analysis parameters that the sweep diagnostics established as
// clean: 512-sample window, 45% position, 120 Hz smoothing kernel.

#include "doctest.h"
#include "audio_capture.h"
#include "pack_fixture.h"
#include "spectrum_helpers.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using tgsb_test::findFormantPeaks;
using tgsb_test::FormantPeak;
using tgsb_test::HandleFixture;
using tgsb_test::smoothedEnvelopeAt;
using tgsb_test::synthesizeToPcm;

// Return F1, F2, F3 at the 45% position of pcm using the clean-analysis
// settings. Returns zeros for any formant not found.
static void formantsAt45(const std::vector<std::int16_t>& pcm,
                         int sampleRate,
                         double& f1, double& f2, double& f3) {
    const std::size_t center = static_cast<std::size_t>(pcm.size() * 0.45);
    auto ws = smoothedEnvelopeAt(pcm, center, sampleRate, /*fft*/ 512, /*smooth*/ 120.0);
    auto peaks = findFormantPeaks(ws.magnitude, sampleRate, ws.fftLength,
                                  /*min*/ 200.0, /*max*/ 4000.0, /*maxPeaks*/ 5);
    f1 = peaks.size() > 0 ? peaks[0].freqHz : 0.0;
    f2 = peaks.size() > 1 ? peaks[1].freqHz : 0.0;
    f3 = peaks.size() > 2 ? peaks[2].freqHz : 0.0;
}

// RMS of a short window around the given sample. Used as a cheap
// amplitude proxy — /ɣ/ (approximant, quieter) vs /l/ (lateral, fully
// voiced) should have measurably different RMS in context.
static double rmsAround(const std::vector<std::int16_t>& pcm,
                        std::size_t center, std::size_t halfWindow) {
    const std::size_t lo = (center > halfWindow) ? center - halfWindow : 0;
    const std::size_t hi = std::min(pcm.size(), center + halfWindow);
    if (hi <= lo) return 0.0;
    double sumSq = 0.0;
    for (std::size_t i = lo; i < hi; ++i) {
        const double x = static_cast<double>(pcm[i]) / 32768.0;
        sumSq += x * x;
    }
    return std::sqrt(sumSq / static_cast<double>(hi - lo));
}

TEST_CASE_FIXTURE(HandleFixture,
                  "hunt: /ɣ/ vs /l/ F2 in /aXa/ minimal context") {
    auto g_pcm = synthesizeToPcm(handle, "aɣa", 1.0);
    auto l_pcm = synthesizeToPcm(handle, "ala", 1.0);
    REQUIRE(!g_pcm.empty());
    REQUIRE(!l_pcm.empty());

    double gf1, gf2, gf3, lf1, lf2, lf3;
    formantsAt45(g_pcm, 22050, gf1, gf2, gf3);
    formantsAt45(l_pcm, 22050, lf1, lf2, lf3);
    MESSAGE("  /ɣ/  F1=" << (int)gf1 << "  F2=" << (int)gf2 << "  F3=" << (int)gf3);
    MESSAGE("  /l/  F1=" << (int)lf1 << "  F2=" << (int)lf2 << "  F3=" << (int)lf3);
    MESSAGE("  Δ    ΔF1=" << (int)std::abs(gf1 - lf1)
            << "  ΔF2=" << (int)std::abs(gf2 - lf2)
            << "  ΔF3=" << (int)std::abs(gf3 - lf3));
    // No assertions — diagnostic. Failing assertions would come in a
    // follow-up once the expected deltas are understood.
    CHECK(true);
}

TEST_CASE_FIXTURE(HandleFixture,
                  "hunt: /ɣ/ vs /l/ F2/F3 in REAL WORD context (entregado vs entrelado)") {
    auto g_pcm = synthesizeToPcm(handle, "entɾeɣaðo", 1.0);
    auto l_pcm = synthesizeToPcm(handle, "entɾelaðo", 1.0);
    REQUIRE(!g_pcm.empty());
    REQUIRE(!l_pcm.empty());

    // In these longer words, 45% of the total length is NOT on the target
    // consonant — it's on the /ɣ/ or /l/ vowel-preceding transition.
    // The target consonant position (middle of /ɾeɣa/ or /ɾela/) sits
    // around 55% of the word. Use that.
    const std::size_t g_center = static_cast<std::size_t>(g_pcm.size() * 0.55);
    const std::size_t l_center = static_cast<std::size_t>(l_pcm.size() * 0.55);

    auto g_ws = smoothedEnvelopeAt(g_pcm, g_center, 22050, 512, 120.0);
    auto l_ws = smoothedEnvelopeAt(l_pcm, l_center, 22050, 512, 120.0);
    auto g_peaks = findFormantPeaks(g_ws.magnitude, 22050, g_ws.fftLength,
                                    200.0, 4000.0, 5);
    auto l_peaks = findFormantPeaks(l_ws.magnitude, 22050, l_ws.fftLength,
                                    200.0, 4000.0, 5);
    REQUIRE(!g_peaks.empty());
    REQUIRE(!l_peaks.empty());

    auto pick = [](const std::vector<FormantPeak>& p, std::size_t i) {
        return p.size() > i ? p[i].freqHz : 0.0;
    };
    MESSAGE("  word /entɾeɣaðo/  F1=" << (int)pick(g_peaks, 0)
            << "  F2=" << (int)pick(g_peaks, 1)
            << "  F3=" << (int)pick(g_peaks, 2));
    MESSAGE("  word /entɾelaðo/  F1=" << (int)pick(l_peaks, 0)
            << "  F2=" << (int)pick(l_peaks, 1)
            << "  F3=" << (int)pick(l_peaks, 2));
    CHECK(true);
}

TEST_CASE_FIXTURE(HandleFixture,
                  "hunt: /ɣ/ vs /l/ RMS amplitude around consonant center") {
    // /ɣ/ (velar approximant) is quieter than /l/ (fully voiced lateral)
    // by YAML: ɣ_es voiceAmplitude=0.82, l_es=0.9. In principle that's an
    // ~0.8 dB difference — perceptible to careful listeners but subtle.
    // If the DSP flattens that difference, we lose a sonority cue.
    auto g_pcm = synthesizeToPcm(handle, "aɣa", 1.0);
    auto l_pcm = synthesizeToPcm(handle, "ala", 1.0);
    REQUIRE(!g_pcm.empty());
    REQUIRE(!l_pcm.empty());

    // Small window (~15 ms) centered on 45% to sit inside the consonant.
    const std::size_t halfWin = static_cast<std::size_t>(0.015 * 22050 / 2);
    const double g_rms = rmsAround(g_pcm, (std::size_t)(g_pcm.size() * 0.45), halfWin);
    const double l_rms = rmsAround(l_pcm, (std::size_t)(l_pcm.size() * 0.45), halfWin);
    const double db_delta = 20.0 * std::log10((l_rms > 0 ? l_rms : 1e-9) /
                                               (g_rms > 0 ? g_rms : 1e-9));
    MESSAGE("  /ɣ/ RMS = " << g_rms);
    MESSAGE("  /l/ RMS = " << l_rms);
    MESSAGE("  Δ = " << db_delta << " dB  (positive means /l/ louder than /ɣ/)");
    CHECK(true);
}

// Helper: synthesize and look up the sample offset where the first
// phoneme whose key starts with `prefix` begins. Returns -1 if not found.
static long phonemeStartSample(const tgsb_test::SynthesisResult& res,
                                const std::vector<tgsb_test::TraceEntry>& trace,
                                const std::string& prefix)
{
    for (const auto& e : trace) {
        if (e.phonemeKey.size() >= prefix.size() &&
            e.phonemeKey.compare(0, prefix.size(), prefix) == 0) {
            if (e.frameIndex >= 0 &&
                static_cast<std::size_t>(e.frameIndex) < res.samplePositions.size()) {
                return static_cast<long>(res.samplePositions[e.frameIndex]);
            }
        }
    }
    return -1;
}

TEST_CASE_FIXTURE(HandleFixture,
                  "hunt: TARGETED — /ɣ/ and /l/ formants in entregado/entrelado via frame_trace") {
    // The earlier "hunt: /ɣ/ vs /l/ F2/F3 in REAL WORD context" test used
    // a heuristic 55% position for the consonant center. This one uses the
    // frame_trace API to land exactly on the target phoneme's first frame.
    // Analysis window 512 samples = 23 ms starts slightly before the first
    // consonant frame (we offset back half a window and forward a bit to
    // hit the consonant's steady-state region rather than its onset).
    auto g_res = tgsb_test::synthesizeToPcmWithTrace(handle, "entɾeɣaðo",
                                                     1.0, 140.0, 0.5, 22050);
    auto l_res = tgsb_test::synthesizeToPcmWithTrace(handle, "entɾelaðo",
                                                     1.0, 140.0, 0.5, 22050);
    REQUIRE(!g_res.pcm.empty());
    REQUIRE(!l_res.pcm.empty());

    auto g_trace = tgsb_test::readFrameTrace(handle);  // tied to LAST synth call
    // Re-sync: last synth was the /l/ one, so re-trace after re-synthesizing
    // /ɣ/. Simpler: synthesize in the order we trace.
    auto g_res2 = tgsb_test::synthesizeToPcmWithTrace(handle, "entɾeɣaðo",
                                                      1.0, 140.0, 0.5, 22050);
    auto g_trace2 = tgsb_test::readFrameTrace(handle);
    auto l_res2 = tgsb_test::synthesizeToPcmWithTrace(handle, "entɾelaðo",
                                                      1.0, 140.0, 0.5, 22050);
    auto l_trace2 = tgsb_test::readFrameTrace(handle);

    // After the /ɣ/→/ɡ_es/ routing change, the velar phoneme in /entɾeɣaðo/
    // is now /ɡ_es/ (a stop). Prefix-match "ɡ" finds /ɡ_es/ in the trace.
    const long g_start = phonemeStartSample(g_res2, g_trace2, "ɡ");
    const long l_start = phonemeStartSample(l_res2, l_trace2, "l");

    REQUIRE_MESSAGE(g_start > 0, "could not locate /ɡ_es/ phoneme in /entɾeɣaðo/");
    REQUIRE_MESSAGE(l_start > 0, "could not locate /l/ phoneme in /entɾelaðo/");

    // Offset forward by ~8 ms (half the typical 15 ms consonant duration)
    // to land on steady-state rather than the onset transition.
    const std::size_t offsetSamples = 22050 * 8 / 1000;
    const std::size_t g_center = static_cast<std::size_t>(g_start) + offsetSamples;
    const std::size_t l_center = static_cast<std::size_t>(l_start) + offsetSamples;

    auto g_ws = smoothedEnvelopeAt(g_res2.pcm, g_center, 22050, 512, 120.0);
    auto l_ws = smoothedEnvelopeAt(l_res2.pcm, l_center, 22050, 512, 120.0);
    auto g_peaks = findFormantPeaks(g_ws.magnitude, 22050, g_ws.fftLength,
                                    200.0, 4000.0, 5);
    auto l_peaks = findFormantPeaks(l_ws.magnitude, 22050, l_ws.fftLength,
                                    200.0, 4000.0, 5);
    REQUIRE(!g_peaks.empty());
    REQUIRE(!l_peaks.empty());

    auto get = [](const std::vector<FormantPeak>& p, std::size_t i) {
        return p.size() > i ? p[i].freqHz : 0.0;
    };
    MESSAGE("  TARGETED /ɣ/ in entɾeɣaðo  start=" << g_start << "  "
            "F1=" << (int)get(g_peaks, 0) << "  F2=" << (int)get(g_peaks, 1)
            << "  F3=" << (int)get(g_peaks, 2));
    MESSAGE("  TARGETED /l/ in entɾelaðo  start=" << l_start << "  "
            "F1=" << (int)get(l_peaks, 0) << "  F2=" << (int)get(l_peaks, 1)
            << "  F3=" << (int)get(l_peaks, 2));
    const double g_f1 = get(g_peaks, 0);
    const double l_f1 = get(l_peaks, 0);
    MESSAGE("  ΔF1 in word context = " << (int)std::abs(g_f1 - l_f1)
            << " Hz  (minimal context was 127 Hz)");
    CHECK(true);
}

TEST_CASE_FIXTURE(HandleFixture,
                  "hunt: overall RMS of entregado vs entrelado") {
    // Gross sanity: are the two full words producing comparable energy?
    // If /entɾeɣaðo/ overall is much quieter than /entɾelaðo/, users
    // might hear the /ɣ/ "vanishing" even if its F1/F2 are distinct.
    auto g_pcm = synthesizeToPcm(handle, "entɾeɣaðo", 1.0);
    auto l_pcm = synthesizeToPcm(handle, "entɾelaðo", 1.0);
    REQUIRE(!g_pcm.empty());
    REQUIRE(!l_pcm.empty());

    double g_sumSq = 0.0, l_sumSq = 0.0;
    for (auto s : g_pcm) { const double x = s / 32768.0; g_sumSq += x * x; }
    for (auto s : l_pcm) { const double x = s / 32768.0; l_sumSq += x * x; }
    const double g_rms = std::sqrt(g_sumSq / g_pcm.size());
    const double l_rms = std::sqrt(l_sumSq / l_pcm.size());
    MESSAGE("  entɾeɣaðo full-word RMS = " << g_rms);
    MESSAGE("  entɾelaðo full-word RMS = " << l_rms);
    CHECK(true);
}
