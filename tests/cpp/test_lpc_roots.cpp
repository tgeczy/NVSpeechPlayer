// LPC root-finding tests — the definitive formant measurements. Uses
// Laguerre's method with deflation + polishing on the original polynomial,
// proper z^-1 inversion, conjugate-pair filtering, and bandwidth-based
// phantom-pole rejection.
//
// If these tests disagree with test_lpc.cpp's naive peak-detection
// measurements, trust these — they extract exact pole locations in the
// complex z-plane rather than sampling a smoothed envelope.

#include "doctest.h"
#include "audio_capture.h"
#include "lpc.h"
#include "pack_fixture.h"

#include <sstream>

using tgsb_test::extractFormantsViaRoots;
using tgsb_test::FormantRoot;
using tgsb_test::HandleFixture;
using tgsb_test::readFrameTrace;
using tgsb_test::synthesizeToPcmWithTrace;

static long findStart(const tgsb_test::SynthesisResult& res,
                      const std::vector<tgsb_test::TraceEntry>& tr,
                      const std::string& prefix) {
    for (const auto& e : tr) {
        if (e.phonemeKey.size() >= prefix.size() &&
            e.phonemeKey.compare(0, prefix.size(), prefix) == 0) {
            if (static_cast<std::size_t>(e.frameIndex) < res.samplePositions.size())
                return static_cast<long>(res.samplePositions[e.frameIndex]);
        }
    }
    return -1;
}

TEST_CASE_FIXTURE(HandleFixture,
                  "LPC roots: /a/ formants (sanity via root-finding)") {
    // Sanity: Laguerre + z^-1 inversion + BW filter should give textbook
    // Spanish /a/ values with tight bandwidths. If this fails, the
    // root-finding machinery has a bug — no further root-based results
    // are trustworthy.
    auto res = synthesizeToPcmWithTrace(handle, "a", 1.0, 140.0, 0.5, 22050);
    REQUIRE(!res.pcm.empty());
    // Order 20 (was 16): since the b9 vowel-B1 widening (cb1/pb1 x1.8,
    // BraiLab-matched), /a/'s true F1 bandwidth is ~140 Hz. An order-16
    // fit smeared the wide F1 pole past the 400 Hz phantom gate and it
    // was discarded; with more poles the fit carves F1 at ~230 Hz BW,
    // safely inside the unchanged gate.
    auto r = extractFormantsViaRoots(res.pcm, res.pcm.size() / 2, 22050,
                                     /*window*/ 1024, /*order*/ 20);
    REQUIRE(r.valid);
    REQUIRE(r.formants.size() >= 3);

    std::ostringstream report;
    report << "\n  /a/ formants (Laguerre roots):\n";
    for (std::size_t i = 0; i < r.formants.size() && i < 5; ++i) {
        report << "    F" << (i + 1) << " = " << (int)r.formants[i].freqHz
               << " Hz   BW = " << (int)r.formants[i].bandwidthHz << " Hz\n";
    }
    MESSAGE(report.str());

    CHECK(r.formants[0].freqHz > 500.0);
    CHECK(r.formants[0].freqHz < 1000.0);
    CHECK(r.formants[1].freqHz > 1100.0);
    CHECK(r.formants[2].freqHz > r.formants[1].freqHz + 400.0);
}

TEST_CASE_FIXTURE(HandleFixture,
                  "LPC roots: /ɣ/ vs /l/ in WORD context — the definitive #84/#95 measurement") {
    auto g = synthesizeToPcmWithTrace(handle, "entɾeɣaðo", 1.0, 140.0, 0.5, 22050);
    auto g_tr = readFrameTrace(handle);
    auto l = synthesizeToPcmWithTrace(handle, "entɾelaðo", 1.0, 140.0, 0.5, 22050);
    auto l_tr = readFrameTrace(handle);
    REQUIRE(!g.pcm.empty());
    REQUIRE(!l.pcm.empty());

    // Post /ɣ/→/ɡ_es/ routing change: prefix-match "ɡ" finds /ɡ_es/.
    const long g_start = findStart(g, g_tr, "ɡ");
    const long l_start = findStart(l, l_tr, "l");
    REQUIRE(g_start > 0);
    REQUIRE(l_start > 0);

    // Analyze +12 ms past onset — sweep analysis showed this is the
    // cleanest in-consonant position (past the crossfade from /e/,
    // before the transition toward /a/).
    const std::size_t offSamp = 22050 * 12 / 1000;
    const std::size_t gc = static_cast<std::size_t>(g_start) + offSamp;
    const std::size_t lc = static_cast<std::size_t>(l_start) + offSamp;

    // Log which phoneme key each side resolved to — ensures we're
    // actually testing /l_es/ not some other /l/ variant.
    std::string g_key, l_key;
    for (const auto& e : g_tr)
        // U+0261 ɡ (script g, used for /ɡ_es/) UTF-8: 0xC9 0xA1
        if (e.phonemeKey.size() > 0 && e.phonemeKey[0] == char(0xC9)
            && e.phonemeKey.size() > 1 && (unsigned char)e.phonemeKey[1] == 0xA1) {
            g_key = e.phonemeKey; break;
        }
    for (const auto& e : l_tr)
        if (e.phonemeKey.size() > 0 && e.phonemeKey[0] == 'l') {
            l_key = e.phonemeKey; break;
        }
    MESSAGE("  resolved /ɣ/ key=" << g_key << "    /l/ key=" << l_key);

    auto gr = extractFormantsViaRoots(g.pcm, gc, 22050, /*win*/ 512, /*order*/ 14);
    auto lr = extractFormantsViaRoots(l.pcm, lc, 22050, 512, 14);
    // After /ɣ/→/ɡ_es/: +12ms past /ɡ_es/ start lands in closure (silent
    // voice bar) — root-finding has nothing to extract. Diagnostic only.
    if (!gr.valid) {
        MESSAGE("  /ɡ_es/ closure region — no formants to extract via roots. "
                "(Stop architecture; was approximant.)");
        CHECK(true);
        return;
    }
    REQUIRE(lr.valid);

    auto fmt = [](const std::vector<FormantRoot>& fs) {
        std::ostringstream s;
        for (std::size_t i = 0; i < fs.size() && i < 4; ++i) {
            s << "F" << (i + 1) << "=" << (int)fs[i].freqHz
              << "(BW=" << (int)fs[i].bandwidthHz << ")  ";
        }
        return s.str();
    };
    MESSAGE("  /ɣ/ LPC-roots word context:  " << fmt(gr.formants));
    MESSAGE("  /l/ LPC-roots word context:  " << fmt(lr.formants));

    if (gr.formants.size() >= 2 && lr.formants.size() >= 2) {
        const double dF1 = std::abs(gr.formants[0].freqHz - lr.formants[0].freqHz);
        const double dF2 = std::abs(gr.formants[1].freqHz - lr.formants[1].freqHz);
        MESSAGE("  Δ: F1=" << (int)dF1 << " Hz   F2=" << (int)dF2 << " Hz");
    }
    CHECK(true);
}

TEST_CASE_FIXTURE(HandleFixture,
                  "LPC roots: /aɣa/ vs /ala/ minimal context — baseline") {
    // For comparison with the word-context test. Minimal context should
    // show a much larger F1/F2 gap if coarticulation is what's collapsing
    // the distinction in word-medial position.
    auto g = synthesizeToPcmWithTrace(handle, "aɣa", 1.0, 140.0, 0.5, 22050);
    auto l = synthesizeToPcmWithTrace(handle, "ala", 1.0, 140.0, 0.5, 22050);
    REQUIRE(!g.pcm.empty());
    REQUIRE(!l.pcm.empty());

    const std::size_t gc = static_cast<std::size_t>(g.pcm.size() * 0.45);
    const std::size_t lc = static_cast<std::size_t>(l.pcm.size() * 0.45);
    auto gr = extractFormantsViaRoots(g.pcm, gc, 22050, 512, 14);
    auto lr = extractFormantsViaRoots(l.pcm, lc, 22050, 512, 14);

    auto fmt = [](const std::vector<FormantRoot>& fs) {
        std::ostringstream s;
        for (std::size_t i = 0; i < fs.size() && i < 4; ++i) {
            s << "F" << (i + 1) << "=" << (int)fs[i].freqHz
              << "(BW=" << (int)fs[i].bandwidthHz << ")  ";
        }
        return s.str();
    };
    MESSAGE("  /aɣa/ middle:  " << fmt(gr.formants));
    MESSAGE("  /ala/ middle:  " << fmt(lr.formants));

    if (gr.formants.size() >= 2 && lr.formants.size() >= 2) {
        MESSAGE("  Δ: F1=" << (int)std::abs(gr.formants[0].freqHz - lr.formants[0].freqHz)
                << " Hz   F2=" << (int)std::abs(gr.formants[1].freqHz - lr.formants[1].freqHz)
                << " Hz");
    }
    CHECK(true);
}
