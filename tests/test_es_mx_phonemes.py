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

def test_g_and_l_acoustic_distinction_minimal_context(es_mx):
    """Issue #84/#95 regression guard: 'entregado' was sounding like 'entrelado'
    (intervocalic /g/ allophone /ɣ/ collapsing acoustically toward /l/).

    METHODOLOGY: minimal 3-phoneme context (/aɣa/ vs /ala/). Each input
    produces exactly 3 voiced frames (vowel-consonant-vowel), so frame[1] is
    reliably the consonant — no need for phoneme attribution on the callback.

    This sidesteps the vowel-domination problem that breaks aggregate-stat
    tests on whole words: in /entreɣaðo/ vs /entrelaðo/ the consonant
    contributes ~3 frames out of ~50, so MIN/AVG/MAX over the whole word
    is dominated by /e/ /a/ /o/ and the consonant difference is invisible.

    Empirical baseline (es-mx, default speed/pitch, post-3.10-beta-1):
      /aɣa/ middle frame: F1=450, F2=1450, voiceAmplitude=0.82
      /ala/ middle frame: F1=350, F2=1400, voiceAmplitude=0.90
    The 100 Hz F1 gap and the ~0.08 voiceAmplitude dip on /ɣ/ are the engine
    actually distinguishing these phonemes. If they collapse to identical
    parameters, this test fires.
    """
    fe, h = es_mx
    g_frames = voiced_frames(fe.capture_frames(h, "aɣa"))
    l_frames = voiced_frames(fe.capture_frames(h, "ala"))

    # Frame count is exact for minimal 3-phoneme inputs. If this ever produces
    # different counts, frame emission timing has changed — worth knowing.
    assert len(g_frames) == 3, (
        f"Expected 3 voiced frames for /aɣa/, got {len(g_frames)}: {g_frames}"
    )
    assert len(l_frames) == 3, (
        f"Expected 3 voiced frames for /ala/, got {len(l_frames)}: {l_frames}"
    )

    # Frame [1] is the consonant in V-C-V structure.
    g = g_frames[1].frame_dict
    l = l_frames[1].frame_dict

    # F1 must differ by at least 50 Hz. Empirical delta is ~100 Hz; a 50 Hz
    # threshold catches collapse without being so tight it false-fails on
    # legitimate small parameter retunings.
    f1_delta = abs(g["cf1"] - l["cf1"])
    assert f1_delta > 50.0, (
        f"/ɣ/ and /l/ collapsed to identical F1 (delta={f1_delta:.0f} Hz). "
        f"This is the issue #84/#95 acoustic regression — the engine has "
        f"lost the F1 distinction between intervocalic /g/-allophone and /l/. "
        f"ɣ frame: F1={g['cf1']:.0f} F2={g['cf2']:.0f} vAmp={g['voiceAmplitude']:.2f}; "
        f"l frame: F1={l['cf1']:.0f} F2={l['cf2']:.0f} vAmp={l['voiceAmplitude']:.2f}"
    )


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
