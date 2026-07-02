"""
Fingerprint the /s/->/z/ artifact in 29-Bloo's bug recording.

Discriminates three mechanism classes:
  (A) underrun SILENCE  -> periodic silence gaps chopping frication
  (B) underrun STUTTER/AM -> amplitude modulation at F0 (voicing-like buzz)
  (C) added VOICE BAR   -> continuous low-freq periodic energy in /s/, no gaps

Output: a few summary numbers + spectrogram PNGs (read visually).
"""
import sys
import os
import numpy as np

try:
    import librosa
    import librosa.display
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except Exception as e:
    print("IMPORT_FAIL:", e)
    sys.exit(1)

PATH = sys.argv[1] if len(sys.argv) > 1 else "bug_29bloo.mp3"
BASE = os.path.splitext(os.path.basename(PATH))[0]

y, sr = librosa.load(PATH, sr=None, mono=True)
dur = len(y) / sr
print(f"sr={sr} dur={dur:.2f}s n={len(y)}")

# Spectral rolloff to see if band-limited (11025 capture -> ~5.5k ceiling)
S = np.abs(librosa.stft(y, n_fft=2048, hop_length=512))
freqs = librosa.fft_frequencies(sr=sr, n_fft=2048)
roll = librosa.feature.spectral_rolloff(S=S, sr=sr, roll_percent=0.95).mean()
cent = librosa.feature.spectral_centroid(S=S, sr=sr).mean()
print(f"rolloff95={roll:.0f}Hz centroid={cent:.0f}Hz")

# ---- (A) silence-gap detection ----
# short RMS, gate at peak-35dB, find gaps longer than 12ms
hop = max(1, sr // 1000)            # ~1ms hop
frame = max(2, sr // 200)          # ~5ms frame
rms = librosa.feature.rms(y=y, frame_length=frame, hop_length=hop)[0]
rms_db = 20 * np.log10(rms + 1e-9)
peak_db = np.percentile(rms_db, 99)
gate = rms_db > (peak_db - 35)     # True = audible
# gaps = runs of False bounded by True (interior silences)
silent = ~gate
runs = []
i = 0
while i < len(silent):
    if silent[i]:
        j = i
        while j < len(silent) and silent[j]:
            j += 1
        runs.append((i, j))
        i = j
    else:
        i += 1
# interior gaps only (drop leading/trailing silence)
interior = [r for r in runs if r[0] > 0 and r[1] < len(silent)]
gap_ms = [ (b - a) * hop / sr * 1000 for a, b in interior ]
gap_ms = [g for g in gap_ms if g >= 12.0]
print(f"interior_gaps>=12ms={len(gap_ms)} total_gap_ms={sum(gap_ms):.0f}")
if gap_ms:
    print(f"gap_ms_sample={[round(g) for g in gap_ms[:12]]}")

# ---- (B) amplitude modulation at F0 (voicing-like buzz) ----
# envelope autocorrelation, look for a peak in 70-200 Hz
env = rms - rms.mean()
ac = np.correlate(env, env, mode="full")[len(env)-1:]
ac = ac / (ac[0] + 1e-12)
lo = int(sr / 200 / hop)           # 200 Hz
hi = int(sr / 70 / hop)            # 70 Hz
if hi > lo + 1 and hi < len(ac):
    seg = ac[lo:hi]
    k = np.argmax(seg) + lo
    am_hz = sr / (k * hop)
    am_strength = ac[k]
    print(f"AM_peak={am_hz:.0f}Hz strength={am_strength:.3f}")
else:
    print("AM_peak=NA")

# ---- (C) voice bar in /s/ frames ----
# /s/ frames = high-centroid frames; measure their <500Hz energy fraction
col_cent = librosa.feature.spectral_centroid(S=S, sr=sr)[0]
hf_cols = np.where(col_cent > max(2500, 0.5 * roll))[0]
lowmask = freqs < 500
if len(hf_cols) > 5:
    sub = S[:, hf_cols]
    lowfrac = (sub[lowmask, :].sum(0) / (sub.sum(0) + 1e-9))
    print(f"s_frames={len(hf_cols)} voicebar_lowfrac_med={np.median(lowfrac):.3f} "
          f"max={lowfrac.max():.3f}")
else:
    print(f"s_frames={len(hf_cols)} (too few for voicebar test)")

# ---- spectrogram PNGs ----
Sdb = librosa.amplitude_to_db(S, ref=np.max)
plt.figure(figsize=(16, 5))
librosa.display.specshow(Sdb, sr=sr, hop_length=512, x_axis="time",
                         y_axis="log", cmap="magma")
plt.colorbar(format="%+2.0f dB")
plt.title("bug recording — full")
plt.tight_layout()
plt.savefig(f"spec_full_{BASE}.png", dpi=90)
print(f"saved spec_full_{BASE}.png")

# zoom: first 6 seconds, linear freq up to 6k to see voice bars under /s/
n6 = min(len(y), int(6 * sr))
S2 = np.abs(librosa.stft(y[:n6], n_fft=1024, hop_length=128))
S2db = librosa.amplitude_to_db(S2, ref=np.max)
plt.figure(figsize=(16, 5))
librosa.display.specshow(S2db, sr=sr, hop_length=128, x_axis="time",
                         y_axis="linear", cmap="magma")
plt.ylim(0, 6000)
plt.colorbar(format="%+2.0f dB")
plt.title("bug recording — first 6s, 0-6kHz")
plt.tight_layout()
plt.savefig(f"spec_zoom_{BASE}.png", dpi=90)
print(f"saved spec_zoom_{BASE}.png")
