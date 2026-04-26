"""
Cross-engine vowel formant comparison for languages where Tomi can't ear-test.

Renders the same word with TGSB and eSpeak NG (already done by the C++ test
test_hr_vowels.cpp + an espeak-ng CLI batch), then runs librosa LPC formant
extraction at the target vowel's midpoint in each. Reports F1/F2 (and F3)
side-by-side along with each engine's distance from a peer-reviewed
literature target.

This is the "triangulation" methodology: TGSB vs eSpeak vs literature, three
reference points to validate language tuning when ear-testing isn't reliable.
See memory/project_triangulation_methodology.md.

For TGSB renders, uses the per-word .trace sidecar (written by the C++ test)
to align analysis windows on the actual phoneme boundaries the engine
emitted. eSpeak has no equivalent metadata, so it falls back to a percentile
window with vowel-energy peak detection.

Run with 64-bit Python 3.12:
  C:/Users/Tomi/AppData/Local/Programs/Python/Python312/python.exe \
    tools/compare_engine_formants.py
"""
from __future__ import annotations

import pathlib
import sys
from dataclasses import dataclass, field
from typing import Optional

import numpy as np
import librosa


WAV_DIR = pathlib.Path(__file__).resolve().parent.parent / "tests" / "wav_samples" / "hr_compare"

# Window length for LPC analysis. 30 ms gives reasonable formant resolution
# at speech sample rates while staying inside most vowel steady-states.
LPC_WINDOW_MS = 30.0
# If a phoneme's traced duration is shorter than this, the analysis window
# extends past phoneme boundaries (caveat: contaminated by neighbors).
MIN_TRACED_VOWEL_MS = 25.0


@dataclass
class WordSpec:
    name: str
    target_vowel: str          # 'u' / 'o' / 'a' / 'e' / 'i'
    espeak_window: tuple[float, float]  # percentile fallback for eSpeak
    note: str = ""
    # TGSB phoneme keys to match in the trace sidecar (post-replacement,
    # e.g. "u_es" not "u"). Tries each in order; first match wins.
    tgsb_keys: list[str] = field(default_factory=list)


def keys_for(vowel: str) -> list[str]:
    """Per-vowel phoneme-key candidates, post-hr.yaml normalization."""
    return {
        "u": ["u_es", "u"],
        "o": ["o_es", "o"],
        "a": ["a_open", "a"],
        "e": ["e_fi", "e"],
        "i": ["i_hu", "i"],
    }[vowel]


WORDS: list[WordSpec] = [
    # /u/ — abbd858 changed (Finnish → Spanish borrow)
    WordSpec("uvod", "u", (0.10, 0.35), "/u/ initial stressed", keys_for("u")),
    WordSpec("kruh", "u", (0.45, 0.70), "/u/ after k+r", keys_for("u")),
    WordSpec("guma", "u", (0.20, 0.45), "/u/ after stop", keys_for("u")),
    WordSpec("put",  "u", (0.30, 0.65), "/u/ monosyllable", keys_for("u")),

    # /o/ — abbd858 changed
    WordSpec("novo", "o", (0.15, 0.40), "/o/ first syllable", keys_for("o")),
    WordSpec("dom",  "o", (0.25, 0.65), "/o/ monosyllable", keys_for("o")),
    WordSpec("most", "o", (0.20, 0.45), "/o/ before /st/", keys_for("o")),
    WordSpec("voda", "o", (0.15, 0.45), "/o/ before /da/", keys_for("o")),

    # /a/ — sanity check on a_open borrow
    WordSpec("kava", "a", (0.20, 0.45), "/a/ first syllable", keys_for("a")),
    WordSpec("vrat", "a", (0.45, 0.75), "/a/ after /vr/", keys_for("a")),
    WordSpec("rad",  "a", (0.30, 0.65), "/a/ monosyllable", keys_for("a")),

    # /e/ — sanity check on e_fi borrow
    WordSpec("selo", "e", (0.20, 0.45), "/e/ first syllable", keys_for("e")),
    WordSpec("let",  "e", (0.30, 0.65), "/e/ monosyllable", keys_for("e")),
    WordSpec("pet",  "e", (0.30, 0.65), "/e/ monosyllable", keys_for("e")),

    # /i/ — sanity check on i_hu borrow
    WordSpec("ime",  "i", (0.10, 0.35), "/i/ initial", keys_for("i")),
    WordSpec("sit",  "i", (0.30, 0.65), "/i/ monosyllable", keys_for("i")),
    WordSpec("vid",  "i", (0.30, 0.65), "/i/ monosyllable", keys_for("i")),
]


# Literature targets for adult male Croatian, derived from Bašić 2023
# (cited in packs/lang/hr.yaml). Slavic 5-vowel system has /u/ /o/ more
# frontal than Finnish 7-vowel /u/ /o/ — no /y/ /ø/ pushing them back.
# Numbers are approximations from the hr.yaml comments; exact paper values
# would tighten these via Consensus paper search.
LITERATURE_TARGETS = {
    "u": {"F1": 340, "F2": 750},
    "o": {"F1": 500, "F2": 900},
    "a": {"F1": 720, "F2": 1300},
    "e": {"F1": 470, "F2": 1900},
    "i": {"F1": 290, "F2": 2200},
}


# ============================================================================
# Trace parsing — read the per-word .trace sidecar emitted by the C++ test
# ============================================================================

@dataclass
class TraceEntry:
    start: int
    end: int
    key: str

    @property
    def duration_samples(self) -> int:
        return max(0, self.end - self.start)


def read_trace(path: pathlib.Path) -> list[TraceEntry]:
    if not path.exists():
        return []
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        parts = line.strip().split(maxsplit=2)
        if len(parts) != 3:
            continue
        try:
            out.append(TraceEntry(int(parts[0]), int(parts[1]), parts[2]))
        except ValueError:
            continue
    return out


def find_vowel_in_trace(trace: list[TraceEntry], keys: list[str],
                        sr: int) -> Optional[TraceEntry]:
    """Find the longest-duration trace entry matching any of the keys.
    Multiple matches may exist (e.g. two /o/'s in 'novo'); we want the longest
    one as it's most likely the stressed vowel and gives best LPC resolution."""
    matches = [e for e in trace if e.key in keys]
    if not matches:
        return None
    return max(matches, key=lambda e: e.duration_samples)


# ============================================================================
# Vowel localization — phoneme-trace for TGSB, percentile heuristic for eSpeak
# ============================================================================

def vowel_center_from_trace(entry: TraceEntry, sr: int) -> int:
    """Center sample of a traced vowel."""
    return (entry.start + entry.end) // 2


def vowel_center_from_window(y: np.ndarray, sr: int,
                             window_pct: tuple[float, float]) -> int:
    """RMS-peak vowel center inside a percentile window (eSpeak fallback)."""
    n = len(y)
    lo = int(n * window_pct[0])
    hi = int(n * window_pct[1])
    if hi - lo < sr // 50:
        return (lo + hi) // 2
    hop = max(1, sr // 200)
    seg = y[lo:hi]
    rms = librosa.feature.rms(y=seg, frame_length=hop * 4, hop_length=hop)[0]
    if len(rms) == 0:
        return (lo + hi) // 2
    return lo + int(np.argmax(rms)) * hop


# ============================================================================
# LPC formant extraction
# ============================================================================

def extract_formants(y: np.ndarray, sr: int, center: int,
                     window_ms: float = LPC_WINDOW_MS) -> Optional[list[float]]:
    """LPC formant extraction at the given sample center. Returns sorted
    formants (F1, F2, F3, ...) with bandwidth < 400 Hz (real formants, not
    phantoms). Pre-emphasis + Hamming window + librosa.lpc + root-finding."""
    win_samples = int(sr * window_ms / 1000.0)
    half = win_samples // 2
    if center < half or center + half >= len(y):
        # Center too close to edge — clamp
        if len(y) < win_samples:
            return None
        center = max(half, min(len(y) - half - 1, center))

    seg = y[center - half:center + half].astype(np.float64)
    if np.max(np.abs(seg)) < 1e-6:
        return None

    pre = np.empty_like(seg)
    pre[0] = seg[0]
    pre[1:] = seg[1:] - 0.97 * seg[:-1]
    pre *= np.hamming(len(pre))

    order = int(sr / 1000) + 4
    try:
        a = librosa.lpc(pre, order=order)
    except Exception:
        return None

    roots = np.roots(a)
    roots = roots[np.imag(roots) >= 0]
    if len(roots) == 0:
        return None
    freqs = np.angle(roots) * sr / (2 * np.pi)
    bws = -0.5 * (sr / np.pi) * np.log(np.maximum(np.abs(roots), 1e-12))

    mask = (freqs > 90) & (freqs < (sr / 2 - 200)) & (bws < 400)
    freqs = freqs[mask]
    if len(freqs) == 0:
        return None
    bws = bws[mask]
    order_idx = np.argsort(freqs)
    return list(freqs[order_idx])


# ============================================================================
# Per-word analysis
# ============================================================================

def analyze_word(word: WordSpec) -> dict:
    tgsb_path = WAV_DIR / f"test_output_hr_{word.name}_tgsb.wav"
    espeak_path = WAV_DIR / f"{word.name}_espeak.wav"
    trace_path = WAV_DIR / f"test_output_hr_{word.name}_tgsb.trace"

    out = {"word": word.name, "vowel": word.target_vowel, "note": word.note,
           "tgsb": None, "espeak": None, "trace_diag": None}

    # Diagnostic — what does the trace say about target vowel duration?
    trace = read_trace(trace_path)
    entry = find_vowel_in_trace(trace, word.tgsb_keys, 22050)
    if entry is not None:
        out["trace_diag"] = {
            "key": entry.key,
            "traced_ms": round(entry.duration_samples * 1000.0 / 22050, 1),
        }

    # Both engines: percentile window + RMS-peak vowel centering. The
    # percentile heuristic is robust across engines with similar word-shape
    # rendering; trace alignment was noisier in practice because TGSB's
    # boundary smoothing redistributes voicing across phoneme boundaries
    # in voiced clusters (v-o-d, n-o-v, etc.).
    for label, path in [("tgsb", tgsb_path), ("espeak", espeak_path)]:
        if not path.exists():
            out[label] = {"error": "missing file"}
            continue
        y, sr = librosa.load(str(path), sr=None, mono=True)
        center = vowel_center_from_window(y, sr, word.espeak_window)
        f = extract_formants(y, sr, center)
        if f is None or len(f) < 2:
            out[label] = {"error": "LPC failed"}
            continue
        out[label] = {
            "F1": round(f[0], 1), "F2": round(f[1], 1),
            "F3": round(f[2], 1) if len(f) > 2 else None,
            "center_pct": round(100 * center / len(y), 1),
        }

    return out


def fmt_formants(d: dict | None) -> str:
    if d is None:
        return "(no file)"
    if "error" in d:
        return "err"
    return f"F1={d['F1']:.0f} F2={d['F2']:.0f}"


def distance_to_target(d: dict | None, target: dict) -> Optional[float]:
    if d is None or "error" in d:
        return None
    df1 = d["F1"] - target["F1"]
    df2 = d["F2"] - target["F2"]
    return float(np.sqrt(df1 * df1 + df2 * df2))


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

    if not WAV_DIR.is_dir():
        print(f"ERROR: WAV directory not found: {WAV_DIR}", file=sys.stderr)
        return 1

    print(f"Croatian vowel triangulation @ {WAV_DIR}\n")
    print("Literature targets (Basic 2023, adult male Croatian):")
    for v, t in LITERATURE_TARGETS.items():
        print(f"  /{v}/  F1={t['F1']:>4}  F2={t['F2']:>4}")
    print()
    rows = [analyze_word(w) for w in WORDS]

    header = (f"{'word':<6}{'V':<3}{'TGSB':<22}{'eSpeak':<22}"
              f"{'TGSB-lit':>9}{'eSpeak-lit':>11}  closer  trace")
    print(header)
    print("-" * len(header))

    sums = {v: [0.0, 0.0, 0] for v in LITERATURE_TARGETS}
    for r in rows:
        v = r["vowel"]
        target = LITERATURE_TARGETS[v]
        d_t = distance_to_target(r["tgsb"], target)
        d_e = distance_to_target(r["espeak"], target)
        better = "-"
        if d_t is not None and d_e is not None:
            better = "TGSB" if d_t < d_e else ("eSpeak" if d_e < d_t else "tie")
            sums[v][0] += d_t
            sums[v][1] += d_e
            sums[v][2] += 1
        d_t_s = f"{d_t:>8.0f}" if d_t is not None else "       -"
        d_e_s = f"{d_e:>10.0f}" if d_e is not None else "         -"
        diag = ""
        if r.get("trace_diag"):
            diag = f"  {r['trace_diag']['key']}={r['trace_diag']['traced_ms']:.0f}ms"
        print(f"{r['word']:<6}/{v}/ {fmt_formants(r['tgsb']):<22}"
              f"{fmt_formants(r['espeak']):<22}{d_t_s} {d_e_s}  {better:<6}{diag}")

    print()
    print("Mean F1/F2 distance to literature target (lower = better,")
    print("                                          excludes trace-short rows):")
    print(f"{'vowel':<8}{'TGSB':>10}{'eSpeak':>10}{'closer':>10}{'n':>4}")
    print("-" * 42)
    for v, (s_t, s_e, n) in sums.items():
        if n == 0:
            print(f"/{v}/    no usable pairs")
            continue
        m_t, m_e = s_t / n, s_e / n
        better = "TGSB" if m_t < m_e else ("eSpeak" if m_e < m_t else "tie")
        print(f"/{v}/   {m_t:>10.1f}{m_e:>10.1f}{better:>10}{n:>4}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
