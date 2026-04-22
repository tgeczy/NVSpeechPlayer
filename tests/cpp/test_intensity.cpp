// Consonant-intensity regression and diagnostic tests.
//
// These tests measure HOW LOUD a consonant is relative to flanking vowels,
// filling the perceptual-amplitude axis that LPC frequency tests don't
// cover. They are essential for /ɣ/ (which can be at the correct F2 but
// still inaudible) and for any future lenited approximant work.
//
// Targets from acoustic phonetics literature:
//   Jongman 2000: nonsibilant fricatives sit at -17 to -18 dB below vowel
//   Kingston 2008: Spanish [ɣ̞] lenition cue is IntDiff — quieter than
//     flanking vowels, but audibly continuant (not silent)
//
// Also writes WAV snapshots to the current working directory so a human
// (especially a non-Spanish-speaking engineer) can A/B listen at each
// config change and confirm "consonant audible" or "consonant missing."

#include "doctest.h"
#include "audio_capture.h"
#include "intensity.h"
#include "wav_export.h"
#include "pack_fixture.h"

#include <sstream>

using tgsb_test::HandleFixture;
using tgsb_test::synthesizeToPcmWithTrace;
using tgsb_test::readFrameTrace;
using tgsb_test::rmsAmplitude;
using tgsb_test::dbRatio;
using tgsb_test::writeWav;

// Find the frame index of the first trace entry whose phoneme key matches.
static int findPhonemeFrameIdx(
    const std::vector<tgsb_test::TraceEntry>& tr,
    const std::string& prefix)
{
    for (const auto& e : tr) {
        if (e.phonemeKey.size() >= prefix.size() &&
            e.phonemeKey.compare(0, prefix.size(), prefix) == 0) {
            return e.frameIndex;
        }
    }
    return -1;
}

// Find the sample index of the LAST trace entry matching `prefix` that
// sits BEFORE frame index `beforeIdx`. Useful to find the /e/ immediately
// preceding a /ɣ/ (not the first /e/ in the word).
static long findPhonemeSampleBefore(
    const tgsb_test::SynthesisResult& res,
    const std::vector<tgsb_test::TraceEntry>& tr,
    const std::string& prefix,
    int beforeIdx)
{
    long lastMatch = -1;
    for (const auto& e : tr) {
        if (e.frameIndex >= beforeIdx) break;
        if (e.phonemeKey.size() >= prefix.size() &&
            e.phonemeKey.compare(0, prefix.size(), prefix) == 0) {
            if (static_cast<std::size_t>(e.frameIndex) < res.samplePositions.size()) {
                lastMatch = static_cast<long>(res.samplePositions[e.frameIndex]);
            }
        }
    }
    return lastMatch;
}

// Find the sample index of the FIRST trace entry matching `prefix` that
// sits AFTER frame index `afterIdx`.
static long findPhonemeSampleAfter(
    const tgsb_test::SynthesisResult& res,
    const std::vector<tgsb_test::TraceEntry>& tr,
    const std::string& prefix,
    int afterIdx)
{
    for (const auto& e : tr) {
        if (e.frameIndex <= afterIdx) continue;
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
                  "Intensity: /ɣ/ RMS vs flanking vowels in /entɾeɣaðo/") {
    // The primary perceptual question from testers: is /ɣ/ audible relative
    // to flanking /e/ and /a/? At 20 ms into each phoneme, measure RMS over
    // a 40 ms window (captures the core steady-state without transition
    // contamination), and report the dB ratio.
    auto res = synthesizeToPcmWithTrace(handle, "entɾeɣaðo", 1.0, 140.0, 0.5, 22050);
    auto tr = readFrameTrace(handle);
    REQUIRE(!res.pcm.empty());
    REQUIRE(!tr.empty());

    const int gIdx = findPhonemeFrameIdx(tr, "ɣ");
    REQUIRE(gIdx >= 0);
    const long g_start = (gIdx < static_cast<int>(res.samplePositions.size()))
        ? static_cast<long>(res.samplePositions[gIdx]) : -1;
    const long e_start = findPhonemeSampleBefore(res, tr, "e", gIdx);
    const long a_start = findPhonemeSampleAfter(res, tr, "a", gIdx);
    REQUIRE(g_start > 0);
    REQUIRE(e_start > 0);
    REQUIRE(a_start > 0);

    // Duration-aware window: use phoneme boundaries from the trace.
    // Consonant window = middle 50% of /ɣ/'s duration (avoids transition
    // contamination from flanking vowels). Vowel windows = 20 ms starting
    // 10 ms into each vowel (conservative, stays well inside /e/ and /a/).
    const std::size_t gDur = static_cast<std::size_t>(a_start - g_start);
    const std::size_t gWinStart = static_cast<std::size_t>(g_start) + gDur / 4;
    const std::size_t gWinLen = gDur / 2;

    const std::size_t vOff = 22050 * 10 / 1000;
    const std::size_t vWin = 22050 * 20 / 1000;

    const double gRms = rmsAmplitude(res.pcm, gWinStart, gWinLen);
    const double eRms = rmsAmplitude(res.pcm, e_start + vOff, vWin);
    const double aRms = rmsAmplitude(res.pcm, a_start + vOff, vWin);
    const double avgVowelRms = (eRms + aRms) * 0.5;

    const double gDbVsE = dbRatio(gRms, eRms);
    const double gDbVsA = dbRatio(gRms, aRms);
    const double gDbVsAvg = dbRatio(gRms, avgVowelRms);

    const double gDurMs = static_cast<double>(gDur) * 1000.0 / 22050.0;
    const double gWinLenMs = static_cast<double>(gWinLen) * 1000.0 / 22050.0;

    MESSAGE("  ----- /ɣ/ INTENSITY REPORT (entɾeɣaðo) -----");
    MESSAGE("  /ɣ/ duration: " << gDurMs << " ms,  measurement window: "
            << gWinLenMs << " ms (middle 50%)");
    MESSAGE("  raw RMS (fraction of full-scale):");
    MESSAGE("    /e/ preceding: " << eRms);
    MESSAGE("    /ɣ/ core:      " << gRms);
    MESSAGE("    /a/ following: " << aRms);
    MESSAGE("  /ɣ/ dB relative to flanking vowels:");
    MESSAGE("    vs /e/:  " << gDbVsE << " dB");
    MESSAGE("    vs /a/:  " << gDbVsA << " dB");
    MESSAGE("    vs avg:  " << gDbVsAvg << " dB");
    MESSAGE("  Jongman 2000 nonsibilant-fricative target: -17 to -18 dB vs vowel");
    MESSAGE("  'Inaudible' threshold per tester reports: below -25 to -30 dB");

    // Loose sanity assertions only. Exact Jongman-alignment target is
    // established in a separate test after baseline measurement.
    CHECK(gDbVsAvg < 0.0);         // quieter than vowel (lenition happens)
    CHECK(gDbVsAvg > -60.0);       // not literally silent

    writeWav("test_output_entregado.wav", res.pcm, 22050);
    MESSAGE("  WAV written: test_output_entregado.wav");

    // Diagnostic comparison: what if /ɣ/ were the hard /ɡ/ stop instead?
    // Renders "entregado" with medial /ɡ/ (no lenition) so we can A/B
    // the approximant vs stop character in identical word context.
    auto hardG = synthesizeToPcmWithTrace(handle, "entɾeɡaðo", 1.0, 140.0, 0.5, 22050);
    if (!hardG.pcm.empty()) {
        writeWav("test_output_entregado_hardG.wav", hardG.pcm, 22050);
        MESSAGE("  WAV written: test_output_entregado_hardG.wav (diagnostic with /ɡ/ stop)");
    }
    // Voiceless stop intervocalic samples — check whether a reduced
    // stopClosureVowelGapMs breaks /p/, /t/, /k/ word perception.
    auto papa = synthesizeToPcmWithTrace(handle, "papa", 1.0, 140.0, 0.5, 22050);
    if (!papa.pcm.empty()) writeWav("test_output_papa.wav", papa.pcm, 22050);
    auto tapa = synthesizeToPcmWithTrace(handle, "tapa", 1.0, 140.0, 0.5, 22050);
    if (!tapa.pcm.empty()) writeWav("test_output_tapa.wav", tapa.pcm, 22050);
}

TEST_CASE_FIXTURE(HandleFixture,
                  "Intensity: /l/ RMS in /entɾelaðo/ — known-working baseline") {
    // Reference measurement: /l/ is reported audible and correct by testers
    // in b1/b2/b201 alike. Its RMS should be comparable to vowels (laterals
    // are sonorants — quieter than vowels by some margin, but much closer
    // to them than to fricatives). Catches any /l/ amplitude regression.
    auto res = synthesizeToPcmWithTrace(handle, "entɾelaðo", 1.0, 140.0, 0.5, 22050);
    auto tr = readFrameTrace(handle);
    REQUIRE(!res.pcm.empty());
    REQUIRE(!tr.empty());

    const int lIdx = findPhonemeFrameIdx(tr, "l");
    REQUIRE(lIdx >= 0);
    const long l_start = (lIdx < static_cast<int>(res.samplePositions.size()))
        ? static_cast<long>(res.samplePositions[lIdx]) : -1;
    const long e_start = findPhonemeSampleBefore(res, tr, "e", lIdx);
    const long a_start = findPhonemeSampleAfter(res, tr, "a", lIdx);
    REQUIRE(l_start > 0);
    REQUIRE(e_start > 0);
    REQUIRE(a_start > 0);

    const std::size_t off = 22050 * 20 / 1000;
    const std::size_t win = 22050 * 40 / 1000;
    const double lRms = rmsAmplitude(res.pcm, l_start + off, win);
    const double eRms = rmsAmplitude(res.pcm, e_start + off, win);
    const double aRms = rmsAmplitude(res.pcm, a_start + off, win);
    const double avgVowelRms = (eRms + aRms) * 0.5;
    const double lDbVsAvg = dbRatio(lRms, avgVowelRms);

    MESSAGE("  ----- /l/ INTENSITY REPORT (entɾelaðo) -----");
    MESSAGE("  raw RMS: /e/=" << eRms << "  /l/=" << lRms << "  /a/=" << aRms);
    MESSAGE("  /l/ dB vs avg flanking vowel: " << lDbVsAvg << " dB");

    // /l/ as sonorant should sit much closer to vowel amplitude than a
    // fricative would. Anything below -15 dB suggests amplitude issue.
    CHECK(lDbVsAvg > -20.0);   // /l/ not too quiet
    CHECK(lDbVsAvg < 3.0);     // /l/ not louder than vowel (sanity)

    writeWav("test_output_entrelado.wav", res.pcm, 22050);
    MESSAGE("  WAV written: test_output_entrelado.wav");
}

TEST_CASE_FIXTURE(HandleFixture,
                  "Intensity: /g/ word-initial stop in /gusano/ — 29-Bloo's diagnostic") {
    // @29-Bloo on #95 asked us to measure word-initial /g/ (velar stop [g]
    // in Spanish, not approximant). This gives us a reference point: our
    // velar place articulation quality in the STOP environment. If /g/ is
    // crisp and /ɣ/ inaudible, the problem is /ɣ_es/-specific. If /g/ is
    // also weak, the problem is broader velar-place modeling in the engine.
    //
    // Word: gusano (/ɡuˈsano/). Word-initial /g/ is normally realized as
    // stop in Spanish after pause. Use wider measurement window to catch
    // the burst plus onset transition.
    auto res = synthesizeToPcmWithTrace(handle, "ɡusano", 1.0, 140.0, 0.5, 22050);
    auto tr = readFrameTrace(handle);
    REQUIRE(!res.pcm.empty());
    REQUIRE(!tr.empty());

    // Try both /g/ ASCII and /ɡ/ IPA glyph.
    int gIdx = findPhonemeFrameIdx(tr, "ɡ");
    if (gIdx < 0) gIdx = findPhonemeFrameIdx(tr, "g");
    REQUIRE(gIdx >= 0);
    const long g_start = (gIdx < static_cast<int>(res.samplePositions.size()))
        ? static_cast<long>(res.samplePositions[gIdx]) : -1;
    REQUIRE(g_start >= 0);

    // Find the /u/ that follows for relative comparison
    const long u_start = findPhonemeSampleAfter(res, tr, "u", gIdx);
    REQUIRE(u_start > 0);

    // /g/ as stop has a closure phase followed by a burst. Frame trace
    // gives the START of /g/'s closure, which is silent. Skip the closure
    // and measure burst + early voicing: offset 40 ms, window 40 ms.
    const std::size_t g_off = 22050 * 40 / 1000;
    const std::size_t g_win = 22050 * 40 / 1000;
    const std::size_t v_off = 22050 * 20 / 1000;
    const std::size_t v_win = 22050 * 40 / 1000;

    const double gRms = rmsAmplitude(res.pcm, g_start + g_off, g_win);
    const double uRms = rmsAmplitude(res.pcm, u_start + v_off, v_win);
    const double gDbVsU = dbRatio(gRms, uRms);

    MESSAGE("  ----- /g/ WORD-INITIAL STOP REPORT (gusano) -----");
    MESSAGE("  raw RMS: /g/=" << gRms << "  /u/=" << uRms);
    MESSAGE("  /g/ dB vs following /u/: " << gDbVsU << " dB");
    MESSAGE("  (Compare to /ɣ/ intervocalic result above — if /g/ is");
    MESSAGE("   much more present than /ɣ/, the /ɣ_es/ tuning is the issue.)");

    // Diagnostic only — no hard threshold. Real insight is the comparison
    // to /ɣ/ above. If /g/ is at -5 dB vs vowel and /ɣ/ is also around
    // -5 dB, velar place articulation is not differentiating stop from
    // approximant by amplitude alone.
    CHECK(true);

    writeWav("test_output_gusano.wav", res.pcm, 22050);
    MESSAGE("  WAV written: test_output_gusano.wav");
}
