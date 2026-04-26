// Spanish /rs/ /rc/ /sg/ /ts/ cluster clarity probes — issue #81.
//
// Greg reported (#81, 2026-04-23): /s/ /c/ /z/ in consonant clusters sounds
// too soft / not prominently articulated. His word list with capital-letter
// notation marks which consonants should be CLEARLY heard:
//   versión → verSión
//   persona → perSona
//   torso → torSo
//   percepción → perCepción
//   hasgo → hasGo
//   catorce → cator-ce  (Mexican only)
//   porciento → por-ciento  (Mexican only)
//
// This test renders representative words in es-mx context. The companion
// audit play-back via PowerShell SoundPlayer lets a Spanish-speaker (Tomi)
// validate /s/ /g/ clarity vs eSpeak es-mx as the linguist-tuned reference.
//
// IPA strings come from `espeak-ng -v es-mx -q --ipa "<word>"`.

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

static void writeTrace(const std::string& path,
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

TEST_CASE_FIXTURE(HandleFixture,
                  "Audit: Spanish rs/rc/sg/ts cluster clarity (#81)") {
    struct Word { const char* name; const char* ipa; const char* note; };
    Word words[] = {
        // /rs/ cluster — the primary complaint
        {"version",    "beɾsjˈon",     "/rs/ — Greg: 'verSión'"},
        {"persona",    "peɾsˈona",     "/rs/ — Greg: 'perSona'"},
        {"torso",      "tˈoɾso",       "/rs/ — Greg: 'torSo'"},
        {"dorsal",     "ðoɾsˈal",      "/rs/ — Greg: 'dorSal'"},

        // /rc/ → /rs/ in es-mx
        {"percepcion", "pˌeɾsepsjˈon", "/rc/+/c/ es-mx → 'perCepción'"},
        {"catorce",    "katˈoɾse",     "/rc/ es-mx → 'cator-ce'"},
        {"porciento",  "poɾsjˈɛnto",   "/rc/ es-mx → 'por-ciento'"},

        // /sg/ cluster
        {"hasgo",      "ˈasɣo",        "/sg/ — Greg: 'hasGo'"},

        // /ts/ in pizza — both dialects merge to /ts/
        {"pizza",      "pˈitsa",       "/ts/ — Greg: 'picsa'"},

        // Broader /s_mx/ regression: common words with /s/ in varied contexts
        {"casa",       "kˈasa",        "common /s/ intervocalic"},
        {"esto",       "ˈesto",        "common /s/ before stop /st/"},
        {"cosa",       "kˈosa",        "common /s/ intervocalic"},
        {"amigos",     "amˈiɣos",      "common /s/ word-final after vowel"},
    };

    for (const auto& w : words) {
        auto res = synthesizeToPcmWithTrace(handle, w.ipa, 1.0, 110.0, 0.5, 22050);
        if (res.pcm.empty()) {
            MESSAGE("  SKIP " << w.name << " (synth empty)");
            continue;
        }
        auto trace = readFrameTrace(handle);
        std::string base = std::string("test_output_es_") + w.name + "_cluster";
        writeWav((base + ".wav").c_str(), res.pcm, 22050);
        writeTrace(base + ".trace", trace, res.samplePositions, res.pcm.size());
        MESSAGE("  " << w.name << "  ->  " << base << ".wav ("
                << res.pcm.size() << " samples)");
    }
}
