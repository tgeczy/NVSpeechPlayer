"""
Step 2: prototype Spanish /s/ robustness variants on a temp pack copy,
render via the validated desktop harness, measure /s/ vs eSpeak targets.

Targets (eSpeak): cent ~4200-5000, hf6 ~0.33-0.36, lowhi ~0.08-0.20.
Current TGSB:     cent ~3100-3900, hf6 ~0.18-0.24, lowhi ~0.34-0.58.
"""
import os
import re
import shutil
import tempfile
import numpy as np
import librosa
from render_tgsb import Renderer, get_ipa

NFFT, HOP = 1024, 128


def s_metrics(path):
    y, sr = librosa.load(path, sr=None, mono=True)
    if len(y) < NFFT:
        return None
    y = y / (np.max(np.abs(y)) + 1e-9)
    S = np.abs(librosa.stft(y, n_fft=NFFT, hop_length=HOP))
    freqs = librosa.fft_frequencies(sr=sr, n_fft=NFFT)
    flat = librosa.feature.spectral_flatness(S=S)[0]
    cent = librosa.feature.spectral_centroid(S=S, sr=sr)[0]
    rms = librosa.feature.rms(S=S, frame_length=NFFT)[0]
    peak = rms.max() + 1e-9
    audible = rms > 0.08 * peak
    s_thr = np.percentile(flat[audible], 60) if audible.any() else 1.0
    s_mask = audible & (flat > s_thr) & (cent > 1800)
    v_thr = np.percentile(flat[audible], 40) if audible.any() else 0.0
    v_mask = audible & (flat < v_thr) & (rms > 0.3 * peak)
    if s_mask.sum() < 2 or v_mask.sum() < 2:
        return None
    Ss = S[:, s_mask]
    s2v = 20 * np.log10(rms[s_mask].mean() / (rms[v_mask].mean() + 1e-9))
    centroid = cent[s_mask].mean()
    tot = Ss.sum() + 1e-9
    hf4 = Ss[freqs > 4000, :].sum() / tot
    hf6 = Ss[freqs > 6000, :].sum() / tot
    lowhi = Ss[freqs < 500, :].sum() / (Ss[freqs > 2000, :].sum() + 1e-9)
    return dict(s2v=s2v, cent=centroid, hf4=hf4, hf6=hf6, lowhi=lowhi)


def write_variant(src, dst, edits):
    cur = None
    out = []
    with open(src, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^  (\S+):\s*$", line)
            if m:
                cur = m.group(1)
            else:
                m2 = re.match(r"^    (\w+):", line)
                if m2 and cur in edits and m2.group(1) in edits[cur]:
                    line = f"    {m2.group(1)}: {edits[cur][m2.group(1)]}\n"
            out.append(line)
    with open(dst, "w", encoding="utf-8") as f:
        f.writelines(out)


def apply3(d):
    return {k: dict(d) for k in ("s", "s_es", "s_mx")}


PACKS = r"C:\git\TGSpeechBox\packs"
tmp = tempfile.mkdtemp(prefix="tgsbvar_")
packdir = os.path.join(tmp, "packs")
shutil.copytree(PACKS, packdir)
srcyaml = os.path.join(PACKS, "phonemes.yaml")
dstyaml = os.path.join(packdir, "phonemes.yaml")

IPA = {
    "espanol": get_ipa("espanol", "es-419"),
    "casa_ctrl": get_ipa("casa", "es-419"),
    "suspendido": get_ipa("suspendido", "es-419"),
}

variants = [
    ("baseline", {}),
    ("pf6_6500", apply3({"pf6": 6500, "pb6": 2000, "pa6": 0.9})),
    ("pf6_7000", apply3({"pf6": 7000, "pb6": 2200, "pa6": 0.95})),
    ("pf56", apply3({"pf5": 5000, "pf6": 6800, "pa5": 0.6, "pa6": 0.95,
                     "pb5": 600, "pb6": 2200})),
    ("pf56_hot", apply3({"pf5": 5200, "pf6": 7200, "pa5": 0.7, "pa6": 1.0,
                         "pb5": 650, "pb6": 2400})),
]

print(f"{'variant':10} {'word':12} {'cent':>5} {'hf4':>5} {'hf6':>5} {'lowhi':>6} {'s2v':>6}")
print("-" * 60)
for name, edits in variants:
    write_variant(srcyaml, dstyaml, edits)
    r = Renderer(pack_dir=packdir, lang="es-mx")
    od = os.path.join(tmp, f"var_{name}")
    os.makedirs(od, exist_ok=True)
    for w, ipa in IPA.items():
        out = os.path.join(od, w + ".wav")
        r.render(ipa, out)
        m = s_metrics(out)
        if m:
            print(f"{name:10} {w:12} {m['cent']:5.0f} {m['hf4']:5.2f} "
                  f"{m['hf6']:5.2f} {m['lowhi']:6.2f} {m['s2v']:6.1f}")
        else:
            print(f"{name:10} {w:12} no-s")
    print()

print("TARGET eSpk:  espanol 4968/.36/.08 | casa 4508/.36/.08 | suspendido 4191/.36/.19  (cent/hf6/lowhi)")
print(f"variant WAVs under: {tmp}")
