// Tests for pure helpers in src/frontend/passes/pass_common.h.
//
// These are the lightest-weight C++ tests in the suite — no Token vectors,
// no packs, no synthesis. They pin pure-function invariants that every pass
// depends on. If getPlace() silently starts returning Unknown for /g/ because
// of a refactoring mistake, the whole pipeline degrades and this test fires.

#include "doctest.h"

#include "passes/pass_common.h"

using nvsp_frontend::Place;
using nvsp_frontend::getPlace;

TEST_CASE("getPlace: labial consonants") {
    CHECK(getPlace(U"p") == Place::Labial);
    CHECK(getPlace(U"b") == Place::Labial);
    CHECK(getPlace(U"m") == Place::Labial);
    CHECK(getPlace(U"f") == Place::Labial);
    CHECK(getPlace(U"v") == Place::Labial);
    CHECK(getPlace(U"w") == Place::Labial);
    CHECK(getPlace(U"β") == Place::Labial);  // Spanish intervocalic /b/
}

TEST_CASE("getPlace: alveolar consonants") {
    CHECK(getPlace(U"t") == Place::Alveolar);
    CHECK(getPlace(U"d") == Place::Alveolar);
    CHECK(getPlace(U"n") == Place::Alveolar);
    CHECK(getPlace(U"s") == Place::Alveolar);
    CHECK(getPlace(U"z") == Place::Alveolar);
    CHECK(getPlace(U"l") == Place::Alveolar);
    CHECK(getPlace(U"r") == Place::Alveolar);
    CHECK(getPlace(U"ɾ") == Place::Alveolar);  // Spanish tap
    CHECK(getPlace(U"ɹ") == Place::Alveolar);  // English approximant r
    CHECK(getPlace(U"ð") == Place::Alveolar);  // Spanish intervocalic /d/
}

TEST_CASE("getPlace: palatal / postalveolar consonants") {
    CHECK(getPlace(U"ʃ") == Place::Palatal);
    CHECK(getPlace(U"ʒ") == Place::Palatal);
    CHECK(getPlace(U"j") == Place::Palatal);
    CHECK(getPlace(U"ɲ") == Place::Palatal);
    CHECK(getPlace(U"ʝ") == Place::Palatal);  // Spanish palatal /ɣj/ coalescence
}

TEST_CASE("getPlace: tie-bar affricates are palatal") {
    // Tie-bar affricates have two representations in YAML: U+035C (double
    // arc below) and U+0361 (double arc above). getPlace should recognize
    // the U+0361 form as palatal for /tʃ/ and /dʒ/.
    CHECK(getPlace(U"t\u0361ʃ") == Place::Palatal);
    CHECK(getPlace(U"d\u0361ʒ") == Place::Palatal);
}

TEST_CASE("getPlace: velar consonants") {
    CHECK(getPlace(U"k") == Place::Velar);
    CHECK(getPlace(U"g") == Place::Velar);
    CHECK(getPlace(U"ŋ") == Place::Velar);
    CHECK(getPlace(U"x") == Place::Velar);
    CHECK(getPlace(U"ɣ") == Place::Velar);  // Spanish intervocalic /g/ — the
                                             // phoneme at the center of the
                                             // issue #84/#95 investigation.
}

TEST_CASE("getPlace: unknown / vowel / non-place phonemes") {
    // Vowels and unknown keys have no place of articulation.
    CHECK(getPlace(U"a") == Place::Unknown);
    CHECK(getPlace(U"e") == Place::Unknown);
    CHECK(getPlace(U"i") == Place::Unknown);
    CHECK(getPlace(U"o") == Place::Unknown);
    CHECK(getPlace(U"u") == Place::Unknown);
    CHECK(getPlace(U"") == Place::Unknown);

    // Dialect-suffixed keys are NOT recognized as their base — they're either
    // handled upstream (replaced before getPlace is called) or considered
    // unknown here. This test pins the current behavior; if we later add
    // dialect-aware place classification, this test will need updating.
    CHECK(getPlace(U"ɣ_es") == Place::Unknown);
    CHECK(getPlace(U"l_mx") == Place::Unknown);
}
