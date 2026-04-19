"""Word-context phonemization tests using the frame-trace API (v3.10+).

These tests complement test_es_mx_phonemes.py by catching regressions that
only manifest inside real words (where boundary smoothing, rate compensation,
and allophone rules all interact) rather than in isolated V-C-V minimal
contexts.

The frame-trace API (NVSP_DATA_FRAMETRACE) emits one entry per emitted
phoneme, mapping each phoneme's IPA key to the position of its first frame
in the callback stream. This lets tests pick out the specific consonant
frame inside a multi-phoneme word without fragile timing heuristics.
"""
from __future__ import annotations

import pytest

from _test_frontend import read_frame_trace


# ---------------------------------------------------------------------------
# Frame-trace sanity
# ---------------------------------------------------------------------------

def test_frame_trace_reports_phonemes_in_order(es_mx):
    """Sanity: the trace emits entries in IPA order, frame_index is monotonic
    non-decreasing, and every referenced frame index is a valid position in
    the capture_frames() output list.
    """
    fe, h = es_mx
    records = fe.capture_frames(h, "aɣa")
    trace = read_frame_trace(fe, h)

    assert len(trace) >= 3, (
        f"expected ≥3 trace entries for /aɣa/ (one per phoneme), got "
        f"{len(trace)}: {trace}"
    )

    # Monotonic non-decreasing frame_index
    for i in range(1, len(trace)):
        assert trace[i].frame_index >= trace[i - 1].frame_index, (
            f"trace frame_index went backwards at entry {i}: {trace}"
        )

    # Every trace entry references a real position in the records list
    for e in trace:
        assert 0 <= e.frame_index < len(records), (
            f"trace entry {e} references out-of-range frame index "
            f"(records has {len(records)} entries)"
        )


def test_frame_trace_resets_between_utterances(es_mx):
    """Each queueIPA_Ex call must produce a fresh trace — state from the
    previous utterance must not leak in.
    """
    fe, h = es_mx

    fe.capture_frames(h, "aɣa")
    first_trace_len = len(read_frame_trace(fe, h))
    assert first_trace_len >= 3

    fe.capture_frames(h, "a")  # much shorter utterance
    second_trace = read_frame_trace(fe, h)
    assert len(second_trace) < first_trace_len, (
        f"second utterance's trace ({len(second_trace)} entries) should be "
        f"shorter than first's ({first_trace_len}) — state leaked across "
        f"utterances. second trace: {second_trace}"
    )


# ---------------------------------------------------------------------------
# Word-context acoustic regression: issue #84/#95 ("entregado" → "entrelado")
# ---------------------------------------------------------------------------

def _find_phoneme(trace, prefix):
    """Return the first trace entry whose phoneme key starts with `prefix`,
    or None. Prefix match handles dialect-replacement suffixes (e.g. ɣ → ɣ_es).
    """
    for e in trace:
        if e.phoneme_key.startswith(prefix):
            return e
    return None


def test_entregado_g_distinct_from_entrelado_l_in_word_context(es_mx):
    """Issue #84/#95 word-context regression guard.

    Grego reported 'entregado' sounding like 'entrelado' in es-mx. The minimal
    V-C-V test (test_g_and_l_acoustic_distinction_minimal_context) already
    rules out pure parameter collision — /aɣa/ and /ala/ produce measurably
    different F1. This test catches the case where the distinction survives
    in isolation but COLLAPSES in word context: something downstream (boundary
    smoothing, rate compensation, allophone rules specific to consonant
    clusters, etc.) would have to be erasing the F1 gap.

    Methodology: full-word synthesis, use the frame-trace API to pick out the
    /ɣ/ and /l/ frames by IPA key rather than by timing, compare F1.

    Tolerance: 30 Hz (lower than the minimal-context test's 50 Hz because
    boundary smoothing legitimately compresses transition segments — we want
    to catch collapse, not legitimate smoothing).
    """
    fe, h = es_mx

    g_records = fe.capture_frames(h, "entɾeɣaðo")
    g_trace = read_frame_trace(fe, h)

    l_records = fe.capture_frames(h, "entɾelaðo")
    l_trace = read_frame_trace(fe, h)

    g_entry = _find_phoneme(g_trace, "ɣ")
    l_entry = _find_phoneme(l_trace, "l")

    assert g_entry is not None, (
        f"no /ɣ/ phoneme found in trace for /entɾeɣaðo/: {g_trace}. "
        f"IPA normalization may have coalesced it; this would be a frontend "
        f"regression on its own."
    )
    assert l_entry is not None, (
        f"no /l/ phoneme found in trace for /entɾelaðo/: {l_trace}"
    )

    g_frame = g_records[g_entry.frame_index].frame_dict
    l_frame = l_records[l_entry.frame_index].frame_dict

    assert g_frame is not None, (
        f"/ɣ/ started on a silence frame (trace entry {g_entry}); "
        f"expected a voiced approximant frame."
    )
    assert l_frame is not None, (
        f"/l/ started on a silence frame (trace entry {l_entry})."
    )

    f1_delta = abs(g_frame["cf1"] - l_frame["cf1"])
    assert f1_delta > 30.0, (
        f"/ɣ/ and /l/ collapsed to near-identical F1 in word context "
        f"(delta={f1_delta:.0f} Hz). Issue #84/#95 word-context regression. "
        f"ɣ first frame (key={g_entry.phoneme_key}): "
        f"F1={g_frame['cf1']:.0f} F2={g_frame['cf2']:.0f} "
        f"vAmp={g_frame['voiceAmplitude']:.2f}; "
        f"l first frame (key={l_entry.phoneme_key}): "
        f"F1={l_frame['cf1']:.0f} F2={l_frame['cf2']:.0f} "
        f"vAmp={l_frame['voiceAmplitude']:.2f}"
    )
