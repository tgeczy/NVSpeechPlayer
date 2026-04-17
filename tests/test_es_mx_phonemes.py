"""Spanish-Mexican phonemization regression tests (es-mx).

Bug-driven: each test pins a real shipped issue from GitHub
(#74, #81, #84, #95). These exercise the IPA -> frame-stream pipeline
via nvspFrontend_queueIPA_Ex.

KNOWN LIMITATION: the frontend doesn't currently expose a phoneme-attribution
API on the frame callback (we get per-frame DSP parameters, not "this frame
came from phoneme /g/"). So these tests can detect:

  - Crashes / NULL outputs when the engine can't process an input
  - Total frame count regressions (entire word was dropped vs preserved)
  - Acoustic divergence between two known-different inputs (e.g. /g/ vs /l/
    between vowels MUST produce different frame streams; if they don't, the
    engine has lost its ability to distinguish them)

Once a phoneme-attribution API is added, these tests can become more precise:
"frame at index N was attributed to phoneme /ɣ/, not /l/." For now they're
acoustic-differentiation tests, which still catch real regressions.
"""
from __future__ import annotations

import pytest

from _test_frontend import voiced_frames


def _summary(records):
    """Compact text summary of a captured frame stream — useful in assertion messages."""
    voiced = voiced_frames(records)
    silences = [r for r in records if r.is_silence]
    total_ms = sum(r.duration_ms for r in records)
    return (f"{len(records)} records ({len(voiced)} voiced, {len(silences)} silence), "
            f"~{total_ms:.0f}ms total")


# ---------------------------------------------------------------------------
# Smoke: the engine can synthesize Spanish IPA at all.
# ---------------------------------------------------------------------------

def test_es_mx_synthesizes_simple_word(es_mx):
    """Sanity: 'hola' should produce a non-empty voiced frame stream."""
    fe, h = es_mx
    records = fe.capture_frames(h, "ola")  # /ola/ — IPA for "hola" (h is silent in Spanish)
    voiced = voiced_frames(records)
    assert len(voiced) > 0, f"Expected voiced frames for /ola/; got {_summary(records)}"


# ---------------------------------------------------------------------------
# Acoustic differentiation: phonemes that SHOULD sound different must produce
# distinguishable frame streams. If they collapse to identical output, the
# engine has lost the ability to distinguish them — i.e. a regression that
# would manifest as "X sounds like Y" user reports.
# ---------------------------------------------------------------------------

@pytest.mark.skip(reason=(
    "Methodology not yet sufficient. Aggregate stats (MIN/MAX/AVG) over the whole "
    "word are dominated by vowel frames — the few consonant frames don't move the "
    "needle. Reproduced experimentally: MIN F2 is identical (910 Hz, dominated by "
    "/o/ in '-aðo') for both /entreɣaðo/ and /entrelaðo/. Re-enable once one of: "
    "(1) frontend exposes phoneme attribution on the frame callback ('frame N is "
    "from phoneme /ɣ/'), (2) we add a wrapper around previewPhoneme that diffs "
    "individual phoneme definitions, or (3) we synthesize minimal contexts like "
    "'aɣa' vs 'ala' where the consonant dominates the stream."
))
def test_g_and_l_intervocalic_produce_different_streams(es_mx):
    """Issue #84/#95: 'entregado' was sounding like 'entrelado' (/g/ -> /l/).

    Currently skipped — see decorator. Kept in the suite as a reminder that this
    is the bug class we want to catch automatically once phoneme attribution exists.
    """
    fe, h = es_mx
    g_frames = voiced_frames(fe.capture_frames(h, "entreɣaðo"))
    l_frames = voiced_frames(fe.capture_frames(h, "entrelaðo"))
    g_min_f2 = min(f.frame_dict["cf2"] for f in g_frames)
    l_min_f2 = min(f.frame_dict["cf2"] for f in l_frames)
    assert abs(g_min_f2 - l_min_f2) > 1.0


# ---------------------------------------------------------------------------
# Dialect divergence: es-mx and es-es should produce different output for
# words where the dialects differ (e.g. /s/ realization). If they don't,
# the dialect replacement (s -> s_es vs s -> s_mx) isn't firing.
# ---------------------------------------------------------------------------

def test_mexican_s_diverges_from_spain_s(es_mx, es_es):
    """Issue #74/#81: Mexican /s/ was sounding identical to Spain /s/ in beta 1.

    Different dialect packs replace /s/ with different language-specific phonemes
    (s_mx vs s_es) which should have measurably different acoustic parameters.
    If the dialect divergence isn't firing, the two will produce identical streams.

    Test word: 'casa' (very common, contains /s/ in a clean intervocalic context).
    """
    fe_mx, h_mx = es_mx
    fe_es, h_es = es_es

    # /s/ is voiceless — voiced_frames() would filter it out entirely. Use ALL
    # non-silence frames so the /s/ region is included in the comparison.
    mx_records = [r for r in fe_mx.capture_frames(h_mx, "kasa") if not r.is_silence]
    es_records = [r for r in fe_es.capture_frames(h_es, "kasa") if not r.is_silence]

    assert len(mx_records) > 0 and len(es_records) > 0, "one dialect produced no frames"

    # Diagnostic note: s_es and s_mx in packs/phonemes.yaml share fricationAmplitude
    # (both 0.9) and total parallel energy (pa5+pa6 both sum to 1.0), but distribute
    # that energy DIFFERENTLY across F5 and F6:
    #
    #   s_es: pa5=0.9,  pa6=0.1   (Castilian apical: energy concentrated at F5)
    #   s_mx: pa5=0.35, pa6=0.65  (Mexican laminal: energy spread to F6)
    #
    # pa6 alone is a 6.5x discriminator. MAX pa6 across "kasa" should sit near
    # the /s/ value (vowels and /k/ have pa6 near 0).
    mx_max_pa6 = max(f.frame_dict["pa6"] for f in mx_records)
    es_max_pa6 = max(f.frame_dict["pa6"] for f in es_records)
    delta = abs(mx_max_pa6 - es_max_pa6)

    # If the dialect replacement (s -> s_mx vs s -> s_es) is firing, MAX pa6
    # for the word "kasa" must differ measurably (s_mx ~0.65 vs s_es ~0.10).
    assert delta > 0.05, (
        f"es-mx and es-es produced near-identical pa6 peak for 'casa'. "
        f"Dialect replacement (s -> s_mx vs s -> s_es) likely not firing — "
        f"this is the issue #81 regression. "
        f"mx max pa6: {mx_max_pa6:.4f}, es max pa6: {es_max_pa6:.4f}"
    )
