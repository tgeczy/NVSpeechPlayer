#!/usr/bin/env python3
"""
ipa_klatt_probe.py

Small helper for NV Speech Player tuning:
- Get IPA from eSpeak for a word/phrase
- Apply a few "pack-style" normalization tweaks (optional)
- Roughly synthesize the IPA as concatenated Klatt segments using klatt_tune_sim.py

This is NOT a full NVSpeechPlayer frontend emulation. It's a quick ear-test tool.
"""

import argparse
import re
import subprocess
import sys
import wave
import struct
from pathlib import Path

import numpy as np

# Import the existing single-phoneme simulator.
# Keep this script in the same folder as klatt_tune_sim.py.
import klatt_tune_sim as kts


TRANSPARENT = {"ˈ", "ˌ", "ː", "ˑ", ".", "‿", " " , "\t", "\n", "\r", "͡"}  # tie bar handled separately


def espeak_ipa(voice: str, text: str) -> str:
    cmd = ["espeak", "-q", "--ipa", "-v", voice, text]
    try:
        out = subprocess.check_output(cmd, text=True)
    except FileNotFoundError:
        raise RuntimeError("espeak not found on PATH")
    return out.strip()


def normalize_ipa(ipa: str, voice: str) -> str:
    """
    A *small* normalization layer that mirrors the specific pack tweaks
    we currently care about (dialog triphthong, fear/dear NEAR, Spanish yo).
    """
    s = ipa.strip()

    # Collapse multiple spaces (eSpeak sometimes prefixes a space)
    s = re.sub(r"\s+", " ", s).strip()

    # Spanish: neutralize y/ll onset.
    if voice.startswith("es"):
        # eSpeak uses ʝ for word-initial "y" (yo). Map it to glide j (neutral).
        s = s.replace("ʝ", "j")

    # English: dialog/fire triphthong-ish sequences
    if voice.startswith("en"):
        # tie PRICE before schwa: aɪə -> a͡ɪə
        s = s.replace("aɪə", "a͡ɪə")

        # Word-final NEAR in en-gb: iə -> i͡ɚ (thicker offglide)
        s = re.sub(r"iə(?=(\s|$))", "i͡ɚ", s)

        # Word-final rhotic NEAR in en-us/en-ca: ɪɹ -> iɹ
        s = re.sub(r"ɪɹ(?=(\s|$))", "iɹ", s)

    return s


def _build_tokenizer(phoneme_keys):
    # Sort by length desc for greedy match.
    keys = sorted(phoneme_keys, key=len, reverse=True)
    return keys


def tokenize_ipa(ipa: str, phoneme_keys):
    """
    Greedy tokenizer that recognizes:
    - known phoneme keys from phonemes.yaml
    - stress marks / length marks / tie bar as single-char tokens
    - spaces as word separators
    """
    keys = _build_tokenizer(phoneme_keys)
    s = ipa
    out = []
    i = 0
    while i < len(s):
        ch = s[i]
        if ch.isspace():
            out.append(" ")
            i += 1
            continue
        if ch in {"ˈ","ˌ","ː","ˑ",".","‿","͡"}:
            out.append(ch)
            i += 1
            continue

        matched = None
        for k in keys:
            if s.startswith(k, i):
                matched = k
                break
        if matched is None:
            # Unknown symbol; keep as-is (may be a combining diacritic etc)
            out.append(ch)
            i += 1
        else:
            out.append(matched)
            i += len(matched)
    # Clean duplicate spaces
    cleaned=[]
    for t in out:
        if t == " " and cleaned and cleaned[-1] == " ":
            continue
        cleaned.append(t)
    return cleaned


def _base_duration_s(props: dict) -> float:
    # Very rough defaults tuned for quick ear tests (16 kHz).
    # NVSpeechPlayer's real timing is more complex.
    if props.get("_isVowel"):
        return 0.115
    if props.get("_isStop"):
        return 0.055
    if props.get("_isSemivowel"):
        return 0.060
    if props.get("_isLiquid"):
        return 0.070
    if props.get("_isNasal"):
        return 0.070
    # fricatives/others
    return 0.070


def synth_phrase(tokens, phoneme_map, sample_rate=16000, f0=140.0, crossfade_ms=5.0):
    defaults = {"defaultPreFormantGain": 1.0, "defaultOutputGain": 1.5}

    segs = []
    stress = None
    tie_next = False
    pending_len = None

    # We'll apply length marks to the *previous* segment by stretching it after synthesis.
    # That keeps the logic simple.
    last_len_mark = None

    for idx, tok in enumerate(tokens):
        if tok == " ":
            # small word gap
            segs.append(np.zeros(int(sample_rate * 0.035), dtype=np.float32))
            continue
        if tok in {"ˈ","ˌ"}:
            stress = tok
            continue
        if tok == "͡":
            tie_next = True
            continue
        if tok in {"ː","ˑ"}:
            # mark length for previous synth segment
            last_len_mark = tok
            continue
        if tok in {".","‿"}:
            continue

        props = phoneme_map.get(tok)
        if props is None:
            # Unknown token: ignore but keep spacing sane.
            continue

        dur = _base_duration_s(props)

        # If tied to previous, treat as offglide: shorter.
        if tie_next:
            dur *= 0.38
            tie_next = False

        # Stress: slight duration & amplitude tweak.
        amp_scale = 1.0
        if stress == "ˈ":
            dur *= 1.12
            amp_scale = 1.08
        elif stress == "ˌ":
            dur *= 1.05
            amp_scale = 1.03
        stress = None

        frame = kts.build_frame_from_phoneme(props, f0=f0, defaults=defaults)
        # Nudge output gain for stress.
        frame.outputGain *= amp_scale

        wav = kts.synthesize(
            frame,
            duration_s=dur,
            sample_rate=sample_rate,
            model="engine",
            rosenberg_oq=0.4,
            rosenberg_sq=0.6,
        ).astype(np.float32)

        # Apply length mark by simple time-stretch (repeat samples) — crude but audible.
        if last_len_mark is not None:
            if last_len_mark == "ː":
                stretch = 1.55
            else:
                stretch = 1.20
            n = int(len(wav) * stretch)
            # linear resample
            x = np.linspace(0, 1, len(wav))
            xi = np.linspace(0, 1, n)
            wav = np.interp(xi, x, wav).astype(np.float32)
            last_len_mark = None

        segs.append(wav)

    # Concatenate with crossfade to avoid clicks
    if not segs:
        return np.zeros(0, dtype=np.float32)

    xf = int(sample_rate * (crossfade_ms / 1000.0))
    out = segs[0]
    for s in segs[1:]:
        if xf > 0 and len(out) > xf and len(s) > xf:
            a = out[-xf:]
            b = s[:xf]
            t = np.linspace(0, 1, xf, dtype=np.float32)
            mix = a * (1.0 - t) + b * t
            out = np.concatenate([out[:-xf], mix, s[xf:]])
        else:
            out = np.concatenate([out, s])
    return out


def write_wav(path: Path, audio: np.ndarray, sample_rate: int):
    # Normalize to int16 without clipping too hard
    if audio.size == 0:
        audio = np.zeros(1, dtype=np.float32)
    peak = float(np.max(np.abs(audio)))
    if peak < 1e-9:
        peak = 1.0
    audio = audio / peak * 0.85
    pcm = np.clip(audio, -1.0, 1.0)
    pcm16 = (pcm * 32767.0).astype(np.int16)

    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm16.tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--packs", required=True, help="Path to extracted packs folder (contains packs/phonemes.yaml)")
    ap.add_argument("--voice", default="en-gb", help="eSpeak voice, e.g. en-gb, en-us, es, es-mx")
    ap.add_argument("--text", help="Text to pass to espeak --ipa")
    ap.add_argument("--ipa", help="Provide IPA directly (skips espeak)")
    ap.add_argument("--no-normalize", action="store_true", help="Skip the small normalization layer")
    ap.add_argument("--out", help="Write synthesized wav to this path (e.g. out.wav)")
    ap.add_argument("--f0", type=float, default=140.0)
    ap.add_argument("--sr", type=int, default=16000)
    args = ap.parse_args()

    packs_path = Path(args.packs)
    phon_path = packs_path / "packs" / "phonemes.yaml"
    if not phon_path.exists():
        print(f"ERROR: {phon_path} not found. Pass --packs to your extracted pack root.", file=sys.stderr)
        sys.exit(2)

    phon_map = kts._parse_simple_yaml_map_of_maps(str(phon_path))
    keys = set(phon_map.keys())

    if args.ipa:
        ipa = args.ipa
    elif args.text:
        ipa = espeak_ipa(args.voice, args.text)
    else:
        print("ERROR: Provide --text or --ipa", file=sys.stderr)
        sys.exit(2)

    norm = ipa
    if not args.no_normalize:
        norm = normalize_ipa(ipa, args.voice)

    print("IPA (raw):      ", ipa)
    if not args.no_normalize:
        print("IPA (normalized):", norm)

    toks = tokenize_ipa(norm, keys)
    # show tokens without spaces spam
    pretty = " ".join([t for t in toks if t != " "])
    print("Tokens:", pretty)

    if args.out:
        audio = synth_phrase(toks, phon_map, sample_rate=args.sr, f0=args.f0)
        out_path = Path(args.out)
        write_wav(out_path, audio, args.sr)
        print(f"Wrote: {out_path}")

if __name__ == "__main__":
    main()
