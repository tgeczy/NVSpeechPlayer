// Tests that probe post-pipeline Token state for end-target formant
// values (endCf*) and transition-scale fields — the machinery that
// coarticulation, boundary_smoothing, and cluster_* passes use to shape
// formant trajectories during phoneme transitions.
//
// Why this matters (the "egg-center" hunt):
//   The frame-level cf1 of /ɣ/ is stable across passes (we proved this in
//   test_pass_trace.py). Token.field[cf1] of /ɣ/ reads 450 Hz at every pass
//   output. But /ɣ/ isn't emitted as a single steady frame — it's emitted
//   as a trajectory from onset (~450) TOWARD an end target (endCf1).
//
//   If coarticulation sets endCf1 aggressively toward the flanking vowel's
//   F1, the /ɣ/ spends most of its duration sweeping AWAY from its
//   canonical F1, spectrally resembling something else. A listener
//   integrating energy over the phoneme's duration would hear the mean F1,
//   not the onset F1. If that mean is closer to /l/'s F1 than to /ɣ/'s,
//   you get "entrelado" perceptually even though cf1 at the emission start
//   is correct.

#include "doctest.h"
#include "pack_fixture.h"

#include "ipa_engine.h"

using nvsp_frontend::convertIpaToTokens;
using nvsp_frontend::Token;
using tgsb_test::PackFixture;

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

TEST_CASE_FIXTURE(PackFixture,
                  "endCf: /ɣ/ in /aɣa/ does NOT get endCf or transF*Scale set") {
    // Pins a current-engine finding: no pass in the pipeline sets end-target
    // formants or transition-scale fields on /ɣ/ in intervocalic context.
    // The phoneme is emitted as a steady-state frame at its PhonemeDef
    // cf1/cf2/cf3, and the transition to flanking vowels happens purely via
    // fadeMs-driven DSP crossfade.
    //
    // If a future change (e.g. adding coarticulation for approximants) starts
    // populating these fields, this test fires and forces a review — the
    // settings might be intentional, or might be the source of a new
    // word-context regression in /ɣ/ vs neighboring phonemes.
    //
    // This is exactly the kind of "document current behavior as a trip-wire"
    // test that catches unintended-consequence bugs.
    std::vector<Token> toks;
    std::string err;
    REQUIRE(convertIpaToTokens(pack, "aɣa", 1.0, 140.0, 0.5, '.', toks, err));

    const Token* g = findFirst(toks, U"ɡ");
    REQUIRE(g);

    CHECK_FALSE(g->hasEndCf1);
    CHECK_FALSE(g->hasEndCf2);
    CHECK_FALSE(g->hasEndCf3);
    CHECK(g->transF1Scale == 0.0);
    CHECK(g->transF2Scale == 0.0);
    CHECK(g->transF3Scale == 0.0);
}

TEST_CASE_FIXTURE(PackFixture,
                  "endCf: /ɣ/ in /entɾeɣaðo/ also has no endCf set — full word context") {
    // The same finding extended to real word context. If coarticulation
    // responds differently to complex contexts (multi-phoneme surround,
    // stress, cluster position), this test would show it.
    std::vector<Token> toks;
    std::string err;
    REQUIRE(convertIpaToTokens(pack, "entɾeɣaðo", 1.0, 140.0, 0.5, '.', toks, err));

    const Token* g = findFirst(toks, U"ɡ");
    REQUIRE(g);

    CHECK_FALSE(g->hasEndCf1);
    CHECK_FALSE(g->hasEndCf2);
    CHECK_FALSE(g->hasEndCf3);
}

TEST_CASE_FIXTURE(PackFixture,
                  "endCf: /ɣ/'s endCf1 (if set) shouldn't sink into /l/ territory") {
    // If coarticulation sets /ɣ/'s endCf1, it's supposedly sweeping toward
    // the following /a/ (F1 ~730 Hz). That's UP, not DOWN. An endCf1 below
    // /l_es/'s native cf1 (~350 Hz) means something is sweeping /ɣ/ in
    // the WRONG direction — away from /a/ and toward /l/-like territory.
    std::vector<Token> toks;
    std::string err;
    REQUIRE(convertIpaToTokens(pack, "entɾeɣaðo", 1.0, 140.0, 0.5, '.', toks, err));

    const Token* g = findFirst(toks, U"ɡ");
    REQUIRE(g);

    if (g->hasEndCf1) {
        INFO("/ɣ/ endCf1=" << g->endCf1 << " (expected to sweep UP toward /a/ ~730 Hz, not down to /l/ ~350 Hz)");
        CHECK(g->endCf1 >= 400.0);  // anywhere below is suspiciously /l/-like
    } else {
        INFO("/ɣ/ has no endCf1 set — coarticulation didn't touch it");
        CHECK(true);  // no-op pass
    }
}

TEST_CASE_FIXTURE(PackFixture,
                  "endCf: /ɣ/ and /l/ in matched context have distinct trajectories") {
    // The sharper version: compare the full end-target picture.
    // If /ɣ/'s {cf1→endCf1, cf2→endCf2} trajectory ends up in the same
    // acoustic quadrant as /l/'s, listeners can't disambiguate them no
    // matter what the onset formants are.
    std::vector<Token> g_toks, l_toks;
    std::string err;
    REQUIRE(convertIpaToTokens(pack, "entɾeɣaðo", 1.0, 140.0, 0.5, '.', g_toks, err));
    REQUIRE(convertIpaToTokens(pack, "entɾelaðo", 1.0, 140.0, 0.5, '.', l_toks, err));

    const Token* g = findFirst(g_toks, U"ɡ");
    const Token* l = findFirst(l_toks, U"l");
    REQUIRE(g);
    REQUIRE(l);

    // If both have endCf1 set, compare trajectory end points.
    // If neither has it, they steady-state at their respective cf1s —
    // which the phoneme-def test already pinned (>50 Hz apart).
    // If one has it and the other doesn't, that asymmetry is itself
    // worth flagging.
    INFO("ɣ: hasEndCf1=" << g->hasEndCf1 << " endCf1=" << g->endCf1
         << "  l: hasEndCf1=" << l->hasEndCf1 << " endCf1=" << l->endCf1);

    if (g->hasEndCf1 && l->hasEndCf1) {
        // Both sweep. Endpoint separation should still exist.
        const double delta = std::abs(g->endCf1 - l->endCf1);
        CHECK_MESSAGE(delta > 30.0,
                      "/ɣ/ and /l/ endCf1 endpoints collapsed (delta=" << delta
                      << " Hz) — trajectories converge even if onsets differ");
    } else {
        CHECK(true);  // other cases handled by steady-state tests
    }
}

TEST_CASE_FIXTURE(PackFixture,
                  "transition: /ɣ/ doesn't get aggressive transF1Scale") {
    // transF1Scale < 1.0 means "formants lead, source follows" — used for
    // boundary smoothing. But applied aggressively to /ɣ/, it would make
    // /ɣ/'s F1 trajectory extend far into neighboring phonemes, blurring
    // the phoneme boundary. A conservative cap pins this.
    std::vector<Token> toks;
    std::string err;
    REQUIRE(convertIpaToTokens(pack, "entɾeɣaðo", 1.0, 140.0, 0.5, '.', toks, err));

    const Token* g = findFirst(toks, U"ɡ");
    REQUIRE(g);

    INFO("/ɣ/ transF1Scale=" << g->transF1Scale
         << "  transF2Scale=" << g->transF2Scale
         << "  transF3Scale=" << g->transF3Scale);
    // 0.0 means "unset / use default". Positive values are explicit scales.
    // Anything < 0.3 would mean formants transition MUCH faster than source,
    // which smears this phoneme's spectral identity.
    if (g->transF1Scale > 0.0) {
        CHECK(g->transF1Scale >= 0.3);
    }
}
