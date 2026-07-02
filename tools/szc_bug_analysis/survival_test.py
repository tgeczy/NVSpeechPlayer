"""
Survival test: apply the device's HF-loss (duck everything >3.5kHz by -15dB)
to BOTH baseline and fix renders. If the fix retains more /s/ identity
(higher centroid/hf6, lower lowhi residue) after the same loss, it survives
the device effect where baseline collapses toward /z/.

Writes surv_*_<word>.wav for ear-test.
"""
import numpy as np
import librosa
import soundfile as sf

NFFT, HOP = 1024, 128


def metrics(y, sr):
    y = y / (np.max(np.abs(y)) + 1e-9)
    S = np.abs(librosa.stft(y, n_fft=NFFT, hop_length=HOP))
    freqs = librosa.fft_frequencies(sr=sr, n_fft=NFFT)
    flat = librosa.feature.spectral_flatness(S=S)[0]
    cent = librosa.feature.spectral_centroid(S=S, sr=sr)[0]
    rms = librosa.feature.rms(S=S, frame_length=NFFT)[0]
    peak = rms.max() + 1e-9
    aud = rms > 0.08 * peak
    sth = np.percentile(flat[aud], 60) if aud.any() else 1.0
    sm = aud & (flat > sth) & (cent > 1800)
    if sm.sum() < 2:
        return None
    Ss = S[:, sm]
    tot = Ss.sum() + 1e-9
    return dict(cent=cent[sm].mean(),
                hf6=Ss[freqs > 6000, :].sum() / tot,
                lowhi=Ss[freqs < 500, :].sum() / (Ss[freqs > 2000, :].sum() + 1e-9))


def duck(y, sr, fc=3500, gain_db=-15.0):
    S = librosa.stft(y, n_fft=NFFT, hop_length=HOP)
    freqs = librosa.fft_frequencies(sr=sr, n_fft=NFFT)
    g = np.ones(S.shape[0])
    g[freqs > fc] = 10 ** (gain_db / 20.0)
    return librosa.istft(S * g[:, None], hop_length=HOP)


WORDS = ["espanol", "restablecer", "casa_ctrl", "suspendido"]
SRC = {"baseline": "windows_probe", "fix": "windows_probe_v1"}

print(f"{'word':12} {'cond':14} {'cent':>5} {'hf6':>5} {'lowhi':>6}")
print("-" * 46)
for w in WORDS:
    for cond, d in SRC.items():
        y, sr = librosa.load(f"{d}/{w}.wav", sr=None, mono=True)
        m = metrics(y, sr)
        print(f"{w:12} {cond+'_orig':14} {m['cent']:5.0f} {m['hf6']:5.2f} {m['lowhi']:6.2f}")
    for cond, d in SRC.items():
        y, sr = librosa.load(f"{d}/{w}.wav", sr=None, mono=True)
        yd = duck(y, sr)
        m = metrics(yd, sr)
        yd = yd / (np.max(np.abs(yd)) + 1e-9) * 0.9
        sf.write(f"surv_{cond}_{w}.wav", yd.astype("float32"), sr, subtype="PCM_16")
        print(f"{w:12} {cond+'_DUCK':14} {m['cent']:5.0f} {m['hf6']:5.2f} {m['lowhi']:6.2f}")
    print()
