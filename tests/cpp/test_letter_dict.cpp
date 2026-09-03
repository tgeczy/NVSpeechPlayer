// Letter-name dictionary lookup is case-insensitive (#122).
//
// packs/dict/<lang>-letters.tsv is authored in lowercase ("á" -> "a
// acentuada"), but character navigation and typed-character echo deliver
// capitals as well. The NVDA driver folded case on its own side; every
// other platform (SAPI, Android, iOS, speech-dispatcher) reaches the
// dictionary only through nvspFrontend_prepareText, whose lookup was an
// exact match -- so "Á" silently fell through to eSpeak. Pins both cases.

#include <cstring>
#include <string>

#include "doctest.h"
#include "pack_fixture.h"

#include "nvspFrontend.h"
#include "utf8.h"

namespace {

std::string prepare(nvspFrontend_handle_t h, const char* text) {
    char* out = nvspFrontend_prepareText(h, text);
    if (!out) return std::string();
    std::string s(out);
    nvspFrontend_freeString(out);
    return s;
}

}  // namespace

TEST_CASE_FIXTURE(tgsb_test::HandleFixture,
                  "letter dict: lowercase entry resolves (es \"á\")") {
    CHECK(prepare(handle, "á") == "a acentuada");
}

TEST_CASE_FIXTURE(tgsb_test::HandleFixture,
                  "letter dict: capital letter folds to the lowercase entry (#122)") {
    CHECK(prepare(handle, "Á") == "a acentuada");
    CHECK(prepare(handle, "Ñ") == "eñe");
}

TEST_CASE_FIXTURE(tgsb_test::HandleFixture,
                  "letter dict: multi-character input is never a letter name") {
    // "rrr" is a word, not a character -- it must take the normal path.
    CHECK(prepare(handle, "rrr") != "eñe");
    CHECK(prepare(handle, "ÁÁ").find("acentuada") == std::string::npos);
}

TEST_CASE("letter dict: codepoint fold covers every shipped alphabet") {
    using nvsp_frontend::foldCodepointLower;
    CHECK(foldCodepointLower(U'A') == U'a');
    CHECK(foldCodepointLower(U'Á') == U'á');  // Á -> á
    CHECK(foldCodepointLower(U'Ñ') == U'ñ');  // Ñ -> ñ
    CHECK(foldCodepointLower(U'×') == U'×');  // × unchanged
    CHECK(foldCodepointLower(U'Ő') == U'ő');  // Ő -> ő (hu)
    CHECK(foldCodepointLower(U'Ž') == U'ž');  // Ž -> ž (hr/cs/sk)
    CHECK(foldCodepointLower(U'Ł') == U'ł');  // Ł -> ł (pl)
    CHECK(foldCodepointLower(U'İ') == U'i');        // İ -> i (tr)
    CHECK(foldCodepointLower(U'Я') == U'я');  // Я -> я (ru)
    CHECK(foldCodepointLower(U'Є') == U'є');  // Є -> є (uk)
    CHECK(foldCodepointLower(U'Ґ') == U'ґ');  // Ґ -> ґ (uk)
    CHECK(foldCodepointLower(U'á') == U'á');  // already lower
    CHECK(foldCodepointLower(U'7') == U'7');
}
