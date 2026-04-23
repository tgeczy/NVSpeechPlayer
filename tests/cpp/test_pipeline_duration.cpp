// Duration tests that exercise the full frontend pipeline (convertIpaToTokens)
// at realistic user speeds.
//
// Speed semantics (for reference):
//   NVDA rate slider 0–100 maps via _curRate = 0.25 * 2^(rate/25) to:
//     rate 0   -> speed 0.25  (very slow)
//     rate 50  -> speed 1.0   (default "normal")
//     rate 75  -> speed 2.0   (NVDA synth cap; timeStretch kicks in beyond)
//     rate 100 -> speed 4.0   (handled as timeStretch post-synth)
//   `speed` is a DIVISOR: convertIpaToTokens divides baseline durations by it.
//   So speed=1.0 gives ~60 ms vowels, speed=2.0 gives ~30 ms vowels, etc.
//
// These tests pin that at realistic user speeds, consonants like /ɣ/ stay
// above audibility. If rate_compensation or any other pass starts crushing
// consonants too aggressively, these fire — the exact "word parts collapse
// way way too quick" regression Tomi described.

#include "doctest.h"
#include "pack_fixture.h"

#include "ipa_engine.h"

using nvsp_frontend::convertIpaToTokens;
using nvsp_frontend::Token;
using tgsb_test::PackFixture;

// Return the first non-silence token whose phoneme key starts with `prefix`
// (prefix-match handles dialect suffixes e.g. ɣ → ɣ_es).
static const Token* findFirst(const std::vector<Token>& tokens,
                              const std::u32string& prefix) {
    for (const Token& t : tokens) {
        if (!t.def) continue;
        const auto& key = t.def->key;
        if (key.size() >= prefix.size() &&
            key.compare(0, prefix.size(), prefix) == 0) {
            return &t;
        }
    }
    return nullptr;
}

// Return the LAST non-silence token whose phoneme key starts with `prefix`.
static const Token* findLast(const std::vector<Token>& tokens,
                             const std::u32string& prefix) {
    const Token* last = nullptr;
    for (const Token& t : tokens) {
        if (!t.def) continue;
        const auto& key = t.def->key;
        if (key.size() >= prefix.size() &&
            key.compare(0, prefix.size(), prefix) == 0) {
            last = &t;
        }
    }
    return last;
}

TEST_CASE_FIXTURE(PackFixture,
                  "duration: velar in /entɾeɣaðo/ at normal speed (1.0) is present") {
    // After /ɣ/→/ɡ_es/ routing change, the velar is a stop (~6 ms burst body
    // + 8 ms closure) instead of an approximant (~30 ms). Body durationMs is
    // canonical-short for stops; intelligibility comes from closure+burst,
    // not body length. Just guard presence here — duration check inadequate
    // for stops. A future test should cover the closure+burst pattern.
    std::vector<Token> tokens;
    std::string err;
    REQUIRE(convertIpaToTokens(pack, "entɾeɣaðo", 1.0, 140.0, 0.5, '.', tokens, err));

    const Token* g = findFirst(tokens, U"ɡ");
    REQUIRE_MESSAGE(g, "/ɡ_es/ token missing at speed 1.0");
    INFO("key=" << std::string(g->def->key.begin(), g->def->key.end())
         << "  durationMs=" << g->durationMs);
    CHECK(g->durationMs >= 4.0);  // stop body, ~6 ms canonical
}

TEST_CASE_FIXTURE(PackFixture,
                  "duration: velar at NVDA synth cap (2.0) is still present") {
    // speed=2.0 is the hardest real-world case — NVDA caps the synth there
    // and uses timeStretch for faster rates. After /ɣ/→/ɡ_es/, the velar
    // is now a stop with a fixed-short canonical body (~6 ms / 2 = 3 ms at
    // speed 2). Burst-presence guard only — the closure+burst pattern is
    // what carries intelligibility, not body length per se.
    std::vector<Token> tokens;
    std::string err;
    REQUIRE(convertIpaToTokens(pack, "entɾeɣaðo", 2.0, 140.0, 0.5, '.', tokens, err));

    const Token* g = findFirst(tokens, U"ɡ");
    REQUIRE(g);
    INFO("key=" << std::string(g->def->key.begin(), g->def->key.end())
         << "  durationMs=" << g->durationMs);
    CHECK_MESSAGE(g->durationMs >= 2.0,
                  "/ɡ_es/ stop body collapsed below 2 ms at NVDA max synth speed");
}

TEST_CASE_FIXTURE(PackFixture,
                  "duration: /ɡ_es/ vs /l_es/ both present in matched word context") {
    // After /ɣ/→/ɡ_es/, comparing a stop body (~6 ms canonical) to a
    // sonorant (~30 ms) by ratio doesn't carry the same meaning the
    // approximant-vs-lateral comparison did. Just ensure both are present
    // — that's the regression guard worth keeping post-routing-change.
    std::vector<Token> g_toks, l_toks;
    std::string err;
    REQUIRE(convertIpaToTokens(pack, "entɾeɣaðo", 1.0, 140.0, 0.5, '.', g_toks, err));
    REQUIRE(convertIpaToTokens(pack, "entɾelaðo", 1.0, 140.0, 0.5, '.', l_toks, err));

    const Token* g = findFirst(g_toks, U"ɡ");
    const Token* l = findFirst(l_toks, U"l");
    REQUIRE(g);
    REQUIRE(l);

    INFO("/ɡ_es/ = " << g->durationMs << " ms   /l_es/ = " << l->durationMs << " ms");
    CHECK(g->durationMs > 0.0);
    CHECK(l->durationMs > 0.0);
}

TEST_CASE_FIXTURE(PackFixture,
                  "duration: final unstressed vowel survives synth cap (2.0)") {
    // Tomi's hypothesis: at fast rates, unstressed vowels collapse
    // "way way too quick" — characteristic Spanish-at-speed unclarity.
    // The final /o/ in /entɾeɣaðo/ is word-final AND unstressed: the
    // worst-case candidate for aggressive rate compensation.
    std::vector<Token> tokens;
    std::string err;
    REQUIRE(convertIpaToTokens(pack, "entɾeɣaðo", 2.0, 140.0, 0.5, '.', tokens, err));

    const Token* final_o = findLast(tokens, U"o");
    REQUIRE_MESSAGE(final_o, "final /o/ not found — replacement may have altered key");
    INFO("final /o/ (key=" << std::string(final_o->def->key.begin(), final_o->def->key.end())
         << ") durationMs=" << final_o->durationMs);
    CHECK_MESSAGE(final_o->durationMs >= 15.0,
                  "final unstressed vowel collapsed below 15 ms at speed 2.0 — "
                  "characteristic of the 'Spanish at speed is unclear' problem");
}

TEST_CASE_FIXTURE(PackFixture,
                  "duration: speed ratio is predictable (2x speed ≈ halved duration)") {
    // Rate compensation should behave monotonically and predictably. If
    // /ɣ/ gets weird special-case treatment at certain speeds, comparing
    // the duration at speed 1.0 vs 2.0 should still show roughly
    // 2x-faster = half-duration. Within ±25% tolerance.
    std::vector<Token> t1, t2;
    std::string err;
    REQUIRE(convertIpaToTokens(pack, "entɾeɣaðo", 1.0, 140.0, 0.5, '.', t1, err));
    REQUIRE(convertIpaToTokens(pack, "entɾeɣaðo", 2.0, 140.0, 0.5, '.', t2, err));

    const Token* g1 = findFirst(t1, U"ɡ");
    const Token* g2 = findFirst(t2, U"ɡ");
    REQUIRE(g1);
    REQUIRE(g2);

    const double shrinkRatio = g2->durationMs / g1->durationMs;
    INFO("speed=1.0: " << g1->durationMs << " ms   speed=2.0: " << g2->durationMs
         << " ms   shrink=" << shrinkRatio);
    // Expected ~0.5 (perfect 2x scaling). Allow 0.35–0.65 for non-linear
    // compensation that legitimately preserves minimum durations.
    CHECK(shrinkRatio > 0.35);
    CHECK(shrinkRatio < 0.65);
}
