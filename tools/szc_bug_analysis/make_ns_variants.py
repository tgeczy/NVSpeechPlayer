"""
Simulate a device-side NOISE SUPPRESSOR (voice-isolation DSP) on clean
renders. Unlike a flat low-pass, this targets the aperiodic frication
(/s/, /f/) while leaving harmonic vowels intact — the selective process
that would turn /s/ -> /z/ without dulling the whole utterance.

Uses HPSS (harmonic/percussive/residual separation). With a wide margin,
frication noise falls into the discarded residual, so reconstructing from
the harmonic part suppresses /s/ specifically.

Per word: _ns_orig, _ns_mild (keep 30% non-harmonic), _ns_strong (harmonic only).
"""
import sys
import numpy as np
import soundfile as sf
import librosa

words = sys.argv[1:] or ["espanol", "restablecer"]


def pn(x, db=-1.0):
    p = np.max(np.abs(x)) + 1e-9
    return (x / p * (10 ** (db / 20.0))).astype(np.float32)


for w in words:
    y, sr = sf.read(f"android_probe/{w}.wav")
    if y.ndim > 1:
        y = y.mean(1)
    y = y.astype(np.float64)
    H, P = librosa.effects.hpss(y, margin=3.0)   # wide margin -> noise to residual
    sf.write(f"{w}_ns_orig.wav",   pn(y),         sr, subtype="PCM_16")
    sf.write(f"{w}_ns_mild.wav",   pn(H + 0.3 * P), sr, subtype="PCM_16")
    sf.write(f"{w}_ns_strong.wav", pn(H),         sr, subtype="PCM_16")
    print(f"{w}: wrote ns_orig, ns_mild, ns_strong")
