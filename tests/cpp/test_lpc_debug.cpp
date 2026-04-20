// Debug: dump all LPC poles with NO bandwidth filter, to understand
// what's happening to /l/'s spectrum with caN0=1 and the 1700 Hz zero.
//
// If phantom poles appear at ~1700 Hz with wide BW, LPC is doing its
// expected "approximate a zero with poles" dance. If /l/'s F1 at 410
// suddenly has BW > 400 (and thus got filtered), that's why we lost it.

#include "doctest.h"
#include "audio_capture.h"
#include "lpc.h"
#include "pack_fixture.h"

#include <sstream>

using tgsb_test::findLpcRoots;
using tgsb_test::HandleFixture;
using tgsb_test::readFrameTrace;
using tgsb_test::rootsToFormants;
using tgsb_test::synthesizeToPcmWithTrace;

TEST_CASE_FIXTURE(HandleFixture,
                  "LPC debug: all /l/ poles with no BW filter") {
    auto l = synthesizeToPcmWithTrace(handle, "entɾelaðo", 1.0, 140.0, 0.5, 22050);
    auto l_tr = readFrameTrace(handle);
    REQUIRE(!l.pcm.empty());

    long l_start = -1;
    for (const auto& e : l_tr)
        if (e.phonemeKey.size() > 0 && e.phonemeKey[0] == 'l') {
            if (static_cast<std::size_t>(e.frameIndex) < l.samplePositions.size())
                l_start = static_cast<long>(l.samplePositions[e.frameIndex]);
            break;
        }
    REQUIRE(l_start > 0);

    const std::size_t lc = static_cast<std::size_t>(l_start) + 22050 * 12 / 1000;

    auto slice = tgsb_test::pcmSlice(l.pcm, lc - 256, 512);
    tgsb_test::preEmphasize(slice, 0.97);
    tgsb_test::hammingWindow(slice);
    auto r = tgsb_test::autocorrelation(slice, 14);
    auto coeffs = tgsb_test::levinsonDurbin(r, 14);
    auto roots = findLpcRoots(coeffs);

    // Dump ALL poles with no bandwidth filter.
    auto allRoots = rootsToFormants(roots, 22050, /*min*/ 50.0, /*max*/ 10000.0,
                                     /*maxBw*/ 5000.0);
    std::ostringstream report;
    report << "\n  ALL /l/ poles (post-rN0, caN0=1, cfN0=1700):\n";
    for (const auto& f : allRoots) {
        report << "    F = " << (int)f.freqHz
               << " Hz   BW = " << (int)f.bandwidthHz << " Hz\n";
    }
    MESSAGE(report.str());
    CHECK(true);
}
