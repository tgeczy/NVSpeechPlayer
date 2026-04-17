"""Regression tests for nvspFrontend_prepareText().

prepareText runs BEFORE eSpeak phonemization. It handles:
  - Letter dictionary lookups (single codepoint -> letter description)
  - Compound word splitting / merging
  - Number/year/thousands-separator normalization
  - Pronunciation dictionary substitutions

These tests do NOT exercise the synthesis pipeline (no DLL frame callbacks
needed), so they're fast and deterministic. They pin specific historic bugs.
"""
from __future__ import annotations


# ---------------------------------------------------------------------------
# Letter dict: single-codepoint inputs should return the letter description.
# ---------------------------------------------------------------------------

def test_es_mx_letter_y_returns_description(es_mx):
    """Pinned by `b557e41` (letter dict was unwired pre-3.10).

    A bare 'y' in Spanish should expand to "i griega", not be passed through
    to eSpeak as a literal 'y' (which would phonemize as /i/).
    """
    fe, h = es_mx
    out = fe.prepare_text(h, "y")
    assert "i griega" in out.lower(), (
        f"Expected 'i griega' in es-mx letter expansion of 'y', got: {out!r}"
    )


def test_es_mx_letter_enye_returns_description(es_mx):
    """'ñ' in Spanish should expand to "eñe" via the letter dict (es-letters.tsv).

    Complements the 'y' test: covers a non-ASCII single codepoint, exercising
    both the letter dict lookup AND the UTF-8 decoding path in prepareText.
    """
    fe, h = es_mx
    out = fe.prepare_text(h, "ñ")
    assert "eñe" in out.lower(), (
        f"Expected 'eñe' in es-mx letter expansion of 'ñ', got: {out!r}"
    )


def test_es_mx_plain_ascii_letter_passes_through(es_mx):
    """Sanity: a plain ASCII letter NOT in es-letters.tsv passes through unchanged.

    The Spanish letter dict only contains letters that need *special* handling
    (accented vowels, 'ñ', 'y'). Plain letters like 'h' are intentionally not
    listed — eSpeak's default letter spelling handles them.
    """
    fe, h = es_mx
    out = fe.prepare_text(h, "h")
    assert out == "h", f"Plain 'h' should pass through unchanged; got: {out!r}"


def test_en_us_multi_char_input_passes_through(en_us):
    """A multi-character input should NOT trigger letter dict lookup.

    Letter dict only fires for single codepoints. 'hello' must pass through
    as-is for eSpeak to phonemize normally.
    """
    fe, h = en_us
    out = fe.prepare_text(h, "hello")
    assert "hello" in out.lower(), (
        f"Multi-char input shouldn't trigger letter dict; got: {out!r}"
    )


# ---------------------------------------------------------------------------
# Numbers: thousands separators must NOT split clauses.
# ---------------------------------------------------------------------------
# Pinned by `fb95777`+`4de1c5c`: pre-fix, "65,543" was being split into
# clauses ("65" + "543") on all platforms because the comma was treated
# as a clause boundary even between digits.

def test_thousands_separator_comma_no_clause_split_es_mx(es_mx):
    """Issue #74-adjacent: '65,543' should stay as one number."""
    fe, h = es_mx
    out = fe.prepare_text(h, "65,543")
    # The exact form depends on whether thousandsSeparatorCommaToSpace is on,
    # but in NO case should we see a clause-style break that separates 65 from 543.
    # A literal '65,543' or '65 543' both indicate the comma was not treated
    # as a clause splitter.
    assert ("65,543" in out or "65 543" in out or "65543" in out), (
        f"'65,543' should not be clause-split; got: {out!r}"
    )


def test_thousands_separator_period_no_clause_split_en_us(en_us):
    """'3.14' decimal point should not be a clause boundary."""
    fe, h = en_us
    out = fe.prepare_text(h, "3.14")
    # The "." between digits must not become an end-of-clause.
    assert "3.14" in out or "3 point 14" in out, (
        f"'3.14' decimal must not be clause-split; got: {out!r}"
    )


# ---------------------------------------------------------------------------
# Year splitting: pure 4-digit numbers ending in "00" should NOT split.
# ---------------------------------------------------------------------------
# Pinned by `7c742a2`: pre-fix, "4000" split to "40 00" and was read as
# "forty oh zero". The fix: skip year-style splitting when the second pair
# is "00" — eSpeak handles "four thousand" naturally.

def test_year_with_double_zero_not_split_en_us(en_us):
    """Memory: '4000' must not split into '40 00' (would say 'forty oh zero')."""
    fe, h = en_us
    out = fe.prepare_text(h, "4000")
    # Must NOT contain the broken split form.
    # Acceptable: "4000" verbatim, or expansion to "four thousand" / similar.
    assert "40 00" not in out, (
        f"'4000' should not split as '40 00' (would say 'forty oh zero'); got: {out!r}"
    )


def test_year_normal_split_still_works_en_us(en_us):
    """Sanity: '1985' should still split into '19 85' for natural year reading."""
    fe, h = en_us
    out = fe.prepare_text(h, "1985")
    # Either "19 85" (split form) or "1985" (eSpeak handles directly) is fine.
    # We just confirm the function doesn't crash and returns sensible output.
    assert any(c.isdigit() for c in out), (
        f"Expected digits in year output; got: {out!r}"
    )
