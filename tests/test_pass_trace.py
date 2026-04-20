"""Pass-trajectory tests using the NVSP_DATA_PASSTRACE API (v3.10+).

These tests observe how individual phonemes are mutated by each pass in the
frontend pipeline. Complements test_word_context.py: frame-trace tests catch
regressions at the final emission layer; pass-trace tests catch regressions
where a distinction is compressed partway through the pipeline.

Scaffold for per-pass unit tests — once we know which pass is responsible for
a given mutation, we can write targeted C++ doctests for that pass in
isolation. The pass-trace gives us the "which pass" signal.
"""
from __future__ import annotations

from collections import defaultdict

import pytest

from _test_frontend import read_pass_trace


# ---------------------------------------------------------------------------
# Pass-trace sanity
# ---------------------------------------------------------------------------

def test_pass_trace_is_non_empty(es_mx):
    """Minimal utterance produces snapshots. If this fails, the pass trace
    isn't being populated at all — sink plumbing broken somewhere upstream
    of the tests."""
    fe, h = es_mx
    fe.capture_frames(h, "aɣa")
    trace = read_pass_trace(fe, h)
    assert len(trace) > 0, "pass trace empty — sink not being populated"


def test_pass_trace_covers_expected_passes(es_mx):
    """Sanity: the core acoustic-mutating passes should all be represented
    in the trace. If any go missing, pass_pipeline.cpp may have de-registered
    one, or a pass returned false silently.
    """
    fe, h = es_mx
    fe.capture_frames(h, "aɣa")
    trace = read_pass_trace(fe, h)

    pass_names = {s.pass_name for s in trace}
    expected = {
        "allophones",
        "coarticulation",
        "boundary_smoothing",
        "rate_compensation",
        "trajectory_limit",
    }
    missing = expected - pass_names
    assert not missing, (
        f"pass trace missing expected passes: {missing}. "
        f"Present passes: {sorted(pass_names)}"
    )


def test_pass_trace_resets_between_utterances(es_mx):
    """Pass-snapshot state must not leak across utterances."""
    fe, h = es_mx

    fe.capture_frames(h, "entɾeɣaðo")
    long_len = len(read_pass_trace(fe, h))

    fe.capture_frames(h, "a")
    short_len = len(read_pass_trace(fe, h))

    assert short_len < long_len, (
        f"second utterance's trace ({short_len} entries) should be shorter "
        f"than first's ({long_len}) — snapshots leaked across utterances."
    )


# ---------------------------------------------------------------------------
# Pass-by-pass phoneme trajectory
# ---------------------------------------------------------------------------

def _snapshots_for_phoneme(trace, key_prefix):
    """Return all snapshots whose phoneme key starts with `prefix`, grouped by
    pass name. Prefix match handles dialect suffixes (ɣ → ɣ_es).
    """
    out = defaultdict(list)
    for s in trace:
        if s.phoneme_key.startswith(key_prefix):
            out[s.pass_name].append(s)
    return out


def test_g_vs_l_f1_distinction_preserved_at_every_pass(es_mx):
    """Issue #84/#95 pass-level regression guard.

    The word-context frame-trace test (test_entregado_g_distinct_from_entrelado_l_in_word_context)
    only asserts the /ɣ/-vs-/l/ F1 distinction at emission time. This test
    walks the pipeline pass-by-pass and asserts the distinction is preserved
    at EVERY stage. If any pass collapses cf1 between /ɣ/ and /l/ below a
    threshold, it fires with diagnostic info naming the guilty pass.

    This is the "scaffold" step toward per-pass unit tests — once we know
    which pass compresses the distinction, we can target it directly.

    Methodology: synthesize 'entregado' and 'entrelado' separately. For each
    pass, extract the /ɣ/ snapshot from the first word and the /l/ snapshot
    from the second. Compare cf1.
    """
    fe, h = es_mx

    fe.capture_frames(h, "entɾeɣaðo")
    g_trace = read_pass_trace(fe, h)

    fe.capture_frames(h, "entɾelaðo")
    l_trace = read_pass_trace(fe, h)

    g_snaps = _snapshots_for_phoneme(g_trace, "ɣ")
    l_snaps = _snapshots_for_phoneme(l_trace, "l")

    assert g_snaps, (
        "/ɣ/ phoneme not found in any pass snapshot for /entɾeɣaðo/. "
        "IPA normalization may have coalesced it; engine-level regression."
    )
    assert l_snaps, "/l/ phoneme not found in any pass snapshot for /entɾelaðo/."

    # For each pass that captured both /ɣ/ and /l/, build a trajectory row.
    trajectory = []
    for pass_name in sorted(set(g_snaps) & set(l_snaps)):
        g = g_snaps[pass_name][0]
        l = l_snaps[pass_name][0]
        trajectory.append((pass_name, g.cf1, l.cf1, abs(g.cf1 - l.cf1)))

    assert trajectory, "no pass captured BOTH /ɣ/ and /l/ — pipeline broken"

    # Find the pass where /ɣ/ and /l/ are closest. That's the "weak link"
    # in the distinction chain. If it drops below threshold, we've found
    # the pass that needs investigation.
    weak_pass, g_cf1, l_cf1, min_delta = min(trajectory, key=lambda t: t[3])

    assert min_delta > 30.0, (
        f"Pass '{weak_pass}' compresses /ɣ/-vs-/l/ F1 distinction to "
        f"{min_delta:.0f} Hz (ɣ F1={g_cf1:.0f}, l F1={l_cf1:.0f}).\n"
        f"This would be the pass responsible for a word-context /ɣ/→/l/ collapse.\n"
        f"Full trajectory (cf1 ɣ vs l per pass):\n"
        + "\n".join(
            f"  {name}: ɣ={g:.0f} l={l:.0f} Δ={d:.0f} Hz"
            for name, g, l, d in trajectory
        )
    )
