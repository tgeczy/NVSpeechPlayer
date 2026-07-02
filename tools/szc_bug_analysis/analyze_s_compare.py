"""
Step 1 (#100 robustness sprint): compare eSpeak-NG vs TGSpeechBox Spanish
/s/ acoustically. The question is robustness, not prettiness: which /s/
survives a device noise-suppressor better?

Per word, isolate the /s/ frication frames (noise-like: high spectral
flatness, audible, brightish) and measure:
  s_to_vowel_db : /s/ RMS relative to the word's vowel RMS  (how duckable)
  centroid_Hz   : brightness of the /s/
  rolloff95_Hz  : where 95% of /s/ energy sits
  hf4k / hf6k   : fraction of /s/ energy above 4k / 6k Hz
  lowhi_ratio   : <500Hz / >2000Hz energy in /s/ frames (voiced residue)

Files: TGSB from android_probe/<tgsb>.wav, eSpeak from espeak_es/<esp>.wav.
"""
import numpy as np
import librosa

# (label, tgsb_basename, espeak_basename)
WORDS = [
    ("espanol",      "espanol",      "espanol"),
    ("restablecer",  "restablecer",  "restablecer"),
    ("seleccionado", "seleccionado", "seleccionado"),
    ("suspendido",   "suspendido",   "suspendido"),
    ("casa",         "casa_ctrl",    "casa"),
    ("espuela",      "espuela",      "espuela"),
]

NFFT, HOP = 1024, 128


def s_metrics(path):
    y, sr = librosa.load(path, sr=None, mono=True)
    if len(y) < NFFT:
        return None
    y = y / (np.max(np.abs(y)) + 1e-9)            # peak-normalize
    S = np.abs(librosa.stft(y, n_fft=NFFT, hop_length=HOP))
    freqs = librosa.fft_frequencies(sr=sr, n_fft=NFFT)
    flat = librosa.feature.spectral_flatness(S=S)[0]
    cent = librosa.feature.spectral_centroid(S=S, sr=sr)[0]
    rms = librosa.feature.rms(S=S, frame_length=NFFT)[0]
    peak = rms.max() + 1e-9

    audible = rms > 0.08 * peak
    # /s/ frames: noisy + bright + audible
    s_thr = np.percentile(flat[audible], 60) if audible.any() else 1.0
    s_mask = audible & (flat > s_thr) & (cent > 1800)
    # vowel frames: tonal + loud
    v_thr = np.percentile(flat[audible], 40) if audible.any() else 0.0
    v_mask = audible & (flat < v_thr) & (rms > 0.3 * peak)
    if s_mask.sum() < 2 or v_mask.sum() < 2:
        return None

    Ss = S[:, s_mask]
    s_rms = rms[s_mask].mean()
    v_rms = rms[v_mask].mean()
    s2v_db = 20 * np.log10(s_rms / (v_rms + 1e-9))
    centroid = cent[s_mask].mean()
    # rolloff95 on the /s/ frames' mean spectrum
    msp = Ss.mean(1)
    csum = np.cumsum(msp) / (msp.sum() + 1e-9)
    roll = freqs[np.searchsorted(csum, 0.95)]
    tot = Ss.sum() + 1e-9
    hf4 = Ss[freqs > 4000, :].sum() / tot
    hf6 = Ss[freqs > 6000, :].sum() / tot
    low = Ss[freqs < 500, :].sum()
    hi = Ss[freqs > 2000, :].sum() + 1e-9
    return dict(s2v=s2v_db, cent=centroid, roll=roll, hf4=hf4, hf6=hf6,
                lowhi=low / hi, n=int(s_mask.sum()), sr=sr)


hdr = f"{'word':13} {'eng':6} {'s2v_dB':>7} {'cent':>6} {'roll95':>7} {'hf>4k':>6} {'hf>6k':>6} {'lowhi':>6} {'n':>3}"
print(hdr)
print("-" * len(hdr))
for label, tg, es in WORDS:
    for eng, path in (("And", f"android_probe/{tg}.wav"),
                      ("Win", f"windows_probe/{tg}.wav"),
                      ("eSpk", f"espeak_es/{es}.wav")):
        try:
            m = s_metrics(path)
        except Exception as e:
            print(f"{label:13} {eng:6} ERR {e}")
            continue
        if m is None:
            print(f"{label:13} {eng:6} (no /s/ frames isolated)")
            continue
        print(f"{label:13} {eng:6} {m['s2v']:7.1f} {m['cent']:6.0f} "
              f"{m['roll']:7.0f} {m['hf4']:6.3f} {m['hf6']:6.3f} "
              f"{m['lowhi']:6.2f} {m['n']:3d}")
    print()
