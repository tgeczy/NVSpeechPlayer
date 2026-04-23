"""
Compare /ɡ/ closure + burst character across candidate WAV renders.

For issue #84/#95 verification: we have a "good" reference (hardG_gap8.wav)
and a "bad" reference (gEs_durationScale.wav, b2.wav). Does our new
b202_closureFix.wav sit with the good reference or the bad one?

Metrics computed per file:
  1. RMS envelope (10 ms windows) — so you can trace the energy profile
     through the word and see where the closure sits, how deep it goes,
     how sharp the burst is.
  2. Closure location and duration — the longest stretch below a silence
     threshold in the middle-third of the word (roughly where /ɡ/ lives
     in "entregado"). Duration is the main discriminator: gEs_durationScale
     produces no real closure; hardG_gap8 produces ~8 ms closure.
  3. Burst peak — the RMS maximum in the 30 ms after the closure ends.
     A real stop has a sharp energy spike; an approximant does not.
  4. Spectral COG (center of gravity, 500 Hz bands 0..5500) during the
     burst window. Different acoustic shape → different distribution.

Stdlib + numpy only (scipy unavailable on this Python install).
Output is pure text, suitable for a blind reader.
"""
import wave
import sys
import pathlib
import numpy as np


SAMPLE_RATE = 22050


def load_wav(path: pathlib.Path) -> np.ndarray:
    with wave.open(str(path), "rb") as w:
        n = w.getnframes()
        raw = w.readframes(n)
        sampwidth = w.getsampwidth()
        nchan = w.getnchannels()
        sr = w.getframerate()
    if sampwidth == 2:
        pcm = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    elif sampwidth == 4:
        pcm = np.frombuffer(raw, dtype=np.int32).astype(np.float32) / 2147483648.0
    else:
        raise RuntimeError(f"unsupported sampwidth {sampwidth} for {path}")
    if nchan == 2:
        pcm = pcm.reshape(-1, 2).mean(axis=1)
    assert sr == SAMPLE_RATE, f"expected {SAMPLE_RATE} Hz, got {sr}"
    return pcm


def rms_envelope(pcm: np.ndarray, window_ms: float = 10.0) -> np.ndarray:
    win = int(SAMPLE_RATE * window_ms / 1000)
    n_windows = len(pcm) // win
    out = np.zeros(n_windows)
    for i in range(n_windows):
        seg = pcm[i * win : (i + 1) * win]
        out[i] = np.sqrt(np.mean(seg ** 2))
    return out


def find_closure(envelope: np.ndarray, window_ms: float = 5.0,
                 search_start_pct: float = 0.35,
                 search_end_pct: float = 0.70) -> tuple[int, int, float]:
    """Find the closure region (low-energy dip) in the middle of the word.

    Stop closures aren't true silence — voice bar persists at low amplitude.
    Strategy: find the lowest-RMS window in the search range, then expand
    outward while RMS stays under 30% of the vowel-region median.

    Returns (start_window_idx, end_window_idx, duration_ms).
    """
    median_rms = float(np.median(envelope))
    expand_thresh = median_rms * 0.30

    lo = int(len(envelope) * search_start_pct)
    hi = int(len(envelope) * search_end_pct)
    if hi <= lo:
        return (-1, -1, 0.0)
    sub = envelope[lo:hi]
    min_idx_local = int(np.argmin(sub))
    min_idx = lo + min_idx_local

    # Expand left and right while envelope stays under expand_thresh.
    left = min_idx
    while left > 0 and envelope[left - 1] < expand_thresh:
        left -= 1
    right = min_idx
    while right < len(envelope) - 1 and envelope[right + 1] < expand_thresh:
        right += 1
    return (left, right + 1, (right + 1 - left) * window_ms)


def burst_peak(envelope: np.ndarray, closure_end: int,
               window_ms: float = 10.0, lookahead_ms: float = 30.0) -> tuple[int, float, float]:
    """Return (peak_idx, peak_rms, baseline_rms_before_closure). Peak is
    max RMS in lookahead_ms after closure; baseline is median RMS in the
    pre-closure vowel (30 ms before closure)."""
    if closure_end < 0 or closure_end >= len(envelope):
        return (-1, 0.0, 0.0)
    n_peak = max(1, int(lookahead_ms / window_ms))
    peak_region = envelope[closure_end : closure_end + n_peak]
    if peak_region.size == 0:
        return (-1, 0.0, 0.0)
    peak_idx_local = int(np.argmax(peak_region))
    peak_rms = float(peak_region[peak_idx_local])
    peak_idx = closure_end + peak_idx_local

    n_base = max(1, int(30.0 / window_ms))
    base_region = envelope[max(0, closure_end - 3 - n_base) : max(0, closure_end - 3)]
    baseline_rms = float(np.median(base_region)) if base_region.size else 0.0

    return (peak_idx, peak_rms, baseline_rms)


def spectral_cog(pcm: np.ndarray, start: int, length: int) -> float:
    """Spectral center of gravity in Hz, single 0..5500 Hz band."""
    seg = pcm[start : start + length]
    if seg.size < 128:
        return 0.0
    window = np.hanning(len(seg))
    fft = np.fft.rfft(seg * window)
    mag = np.abs(fft)
    freqs = np.fft.rfftfreq(len(seg), 1.0 / SAMPLE_RATE)
    mask = freqs <= 5500
    f = freqs[mask]
    m = mag[mask]
    if m.sum() == 0:
        return 0.0
    return float(np.sum(f * m) / np.sum(m))


def render_ascii_envelope(envelope: np.ndarray, width: int = 60, height: int = 8) -> str:
    """ASCII bar-plot of the envelope, one line per time bucket for a
    braille reader who wants to see the shape in text."""
    if len(envelope) == 0:
        return ""
    peak = envelope.max() or 1e-9
    norm = envelope / peak
    bucket = max(1, len(norm) // width)
    buckets = [norm[i * bucket : (i + 1) * bucket].max()
               for i in range(len(norm) // bucket)]
    lines = []
    for h in range(height, 0, -1):
        row = []
        for v in buckets:
            row.append("#" if v * height >= h else " ")
        lines.append("".join(row))
    return "\n".join(lines)


def analyze(path: pathlib.Path) -> dict:
    pcm = load_wav(path)
    dur_ms = len(pcm) * 1000.0 / SAMPLE_RATE
    env = rms_envelope(pcm, window_ms=5.0)
    c_start, c_end, c_dur_ms = find_closure(env, window_ms=5.0)
    b_idx, b_peak, b_base = burst_peak(env, c_end, window_ms=5.0,
                                       lookahead_ms=40.0)
    burst_sample = int(b_idx * 5.0 / 1000 * SAMPLE_RATE)
    burst_cog = spectral_cog(pcm, burst_sample,
                             int(0.020 * SAMPLE_RATE))

    # Closure depth: how far below the vowel-region median does the dip go?
    # A real stop closure drops ~75-95% (voice bar only); an approximant dip
    # is shallower (~10-30%). Stronger discriminator than dip duration.
    median_rms = float(np.median(env))
    if c_start >= 0 and c_end > c_start:
        closure_min_rms = float(np.min(env[c_start:c_end]))
    else:
        closure_min_rms = median_rms
    depth_pct = (1.0 - closure_min_rms / median_rms) * 100.0 if median_rms > 0 else 0.0
    depth_db = 20 * np.log10(closure_min_rms / median_rms) \
        if (median_rms > 0 and closure_min_rms > 0) else float("-inf")

    # Burst sharpness: rise from closure floor to burst peak in dB.
    # A real stop has a sharp release (10-25 dB rise within ~10-20 ms);
    # an approximant doesn't, because there was never a real closure.
    if b_peak > 0 and closure_min_rms > 0:
        burst_rise_db = 20 * np.log10(b_peak / closure_min_rms)
    else:
        burst_rise_db = float("nan")

    return {
        "path": path.name,
        "samples": len(pcm),
        "duration_ms": dur_ms,
        "closure_start_ms": c_start * 5.0 if c_start >= 0 else -1.0,
        "closure_end_ms": c_end * 5.0 if c_end >= 0 else -1.0,
        "closure_dur_ms": c_dur_ms,
        "closure_depth_pct": depth_pct,
        "closure_depth_db": depth_db,
        "burst_peak_ms": b_idx * 5.0,
        "burst_peak_rms": b_peak,
        "baseline_vowel_rms": b_base,
        "burst_vs_baseline_db": 20 * np.log10(b_peak / b_base)
            if b_base > 0 and b_peak > 0 else float("nan"),
        "burst_rise_db": burst_rise_db,
        "burst_cog_hz": burst_cog,
        "envelope_text": render_ascii_envelope(env, width=50, height=6),
    }


def main():
    sample_dir = pathlib.Path(__file__).parent.parent / "tests" / "wav_samples"
    candidates = [
        # Good reference — direct /ɡ/ input with global 8 ms closure.
        "test_output_entregado_hardG_gap8.wav",
        # Our new approach — /ɣ/→/ɡ_es/ via replacement, closureGapMs: 8.
        "test_output_entregado_b202_closureFix.wav",
        # Direct /ɡ/ input with Spanish default 30 ms closure.
        "test_output_entregado_b202_directG.wav",
        # Cluster-context audit (should now use plain /ɡ/, 30 ms closure).
        "test_output_algo_b202.wav",
        "test_output_largo_b202.wav",
        "test_output_rasgar_b202.wav",
        # Pure intervocalic audit (should still use /ɡ_es/, 8 ms closure).
        "test_output_lago_b202.wav",
        "test_output_amigo_b202.wav",
        # Bad references — what we're trying to escape.
        "test_output_entregado_gEs_durationScale.wav",
        "test_output_entregado_b2.wav",
        # For context.
        "test_output_entregado_hardG.wav",
    ]
    results = []
    for name in candidates:
        p = sample_dir / name
        if not p.exists():
            print(f"# MISSING: {name}")
            continue
        results.append(analyze(p))

    # Summary table.
    print("=" * 86)
    print("/ɡ/ CLOSURE + BURST ANALYSIS — issues #84/#95 verification")
    print("=" * 86)
    hdr = (f"{'file':<32} {'dur':>6} {'clos':>5} {'depth':>6} "
           f"{'depth':>7} {'burst':>7} {'COG':>5}")
    print(hdr)
    print(f"{'':<32} {'ms':>6} {'ms':>5} {'%':>6} "
          f"{'dB':>7} {'rise dB':>7} {'Hz':>5}")
    print("-" * 86)
    for r in results:
        short = r["path"].replace("test_output_entregado_", "").replace(".wav", "")
        print(f"{short:<32} "
              f"{r['duration_ms']:>6.1f} "
              f"{r['closure_dur_ms']:>5.1f} "
              f"{r['closure_depth_pct']:>5.1f}% "
              f"{r['closure_depth_db']:>+7.2f} "
              f"{r['burst_rise_db']:>+7.2f} "
              f"{r['burst_cog_hz']:>5.0f}")
    print()
    print("Reading guide:")
    print("  depth%   = how much RMS drops at the dip vs the word's median")
    print("             (real stop closure: 60-90%, approximant: 10-30%)")
    print("  depth dB = same in dB (real stop: -10 to -20 dB, approximant: -1 to -3 dB)")
    print("  burst dB = rise from closure floor to first post-closure peak")
    print("             (real stop: +10 to +20 dB, approximant: ~0 to +3 dB)")
    print()

    # Detailed per-file envelope shapes.
    for r in results:
        short = r["path"].replace("test_output_entregado_", "").replace(".wav", "")
        print(f"--- {short} ({r['duration_ms']:.1f} ms) ---")
        print(f"closure:  {r['closure_start_ms']:.1f}-{r['closure_end_ms']:.1f} ms "
              f"(dur {r['closure_dur_ms']:.1f} ms)")
        print(f"burst:    peak at {r['burst_peak_ms']:.1f} ms, "
              f"{r['burst_vs_baseline_db']:+.2f} dB vs pre-closure vowel")
        print(f"burst COG: {r['burst_cog_hz']:.0f} Hz")
        print(r["envelope_text"])
        print()


if __name__ == "__main__":
    main()
