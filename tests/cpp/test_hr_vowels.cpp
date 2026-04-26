// Croatian vowel rendering for cross-engine formant triangulation.
//
// Mario reported (Mastodon, 2026-04-25) that Croatian "rules sound OK with
// eSpeak but a bit unnatural with TGSpeechBox." abbd858 switched the /u/ /o/
// borrows from Finnish (back) to Spanish (front) per Bašić 2023's Slavic
// formant targets. Tomi can't ear-test Croatian, so we triangulate against:
//   1. eSpeak NG (decades of linguist tuning, Mario calls it OK)
//   2. peer-reviewed formant targets (Bašić 2023, cited in hr.yaml)
//   3. TGSB's actual rendered output (this test)
//
// For each word, dumps:
//   * test_output_hr_<word>_tgsb.wav  — rendered audio
//   * test_output_hr_<word>_tgsb.trace — phoneme-trace sidecar (one entry per
//     line: "<sample_start> <sample_end> <phonemeKey>"). Lets the Python
//     comparison driver align analysis windows on the actual phoneme
//     boundaries the engine emitted, instead of percentile guesses.
//
// IPA strings come from `espeak-ng -v hr -q --ipa "<word>"`. The hr.yaml
// normalization rules then re-route to dialect-specific phonemes.

#include "doctest.h"
#include "audio_capture.h"
#include "wav_export.h"
#include "pack_fixture.h"

#include <cstdio>
#include <string>

using tgsb_test::HandleFixture;
using tgsb_test::synthesizeToPcmWithTrace;
using tgsb_test::readFrameTrace;
using tgsb_test::writeWav;

// HandleFixture variant that loads the Croatian pack (hr.yaml + abbd858
// /u/ /o/ borrow updates) instead of the default es-mx.
struct HrHandleFixture : tgsb_test::HandleFixture {
    HrHandleFixture() : HandleFixture("hr") {}
};

// Write a minimal phoneme-trace sidecar so Python can map the target vowel
// to its actual sample range. Format: one phoneme per line,
//   "<start_sample> <end_sample> <phonemeKey>"
// where <end_sample> is the start of the next phoneme (or pcm end for the
// last). phonemeKey is the post-replacement key (e.g. "u_es", not "u").
static void writeTraceSidecar(const std::string& path,
                              const std::vector<tgsb_test::TraceEntry>& trace,
                              const std::vector<std::size_t>& samplePositions,
                              std::size_t pcmTotal) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    for (std::size_t i = 0; i < trace.size(); ++i) {
        const auto& e = trace[i];
        const std::size_t start = (static_cast<std::size_t>(e.frameIndex)
                                   < samplePositions.size())
            ? samplePositions[e.frameIndex] : 0;
        std::size_t end = pcmTotal;
        if (i + 1 < trace.size()) {
            const auto& next = trace[i + 1];
            if (static_cast<std::size_t>(next.frameIndex) < samplePositions.size())
                end = samplePositions[next.frameIndex];
        }
        std::fprintf(f, "%zu %zu %s\n", start, end, e.phonemeKey.c_str());
    }
    std::fclose(f);
}

TEST_CASE_FIXTURE(HrHandleFixture,
                  "Audit: Croatian vowel renders for engine triangulation") {
    struct Word { const char* name; const char* ipa; const char* note; };
    Word words[] = {
        // /u/-bearing — abbd858 changed (Finnish → Spanish borrow)
        {"uvod",  "ˈuvod",  "/u/ initial stressed"},
        {"kruh",  "kɾˈuh",  "/u/ stressed after cluster"},
        {"guma",  "ɡˈuma",  "/u/ stressed after stop"},
        {"put",   "pˈut",   "/u/ stressed monosyllable"},

        // /o/-bearing — abbd858 changed (Finnish → Spanish borrow)
        {"novo",  "nˈovo",  "/o/ stressed and unstressed"},
        {"dom",   "dˈom",   "/o/ stressed monosyllable"},
        {"most",  "mˈost",  "/o/ stressed before cluster"},
        {"voda",  "vˈoda",  "/o/ stressed before /a/"},

        // /a/ — sanity check on a_open borrow (unchanged by abbd858)
        {"kava",  "kˈava",  "/a/ stressed first syllable"},
        {"vrat",  "vɾˈat",  "/a/ monosyllable"},
        {"rad",   "ɾˈad",   "/a/ short word"},

        // /e/ — sanity check on e_fi borrow (unchanged by abbd858)
        {"selo",  "sˈelo",  "/e/ stressed before /lo/"},
        {"let",   "lˈet",   "/e/ monosyllable"},
        {"pet",   "pˈet",   "/e/ monosyllable after stop"},

        // /i/ — sanity check on i_hu borrow (unchanged by abbd858)
        {"ime",   "ˈime",   "/i/ initial stressed"},
        {"sit",   "sˈit",   "/i/ monosyllable"},
        {"vid",   "vˈid",   "/i/ monosyllable"},
    };

    // Pitch 110 Hz matches Adam baseline (same value the Spanish audit uses).
    // Speed 1.0 = normal rate; vowel center is most stable at slow-to-normal.
    for (const auto& w : words) {
        auto res = synthesizeToPcmWithTrace(handle, w.ipa, 1.0, 110.0, 0.5, 22050);
        if (res.pcm.empty()) {
            MESSAGE("  SKIP " << w.name << " (synth empty)");
            continue;
        }
        auto trace = readFrameTrace(handle);
        std::string base = std::string("test_output_hr_") + w.name + "_tgsb";
        writeWav((base + ".wav").c_str(), res.pcm, 22050);
        writeTraceSidecar(base + ".trace", trace, res.samplePositions, res.pcm.size());
        MESSAGE("  " << w.name << "  ->  " << base << ".wav ("
                << res.pcm.size() << " samples, " << trace.size()
                << " phonemes traced)");
    }
}
