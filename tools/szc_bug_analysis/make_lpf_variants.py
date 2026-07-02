"""
Simulate the tester-device HF-suppression on CLEAN engine renders.
If low-passing our clean /s/ produces a /z/-like percept, the bug
mechanism is HF frication loss in the playback path (not the engine).

Emits, per word: _orig, _lp3500, _lp2500 (all peak-normalized so the
percept change isn't just loudness).
"""
import sys
import numpy as np
import soundfile as sf
from scipy.signal import butter, sosfilt

words = sys.argv[1:] or ["espanol", "restablecer"]


def peak_norm(x, db=-1.0):
    p = np.max(np.abs(x)) + 1e-9
    return x / p * (10 ** (db / 20.0))


for w in words:
    y, sr = sf.read(f"android_probe/{w}.wav")
    if y.ndim > 1:
        y = y.mean(1)
    y = y.astype(np.float64)
    sf.write(f"{w}_orig.wav", peak_norm(y), sr, subtype="PCM_16")
    for fc in (3500, 2500):
        sos = butter(6, fc, btype="low", fs=sr, output="sos")
        yl = sosfilt(sos, y)
        sf.write(f"{w}_lp{fc}.wav", peak_norm(yl), sr, subtype="PCM_16")
    print(f"{w}: sr={sr} wrote orig, lp3500, lp2500")
