#!/usr/bin/env python3
"""
klatt_tune_sim.py

Small, self-contained Klatt-style formant synth simulator (16 kHz) with:
- The "engine" glottal model (mirrors speechWaveGenerator.cpp in this repo)
- A Rosenberg-style glottal flow model (Oq + Sq) for comparison

Why this exists:
- You can A/B how phoneme-level params (formants, bandwidths, gains, glottalOpenQuotient, etc.)
  change spectral balance and perceived "harshness", without needing real-time playback.

Notes on glottalOpenQuotient in THIS engine:
- In the C++ voice generator, glottisOpen is true when cyclePos >= glottalOpenQuotient.
- That means the glottis is open for (1 - glottalOpenQuotient) of the cycle.
- So: engine_open_fraction ≈ (1 - glottalOpenQuotient).
  Example: glottalOpenQuotient=0.40 -> open ~60% of the cycle.

Dependencies:
- numpy (for FFT + arrays)
- No SciPy needed.

Example:
  python klatt_tune_sim.py --phonemes packs/phonemes.yaml --phoneme a --out out_a.wav --model engine
  python klatt_tune_sim.py --phonemes packs/phonemes.yaml --phoneme a --out out_a_ros.wav --model rosenberg --oq 0.62 --sq 1.2
"""

from __future__ import annotations

import argparse
import math
import random
import wave
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Any, Tuple, Optional

import numpy as np


# -----------------------------
# Minimal YAML (map-of-maps) loader that works with the original phonemes.yaml
# -----------------------------

def _parse_simple_yaml_map_of_maps(path: str) -> Dict[str, Dict[str, Any]]:
    """
    Parses the phonemes.yaml structure:
      phonemes:
        KEY:
          field: value
          ...
    It ignores comments and blank lines and only supports this subset.
    """
    text = Path(path).read_text(encoding="utf-8").splitlines()
    phonemes: Dict[str, Dict[str, Any]] = {}
    current_key: Optional[str] = None

    for raw in text:
        line = raw.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        indent = len(line) - len(line.lstrip(" "))
        s = line.strip()

        if indent == 0:
            # "phonemes:" or other top-level keys
            continue

        if indent == 2 and s.endswith(":"):
            key = s[:-1].strip()
            if len(key) >= 2 and key[0] == key[-1] and key[0] in ("'", '"'):
                key = key[1:-1]
            current_key = key
            phonemes[current_key] = {}
            continue

        if indent == 4 and ":" in s and current_key is not None:
            field, valstr = s.split(":", 1)
            field = field.strip()
            valstr = valstr.strip()

            if valstr.lower() == "true":
                val: Any = True
            elif valstr.lower() == "false":
                val = False
            else:
                try:
                    if valstr and all(c.isdigit() or c == "-" for c in valstr):
                        val = int(valstr)
                    else:
                        val = float(valstr)
                except Exception:
                    # string
                    if len(valstr) >= 2 and valstr[0] == valstr[-1] and valstr[0] in ("'", '"'):
                        val = valstr[1:-1]
                    else:
                        val = valstr

            phonemes[current_key][field] = val

    return phonemes


# -----------------------------
# Generators / filters (mirrors speechWaveGenerator.cpp closely)
# -----------------------------

class NoiseGenerator:
    def __init__(self, seed: int = 0):
        self.seed = seed
        self.rng = random.Random(seed)
        self.last_value = 0.0

    def reset(self) -> None:
        self.rng.seed(self.seed)
        self.last_value = 0.0

    def get_next(self) -> float:
        # colored-ish noise like the C++ code:
        x = self.rng.random() - 0.5  # [-0.5, +0.5)
        self.last_value = x + 0.75 * self.last_value
        return self.last_value


class FrequencyGenerator:
    def __init__(self, sample_rate: int):
        self.sample_rate = sample_rate
        self.last_cycle_pos = 0.0

    def reset(self) -> None:
        self.last_cycle_pos = 0.0

    def get_next(self, frequency_hz: float) -> float:
        cycle_pos = ((frequency_hz / self.sample_rate) + self.last_cycle_pos) % 1.0
        self.last_cycle_pos = cycle_pos
        return cycle_pos


class Resonator:
    def __init__(self, sample_rate: int, anti: bool = False):
        self.sample_rate = sample_rate
        self.anti = anti
        self.set_once = False
        self.p1 = 0.0
        self.p2 = 0.0
        self.frequency = 0.0
        self.bandwidth = 0.0
        self.a = 0.0
        self.b = 0.0
        self.c = 0.0

    def reset(self) -> None:
        self.p1 = 0.0
        self.p2 = 0.0
        self.set_once = False

    def set_params(self, frequency: float, bandwidth: float) -> None:
        if (not self.set_once) or (frequency != self.frequency) or (bandwidth != self.bandwidth):
            self.frequency = frequency
            self.bandwidth = bandwidth

            r = math.exp(-math.pi / self.sample_rate * bandwidth)
            self.c = -(r * r)
            self.b = r * math.cos((math.tau / self.sample_rate) * -frequency) * 2.0
            self.a = 1.0 - self.b - self.c

            if self.anti and frequency != 0:
                self.a = 1.0 / self.a
                self.c *= -self.a
                self.b *= -self.a

        self.set_once = True

    def resonate(self, inp: float, frequency: float, bandwidth: float, allow_update: bool = True) -> float:
        if allow_update:
            self.set_params(frequency, bandwidth)
        out = (self.a * inp) + (self.b * self.p1) + (self.c * self.p2)
        self.p2 = self.p1
        self.p1 = inp if self.anti else out
        return out


def lerp(old: float, new: float, ratio: float) -> float:
    return old + ((new - old) * ratio)


class CascadeFormantGenerator:
    def __init__(self, sample_rate: int):
        self.r1 = Resonator(sample_rate)
        self.r2 = Resonator(sample_rate)
        self.r3 = Resonator(sample_rate)
        self.r4 = Resonator(sample_rate)
        self.r5 = Resonator(sample_rate)
        self.r6 = Resonator(sample_rate)
        self.rN0 = Resonator(sample_rate, anti=True)
        self.rNP = Resonator(sample_rate)

    def reset(self) -> None:
        for r in (self.r1, self.r2, self.r3, self.r4, self.r5, self.r6, self.rN0, self.rNP):
            r.reset()

    def get_next(self, f: "Frame", glottis_open: bool, inp: float) -> float:
        _ = glottis_open  # unused in current C++
        inp /= 2.0
        n0 = self.rN0.resonate(inp, f.cfN0, f.cbN0)
        out = lerp(inp, self.rNP.resonate(n0, f.cfNP, f.cbNP), f.caNP)
        out = self.r6.resonate(out, f.cf6, f.cb6)
        out = self.r5.resonate(out, f.cf5, f.cb5)
        out = self.r4.resonate(out, f.cf4, f.cb4)
        out = self.r3.resonate(out, f.cf3, f.cb3)
        out = self.r2.resonate(out, f.cf2, f.cb2)
        out = self.r1.resonate(out, f.cf1, f.cb1)
        return out


class ParallelFormantGenerator:
    def __init__(self, sample_rate: int):
        self.r1 = Resonator(sample_rate)
        self.r2 = Resonator(sample_rate)
        self.r3 = Resonator(sample_rate)
        self.r4 = Resonator(sample_rate)
        self.r5 = Resonator(sample_rate)
        self.r6 = Resonator(sample_rate)

    def reset(self) -> None:
        for r in (self.r1, self.r2, self.r3, self.r4, self.r5, self.r6):
            r.reset()

    def get_next(self, f: "Frame", glottis_open: bool, inp: float) -> float:
        _ = glottis_open  # unused in current C++
        inp /= 2.0
        out = 0.0
        out += (self.r1.resonate(inp, f.pf1, f.pb1) - inp) * f.pa1
        out += (self.r2.resonate(inp, f.pf2, f.pb2) - inp) * f.pa2
        out += (self.r3.resonate(inp, f.pf3, f.pb3) - inp) * f.pa3
        out += (self.r4.resonate(inp, f.pf4, f.pb4) - inp) * f.pa4
        out += (self.r5.resonate(inp, f.pf5, f.pb5) - inp) * f.pa5
        out += (self.r6.resonate(inp, f.pf6, f.pb6) - inp) * f.pa6
        return lerp(out, inp, f.parallelBypass)


class HighShelf:
    def __init__(self, sample_rate: int, frequency: float = 2000.0, gain_db: float = 6.0, q: float = 0.7):
        self.sample_rate = sample_rate
        self.hsIn1 = 0.0
        self.hsIn2 = 0.0
        self.hsOut1 = 0.0
        self.hsOut2 = 0.0

        A = 10 ** (gain_db / 40.0)
        w0 = (math.tau * frequency) / sample_rate
        cosw0 = math.cos(w0)
        sinw0 = math.sin(w0)
        alpha = sinw0 / (2.0 * q)

        a0 = (A + 1) - (A - 1) * cosw0 + (2 * math.sqrt(A) * alpha)
        self.b0 = (A * ((A + 1) + (A - 1) * cosw0 + (2 * math.sqrt(A) * alpha))) / a0
        self.b1 = (-2 * A * ((A - 1) + (A + 1) * cosw0)) / a0
        self.b2 = (A * ((A + 1) + (A - 1) * cosw0 - (2 * math.sqrt(A) * alpha))) / a0
        self.a1 = (2 * ((A - 1) - (A + 1) * cosw0)) / a0
        self.a2 = ((A + 1) - (A - 1) * cosw0 - (2 * math.sqrt(A) * alpha)) / a0

    def reset(self) -> None:
        self.hsIn1 = self.hsIn2 = self.hsOut1 = self.hsOut2 = 0.0

    def apply(self, inp: float) -> float:
        out = (self.b0 * inp) + (self.b1 * self.hsIn1) + (self.b2 * self.hsIn2) - (self.a1 * self.hsOut1) - (self.a2 * self.hsOut2)
        self.hsIn2 = self.hsIn1
        self.hsIn1 = inp
        self.hsOut2 = self.hsOut1
        self.hsOut1 = out
        return out


@dataclass
class Frame:
    # Only fields we actually use in the wave generator:
    voicePitch: float = 120.0
    vibratoPitchOffset: float = 0.0
    vibratoSpeed: float = 0.0
    voiceTurbulenceAmplitude: float = 0.0
    glottalOpenQuotient: float = 0.0

    voiceAmplitude: float = 0.0
    aspirationAmplitude: float = 0.0
    fricationAmplitude: float = 0.0

    cf1: float = 0.0
    cf2: float = 0.0
    cf3: float = 0.0
    cf4: float = 0.0
    cf5: float = 0.0
    cf6: float = 0.0
    cfN0: float = 0.0
    cfNP: float = 0.0

    cb1: float = 0.0
    cb2: float = 0.0
    cb3: float = 0.0
    cb4: float = 0.0
    cb5: float = 0.0
    cb6: float = 0.0
    cbN0: float = 0.0
    cbNP: float = 0.0

    caNP: float = 0.0

    pf1: float = 0.0
    pf2: float = 0.0
    pf3: float = 0.0
    pf4: float = 0.0
    pf5: float = 0.0
    pf6: float = 0.0

    pb1: float = 0.0
    pb2: float = 0.0
    pb3: float = 0.0
    pb4: float = 0.0
    pb5: float = 0.0
    pb6: float = 0.0

    pa1: float = 0.0
    pa2: float = 0.0
    pa3: float = 0.0
    pa4: float = 0.0
    pa5: float = 0.0
    pa6: float = 0.0

    parallelBypass: float = 0.0

    preFormantGain: float = 2.0
    outputGain: float = 1.5


class EngineVoiceSource:
    """Mirrors the C++ VoiceGenerator."""
    def __init__(self, sample_rate: int, seed: int = 0):
        self.sample_rate = sample_rate
        self.pitch_gen = FrequencyGenerator(sample_rate)
        self.vibrato_gen = FrequencyGenerator(sample_rate)
        self.asp_gen = NoiseGenerator(seed=seed)
        self.last_flow = 0.0
        self.last_voiced_in = 0.0
        self.last_voiced_out = 0.0
        self.glottis_open = False

    def reset(self) -> None:
        self.pitch_gen.reset()
        self.vibrato_gen.reset()
        self.asp_gen.reset()
        self.last_flow = 0.0
        self.last_voiced_in = 0.0
        self.last_voiced_out = 0.0
        self.glottis_open = False

    def get_next(self, f: Frame) -> float:
        vibrato = (math.sin(self.vibrato_gen.get_next(f.vibratoSpeed) * math.tau) * 0.06 * f.vibratoPitchOffset) + 1.0
        pitch_hz = f.voicePitch * vibrato
        cycle_pos = self.pitch_gen.get_next(pitch_hz if pitch_hz > 0.0 else 0.0)

        aspiration = self.asp_gen.get_next() * 0.1

        effective = f.glottalOpenQuotient
        if effective <= 0.0:
            effective = 0.4
        effective = max(0.10, min(0.95, effective))

        self.glottis_open = (pitch_hz > 0.0) and (cycle_pos >= effective)

        flow = 0.0
        if self.glottis_open:
            open_len = 1.0 - effective
            open_len = max(0.0001, open_len)

            base_peak_pos = 0.90
            peak_pos = base_peak_pos

            dt = (pitch_hz / self.sample_rate) if pitch_hz > 0.0 else 0.0
            denom = max(0.0001, open_len - dt)
            phase = (cycle_pos - effective) / denom
            phase = max(0.0, min(1.0, phase))

            # ensure at least ~2 samples of closure
            min_close_samples = 2.0
            if pitch_hz > 0.0:
                period_samples = self.sample_rate / pitch_hz
                min_close_frac = min_close_samples / (period_samples * open_len)
                min_close_frac = min(0.5, min_close_frac)
                limit_peak_pos = 1.0 - min_close_frac
                peak_pos = max(0.50, min(peak_pos, limit_peak_pos))

            if phase < peak_pos:
                flow = 0.5 * (1.0 - math.cos(phase * math.pi / peak_pos))
            else:
                flow = 0.5 * (1.0 + math.cos((phase - peak_pos) * math.pi / (1.0 - peak_pos)))

        flow_scale = 1.6
        flow *= flow_scale

        dflow = flow - self.last_flow
        self.last_flow = flow

        radiation_mix = 1.0
        voiced_src = flow + (dflow * radiation_mix)

        turbulence = aspiration * f.voiceTurbulenceAmplitude
        if self.glottis_open:
            flow01 = max(0.0, min(1.0, flow / flow_scale))
            turbulence *= flow01
        else:
            turbulence = 0.0

        voiced_in = (voiced_src + turbulence) * f.voiceAmplitude

        # DC blocker
        dc_pole = 0.9995
        voiced = voiced_in - self.last_voiced_in + (dc_pole * self.last_voiced_out)
        self.last_voiced_in = voiced_in
        self.last_voiced_out = voiced

        asp_out = aspiration * f.aspirationAmplitude
        return asp_out + voiced


class RosenbergVoiceSource:
    """
    Rosenberg glottal flow model.

    Parameters:
      oq: open quotient (fraction of cycle the glottis is open), typical 0.5..0.75
      sq: speed quotient (opening time / closing time), typical ~0.8..1.6
    """
    def __init__(self, sample_rate: int, oq: float = 0.62, sq: float = 1.2, seed: int = 0):
        self.sample_rate = sample_rate
        self.oq = oq
        self.sq = sq
        self.pitch_gen = FrequencyGenerator(sample_rate)
        self.asp_gen = NoiseGenerator(seed=seed)
        self.last_flow = 0.0
        self.last_voiced_in = 0.0
        self.last_voiced_out = 0.0
        self.glottis_open = False

    def reset(self) -> None:
        self.pitch_gen.reset()
        self.asp_gen.reset()
        self.last_flow = 0.0
        self.last_voiced_in = 0.0
        self.last_voiced_out = 0.0
        self.glottis_open = False

    def _rosenberg_flow(self, phase: float) -> float:
        oq = max(0.10, min(0.95, self.oq))
        sq = max(0.20, self.sq)

        # split open phase into opening (Ta) and closing (Tc-Ta)
        Ta = (oq * (sq / (sq + 1.0)))
        Tc = oq  # end of open phase

        if phase < 0.0:
            return 0.0
        if phase < Ta:
            # opening: 0 -> 1
            if Ta < 1e-6:
                return 1.0
            return 0.5 * (1.0 - math.cos(math.pi * (phase / Ta)))
        if phase < Tc:
            # closing: 1 -> 0 (smooth)
            denom = max(1e-6, (Tc - Ta))
            x = (phase - Ta) / denom
            # cos from 1 to 0 as x goes 0..1
            return math.cos((math.pi / 2.0) * x)
        return 0.0

    def get_next(self, f: Frame) -> float:
        pitch_hz = f.voicePitch
        phase = self.pitch_gen.get_next(pitch_hz if pitch_hz > 0.0 else 0.0)

        aspiration = self.asp_gen.get_next() * 0.1

        flow = self._rosenberg_flow(phase)
        self.glottis_open = flow > 0.0

        flow_scale = 1.6
        flow *= flow_scale

        dflow = flow - self.last_flow
        self.last_flow = flow

        radiation_mix = 1.0
        voiced_src = flow + (dflow * radiation_mix)

        turbulence = aspiration * f.voiceTurbulenceAmplitude
        if self.glottis_open:
            flow01 = max(0.0, min(1.0, flow / flow_scale))
            turbulence *= flow01
        else:
            turbulence = 0.0

        voiced_in = (voiced_src + turbulence) * f.voiceAmplitude

        # DC blocker
        dc_pole = 0.9995
        voiced = voiced_in - self.last_voiced_in + (dc_pole * self.last_voiced_out)
        self.last_voiced_in = voiced_in
        self.last_voiced_out = voiced

        asp_out = aspiration * f.aspirationAmplitude
        return asp_out + voiced


def build_frame_from_phoneme(props: Dict[str, Any], f0: float, defaults: Dict[str, float]) -> Frame:
    def getf(name: str) -> float:
        v = props.get(name, defaults.get(name, 0.0))
        try:
            return float(v)
        except Exception:
            return defaults.get(name, 0.0)

    return Frame(
        voicePitch=f0,
        vibratoPitchOffset=getf("vibratoPitchOffset"),
        vibratoSpeed=getf("vibratoSpeed"),
        voiceTurbulenceAmplitude=getf("voiceTurbulenceAmplitude"),
        glottalOpenQuotient=getf("glottalOpenQuotient"),
        voiceAmplitude=getf("voiceAmplitude"),
        aspirationAmplitude=getf("aspirationAmplitude"),
        fricationAmplitude=getf("fricationAmplitude"),
        cf1=getf("cf1"), cf2=getf("cf2"), cf3=getf("cf3"), cf4=getf("cf4"), cf5=getf("cf5"), cf6=getf("cf6"),
        cfN0=getf("cfN0"), cfNP=getf("cfNP"),
        cb1=getf("cb1"), cb2=getf("cb2"), cb3=getf("cb3"), cb4=getf("cb4"), cb5=getf("cb5"), cb6=getf("cb6"),
        cbN0=getf("cbN0"), cbNP=getf("cbNP"),
        caNP=getf("caNP"),
        pf1=getf("pf1"), pf2=getf("pf2"), pf3=getf("pf3"), pf4=getf("pf4"), pf5=getf("pf5"), pf6=getf("pf6"),
        pb1=getf("pb1"), pb2=getf("pb2"), pb3=getf("pb3"), pb4=getf("pb4"), pb5=getf("pb5"), pb6=getf("pb6"),
        pa1=getf("pa1"), pa2=getf("pa2"), pa3=getf("pa3"), pa4=getf("pa4"), pa5=getf("pa5"), pa6=getf("pa6"),
        parallelBypass=getf("parallelBypass"),
        preFormantGain=getf("preFormantGain"),
        outputGain=getf("outputGain"),
    )


def synthesize(f: Frame, duration_s: float, sample_rate: int, model: str, rosenberg_oq: float, rosenberg_sq: float) -> np.ndarray:
    n = int(duration_s * sample_rate)

    # Sources
    if model == "rosenberg":
        voice = RosenbergVoiceSource(sample_rate, oq=rosenberg_oq, sq=rosenberg_sq, seed=0)
    else:
        voice = EngineVoiceSource(sample_rate, seed=0)

    fric = NoiseGenerator(seed=1)
    cascade = CascadeFormantGenerator(sample_rate)
    parallel = ParallelFormantGenerator(sample_rate)
    hs = HighShelf(sample_rate)

    last_in = 0.0
    last_out = 0.0

    smooth_pre = 0.0
    attack_ms = 1.0
    release_ms = 0.5
    attack_alpha = 1.0 - math.exp(-1.0 / (sample_rate * (attack_ms * 0.001)))
    release_alpha = 1.0 - math.exp(-1.0 / (sample_rate * (release_ms * 0.001)))

    out = np.zeros(n, dtype=np.float32)

    for i in range(n):
        # preFormant gain smoothing (same as C++)
        target = f.preFormantGain
        alpha = attack_alpha if target > smooth_pre else release_alpha
        smooth_pre += (target - smooth_pre) * alpha

        v = voice.get_next(f)
        casc = cascade.get_next(f, getattr(voice, "glottis_open", False), v * smooth_pre)
        fr = fric.get_next() * 0.175 * f.fricationAmplitude
        par = parallel.get_next(f, getattr(voice, "glottis_open", False), fr * smooth_pre)

        mixed = (casc + par) * f.outputGain

        # DC blocker
        filtered = mixed - last_in + (0.9995 * last_out)
        last_in = mixed
        last_out = filtered

        bright = hs.apply(filtered)
        out[i] = bright

    return out


def spectral_metrics(wave: np.ndarray, sample_rate: int) -> Dict[str, float]:
    # ignore the first half to reduce transient bias
    w = wave[len(wave) // 2 :].astype(np.float64)
    if len(w) < 32:
        return {"centroid_hz": 0.0}

    win = np.hanning(len(w))
    spec = np.fft.rfft(w * win)
    mag = np.abs(spec)
    freqs = np.fft.rfftfreq(len(w), 1.0 / sample_rate)

    p = mag ** 2
    total = float(np.sum(p)) or 1e-12
    centroid = float(np.sum(freqs * p) / total)

    def band(lo: float, hi: float) -> float:
        m = (freqs >= lo) & (freqs < hi)
        return float(np.sum(p[m]) / total)

    return {
        "centroid_hz": centroid,
        "band_0_1k": band(0.0, 1000.0),
        "band_1k_3k": band(1000.0, 3000.0),
        "band_3k_8k": band(3000.0, 8000.0),
        "peak_abs": float(np.max(np.abs(wave))),
    }


def write_wav(path: str, samples: np.ndarray, sample_rate: int) -> None:
    # Matches the C++ scaling: int16 = clamp(sample * 5000)
    scaled = np.clip(samples.astype(np.float64) * 5000.0, -32767.0, 32767.0).astype(np.int16)

    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)

    with wave.open(str(p), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(scaled.tobytes())


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--phonemes", required=True, help="Path to packs/phonemes.yaml")
    ap.add_argument("--phoneme", required=True, help="Phoneme key to synthesize (e.g. a, ʃ, t͡s)")
    ap.add_argument("--out", required=False, help="Output wav path")
    ap.add_argument("--model", choices=["engine", "rosenberg"], default="engine")
    ap.add_argument("--sr", type=int, default=16000)
    ap.add_argument("--f0", type=float, default=120.0)
    ap.add_argument("--dur", type=float, default=0.25)

    # Rosenberg params
    ap.add_argument("--oq", type=float, default=0.62, help="Rosenberg open quotient (only for --model rosenberg)")
    ap.add_argument("--sq", type=float, default=1.2, help="Rosenberg speed quotient (only for --model rosenberg)")

    args = ap.parse_args()

    phonemes = _parse_simple_yaml_map_of_maps(args.phonemes)
    if args.phoneme not in phonemes:
        keys = sorted(phonemes.keys())
        raise SystemExit(f"Unknown phoneme '{args.phoneme}'. Available count={len(keys)}")

    defaults = {
        "vibratoPitchOffset": 0.0,
        "vibratoSpeed": 0.0,
        "voiceTurbulenceAmplitude": 0.0,
        "glottalOpenQuotient": 0.0,  # 0 -> engine default (0.4 threshold)
        "preFormantGain": 2.0,
        "outputGain": 1.5,
    }

    frame = build_frame_from_phoneme(phonemes[args.phoneme], f0=args.f0, defaults=defaults)
    wav = synthesize(frame, duration_s=args.dur, sample_rate=args.sr, model=args.model, rosenberg_oq=args.oq, rosenberg_sq=args.sq)

    m = spectral_metrics(wav, args.sr)
    print(f"phoneme={args.phoneme} model={args.model} sr={args.sr} f0={args.f0} dur={args.dur}")
    print("metrics:")
    for k, v in m.items():
        print(f"  {k}: {v:.6g}")

    if args.out:
        write_wav(args.out, wav, args.sr)
        print(f"wrote: {args.out}")


if __name__ == "__main__":
    main()
