// Offset sweep inside the /ɣ/ and /l/ in real word context. The earlier
// targeted test measured +8 ms past each phoneme's onset and showed
// /l/ F1 was pulled up to 420 Hz from its minimal-context 298 Hz. If
// that's a window-catches-preceding-/e/ artifact, pushing the window
// later should let /l/'s F1 settle back toward its real value. If F1
// stays at 420 Hz no matter how late we look, the DSP is genuinely
// producing an /l/ with a high F1 in this context — that could be
// the perceptual confusion.

#include "doctest.h"
#include "audio_capture.h"
#include "pack_fixture.h"
#include "spectrum_helpers.h"

#include <sstream>

using tgsb_test::findFormantPeaks;
using tgsb_test::FormantPeak;
using tgsb_test::HandleFixture;
using tgsb_test::smoothedEnvelopeAt;
using tgsb_test::synthesizeToPcmWithTrace;
using tgsb_test::readFrameTrace;

TEST_CASE_FIXTURE(HandleFixture,
                  "offset sweep: /ɣ/ and /l/ formants across ms offsets from onset") {
    auto g_res = synthesizeToPcmWithTrace(handle, "entɾeɣaðo", 1.0, 140.0, 0.5, 22050);
    auto g_trace = readFrameTrace(handle);
    auto l_res = synthesizeToPcmWithTrace(handle, "entɾelaðo", 1.0, 140.0, 0.5, 22050);
    auto l_trace = readFrameTrace(handle);
    REQUIRE(!g_res.pcm.empty());
    REQUIRE(!l_res.pcm.empty());

    // Locate first /ɣ/ and /l/ start samples
    long g_start = -1, l_start = -1;
    for (const auto& e : g_trace)
        if (e.phonemeKey.size() > 0 && e.phonemeKey[0] == char(0xC9) /* ɣ is U+0263 */) {
            // Just match by non-ASCII prefix; cleaner would be utf8 compare
        }
    // Simpler: use std::string::find
    auto findStart = [](const auto& res, const auto& trace, const std::string& prefix) -> long {
        for (const auto& e : trace) {
            if (e.phonemeKey.size() >= prefix.size() &&
                e.phonemeKey.compare(0, prefix.size(), prefix) == 0) {
                if (static_cast<std::size_t>(e.frameIndex) < res.samplePositions.size())
                    return static_cast<long>(res.samplePositions[e.frameIndex]);
            }
        }
        return -1;
    };
    g_start = findStart(g_res, g_trace, "ɣ");
    l_start = findStart(l_res, l_trace, "l");
    REQUIRE(g_start > 0);
    REQUIRE(l_start > 0);

    // Sweep offset from +2 ms to +20 ms in 2 ms steps
    std::ostringstream report;
    report << "\n  offset_ms   /ɣ/ F1  F2   /l/ F1  F2    ΔF1   ΔF2\n";
    for (int offMs = 2; offMs <= 20; offMs += 2) {
        const std::size_t offSamp = static_cast<std::size_t>(22050 * offMs / 1000);
        const std::size_t gc = static_cast<std::size_t>(g_start) + offSamp;
        const std::size_t lc = static_cast<std::size_t>(l_start) + offSamp;

        auto gp = findFormantPeaks(
            smoothedEnvelopeAt(g_res.pcm, gc, 22050, 512, 120.0).magnitude,
            22050, 512, 200.0, 4000.0, 5);
        auto lp = findFormantPeaks(
            smoothedEnvelopeAt(l_res.pcm, lc, 22050, 512, 120.0).magnitude,
            22050, 512, 200.0, 4000.0, 5);

        const double gF1 = gp.size() > 0 ? gp[0].freqHz : 0.0;
        const double gF2 = gp.size() > 1 ? gp[1].freqHz : 0.0;
        const double lF1 = lp.size() > 0 ? lp[0].freqHz : 0.0;
        const double lF2 = lp.size() > 1 ? lp[1].freqHz : 0.0;
        const double dF1 = std::abs(gF1 - lF1);
        const double dF2 = std::abs(gF2 - lF2);

        report << "  " << offMs << " ms\t    "
               << (int)gF1 << "  " << (int)gF2 << "     "
               << (int)lF1 << "  " << (int)lF2 << "     "
               << (int)dF1 << "   " << (int)dF2 << "\n";
    }
    MESSAGE(report.str());
    CHECK(true);
}
