// LPC-based formant measurements — the proper-tool answer to the
// harmonic-interference problem that limited the naive FFT approach.
//
// If LPC still shows /ɣ/ and /l/ with similar F1 in word context, that's
// a real DSP finding (not analysis error). If LPC resolves them cleanly,
// the earlier 4 Hz ΔF1 was indeed a harmonic artifact.

#include "doctest.h"
#include "audio_capture.h"
#include "lpc.h"
#include "pack_fixture.h"

#include <sstream>

using tgsb_test::extractFormantsLPC;
using tgsb_test::HandleFixture;
using tgsb_test::readFrameTrace;
using tgsb_test::synthesizeToPcmWithTrace;

static long findPhonemeStart(const tgsb_test::SynthesisResult& res,
                              const std::vector<tgsb_test::TraceEntry>& tr,
                              const std::string& prefix) {
    for (const auto& e : tr) {
        if (e.phonemeKey.size() >= prefix.size() &&
            e.phonemeKey.compare(0, prefix.size(), prefix) == 0) {
            if (static_cast<std::size_t>(e.frameIndex) < res.samplePositions.size()) {
                return static_cast<long>(res.samplePositions[e.frameIndex]);
            }
        }
    }
    return -1;
}

TEST_CASE_FIXTURE(HandleFixture,
                  "LPC: /a/ formants match Spanish vowel targets") {
    // Sanity check: LPC on a stable vowel should give F1≈750 / F2≈1200 /
    // F3≈2500 for Spanish /a/. If this is wildly off, our LPC machinery
    // is broken and no further LPC test results are trustworthy.
    auto res = synthesizeToPcmWithTrace(handle, "a", 1.0, 140.0, 0.5, 22050);
    REQUIRE(!res.pcm.empty());
    auto f = extractFormantsLPC(res.pcm, res.pcm.size() / 2, 22050,
                                /*window*/ 1024, /*order*/ 16);
    REQUIRE(f.valid);
    REQUIRE(f.freqsHz.size() >= 3);
    MESSAGE("  /a/ LPC formants: F1=" << (int)f.freqsHz[0]
            << "  F2=" << (int)f.freqsHz[1]
            << "  F3=" << (int)f.freqsHz[2]);
    CHECK(f.freqsHz[0] > 500.0);
    CHECK(f.freqsHz[0] < 1100.0);
    CHECK(f.freqsHz[1] > f.freqsHz[0] + 200.0);
    CHECK(f.freqsHz[2] > f.freqsHz[1] + 200.0);
}

TEST_CASE_FIXTURE(HandleFixture,
                  "LPC: /aɣa/ vs /ala/ minimal context") {
    auto g = synthesizeToPcmWithTrace(handle, "aɣa", 1.0, 140.0, 0.5, 22050);
    auto l = synthesizeToPcmWithTrace(handle, "ala", 1.0, 140.0, 0.5, 22050);
    REQUIRE(!g.pcm.empty());
    REQUIRE(!l.pcm.empty());

    const std::size_t gc = static_cast<std::size_t>(g.pcm.size() * 0.45);
    const std::size_t lc = static_cast<std::size_t>(l.pcm.size() * 0.45);
    auto gf = extractFormantsLPC(g.pcm, gc, 22050, 512, 14);
    auto lf = extractFormantsLPC(l.pcm, lc, 22050, 512, 14);
    REQUIRE(gf.valid);
    REQUIRE(lf.valid);
    REQUIRE(gf.freqsHz.size() >= 2);
    REQUIRE(lf.freqsHz.size() >= 2);

    const double gF3 = gf.freqsHz.size() > 2 ? gf.freqsHz[2] : 0.0;
    const double lF3 = lf.freqsHz.size() > 2 ? lf.freqsHz[2] : 0.0;
    MESSAGE("  /ɣ/ LPC: F1=" << (int)gf.freqsHz[0]
            << "  F2=" << (int)gf.freqsHz[1] << "  F3=" << (int)gF3);
    MESSAGE("  /l/ LPC: F1=" << (int)lf.freqsHz[0]
            << "  F2=" << (int)lf.freqsHz[1] << "  F3=" << (int)lF3);
    MESSAGE("  Δ: ΔF1=" << (int)std::abs(gf.freqsHz[0] - lf.freqsHz[0])
            << "  ΔF2=" << (int)std::abs(gf.freqsHz[1] - lf.freqsHz[1]));
    CHECK(true);
}

TEST_CASE_FIXTURE(HandleFixture,
                  "LPC: /ɣ/ vs /l/ in WORD context — offset + window sweep") {
    // Rule out window artifact hypothesis: measure at multiple offsets
    // and window sizes. If F2 stays collapsed (~10-50 Hz delta) across
    // most settings, the DSP is genuinely producing similar F2 for /ɣ/
    // and /l/ in /e_a/ context — the #84/#95 perceptual confusion.
    // If F2 opens up at shorter windows or later offsets, the earlier
    // result was window catching /e/ flanking.
    auto g = synthesizeToPcmWithTrace(handle, "entɾeɣaðo", 1.0, 140.0, 0.5, 22050);
    auto g_tr = readFrameTrace(handle);
    auto l = synthesizeToPcmWithTrace(handle, "entɾelaðo", 1.0, 140.0, 0.5, 22050);
    auto l_tr = readFrameTrace(handle);
    const long g_start = findPhonemeStart(g, g_tr, "ɣ");
    const long l_start = findPhonemeStart(l, l_tr, "l");
    REQUIRE(g_start > 0);
    REQUIRE(l_start > 0);

    std::ostringstream report;
    report << "\n  offset  win   /ɣ/ F1  F2    /l/ F1  F2    ΔF1  ΔF2\n";
    const int offsets[] = {4, 8, 12, 16, 20};
    const int windows[] = {256, 384, 512, 768};
    for (int off : offsets) {
        for (int win : windows) {
            const std::size_t gc = static_cast<std::size_t>(g_start) +
                                   static_cast<std::size_t>(22050 * off / 1000);
            const std::size_t lc = static_cast<std::size_t>(l_start) +
                                   static_cast<std::size_t>(22050 * off / 1000);
            auto gf = extractFormantsLPC(g.pcm, gc, 22050,
                                         static_cast<std::size_t>(win), 14);
            auto lf = extractFormantsLPC(l.pcm, lc, 22050,
                                         static_cast<std::size_t>(win), 14);
            if (gf.freqsHz.size() < 2 || lf.freqsHz.size() < 2) continue;
            const double dF1 = std::abs(gf.freqsHz[0] - lf.freqsHz[0]);
            const double dF2 = std::abs(gf.freqsHz[1] - lf.freqsHz[1]);
            report << "  " << off << "ms  " << win << "  "
                   << (int)gf.freqsHz[0] << "  " << (int)gf.freqsHz[1] << "    "
                   << (int)lf.freqsHz[0] << "  " << (int)lf.freqsHz[1] << "    "
                   << (int)dF1 << "   " << (int)dF2 << "\n";
        }
    }
    MESSAGE(report.str());
    CHECK(true);
}

TEST_CASE_FIXTURE(HandleFixture,
                  "LPC: /ɣ/ and /l/ in REAL WORD context — the #84/#95 question") {
    // The definitive LPC measurement: phoneme-trace target, 512-sample
    // window centered ~8 ms into each consonant. If /ɣ/'s F2 is
    // unambiguously LOW and /l/'s F2 unambiguously HIGH, the place-of-
    // articulation cue IS delivered and the perceptual confusion must
    // come from elsewhere (transitions, duration, etc.). If LPC agrees
    // with our naive measurement (4 Hz ΔF1, ~950 Hz ΔF2), same conclusion.
    auto g = synthesizeToPcmWithTrace(handle, "entɾeɣaðo", 1.0, 140.0, 0.5, 22050);
    auto g_tr = readFrameTrace(handle);
    auto l = synthesizeToPcmWithTrace(handle, "entɾelaðo", 1.0, 140.0, 0.5, 22050);
    auto l_tr = readFrameTrace(handle);
    REQUIRE(!g.pcm.empty());
    REQUIRE(!l.pcm.empty());

    const long g_start = findPhonemeStart(g, g_tr, "ɣ");
    const long l_start = findPhonemeStart(l, l_tr, "l");
    REQUIRE(g_start > 0);
    REQUIRE(l_start > 0);

    // Center the LPC window +8 ms past each consonant's onset.
    const std::size_t off = 22050 * 8 / 1000;
    const std::size_t gc = static_cast<std::size_t>(g_start) + off;
    const std::size_t lc = static_cast<std::size_t>(l_start) + off;

    auto gf = extractFormantsLPC(g.pcm, gc, 22050, 512, 14);
    auto lf = extractFormantsLPC(l.pcm, lc, 22050, 512, 14);
    REQUIRE(gf.valid);
    REQUIRE(lf.valid);
    REQUIRE(gf.freqsHz.size() >= 3);
    REQUIRE(lf.freqsHz.size() >= 3);

    MESSAGE("  WORD /ɣ/ LPC: F1=" << (int)gf.freqsHz[0]
            << "  F2=" << (int)gf.freqsHz[1]
            << "  F3=" << (int)gf.freqsHz[2]
            << "  (start=" << g_start << ")");
    MESSAGE("  WORD /l/ LPC: F1=" << (int)lf.freqsHz[0]
            << "  F2=" << (int)lf.freqsHz[1]
            << "  F3=" << (int)lf.freqsHz[2]
            << "  (start=" << l_start << ")");
    MESSAGE("  WORD Δ: ΔF1=" << (int)std::abs(gf.freqsHz[0] - lf.freqsHz[0])
            << "  ΔF2=" << (int)std::abs(gf.freqsHz[1] - lf.freqsHz[1])
            << "  ΔF3=" << (int)std::abs(gf.freqsHz[2] - lf.freqsHz[2]));
    CHECK(true);
}
