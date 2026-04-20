// Tests that probe PhonemeDef field values directly from a loaded pack.
// These are "base truth" assertions — if the YAML phoneme definitions drift
// such that /ɣ_es/ stops differing from /l_es/ at the PhonemeDef level, no
// amount of pipeline work can recover the distinction. These tests pin that
// base truth.
//
// Also serves as the first real use of PackFixture, proving the pack-load
// infrastructure works end-to-end from a C++ test binary.

#include "doctest.h"
#include "pack_fixture.h"

using nvsp_frontend::FieldId;
using nvsp_frontend::PhonemeDef;
using tgsb_test::PackFixture;

TEST_CASE_FIXTURE(PackFixture, "pack: es-mx loads with core phonemes present") {
    REQUIRE(!pack.phonemes.empty());
    CHECK(find(U"a") != nullptr);
    CHECK(find(U"e") != nullptr);
    CHECK(find(U"i") != nullptr);
    CHECK(find(U"o") != nullptr);
    CHECK(find(U"u") != nullptr);
    CHECK(find(U"l") != nullptr);
    CHECK(find(U"ɣ") != nullptr);
}

TEST_CASE_FIXTURE(PackFixture, "pack: es-mx has the dialect-replacement /ɣ_es/ and /l_es/ variants") {
    // Issue #84/#95 investigation: the dialect replacements ɣ→ɣ_es and l→l_es
    // are what actually get synthesized in es-mx. If these don't exist, the
    // replacement rules in es.yaml are silently falling back to base phonemes
    // and the whole dialect-aware pipeline is degraded.
    const PhonemeDef* g_es = find(U"ɣ_es");
    const PhonemeDef* l_es = find(U"l_es");
    REQUIRE_MESSAGE(g_es != nullptr, "phonemes.yaml missing ɣ_es");
    REQUIRE_MESSAGE(l_es != nullptr, "phonemes.yaml missing l_es");
}

TEST_CASE_FIXTURE(PackFixture, "pack: /ɣ_es/ and /l_es/ have distinctly different cf1") {
    // The base-truth F1 distinction that every downstream assertion depends on.
    // If this test fails, the YAML definitions collapsed and there's nothing
    // the passes can do to rescue the phonemes acoustically.
    const PhonemeDef* g = find(U"ɣ_es");
    const PhonemeDef* l = find(U"l_es");
    REQUIRE(g);
    REQUIRE(l);

    const double g_cf1 = resolve(*g, FieldId::cf1);
    const double l_cf1 = resolve(*l, FieldId::cf1);

    INFO("ɣ_es cf1 = " << g_cf1 << "   l_es cf1 = " << l_cf1);
    CHECK(g_cf1 > 0.0);
    CHECK(l_cf1 > 0.0);
    CHECK(std::abs(g_cf1 - l_cf1) > 50.0);  // >50 Hz perceptual threshold
}

TEST_CASE_FIXTURE(PackFixture,
                  "pack: /ɣ_es/ has lower voiceAmplitude than /l_es/ (approximant vs lateral)") {
    // /ɣ/ is a voiced velar approximant — its voicing should be noticeably
    // quieter than /l/'s, which is fully voiced and sonorant. If voiceAmp
    // is equal or inverted, the /ɣ/ has lost its approximant character.
    const PhonemeDef* g = find(U"ɣ_es");
    const PhonemeDef* l = find(U"l_es");
    REQUIRE(g);
    REQUIRE(l);

    const double g_va = resolve(*g, FieldId::voiceAmplitude);
    const double l_va = resolve(*l, FieldId::voiceAmplitude);

    INFO("ɣ_es voiceAmplitude = " << g_va << "   l_es voiceAmplitude = " << l_va);
    CHECK(g_va > 0.0);
    CHECK(l_va > 0.0);
    CHECK(g_va < l_va);
}
