#!/usr/bin/env python3
"""
formant_trajectory.py

Accurate simulation of NV Speech Player's frame manager and formant synthesis,
with visualization of formant trajectories over time.

This models:
- frame.cpp: Frame queuing, interpolation, fade logic, pitch ramping
- speechWaveGenerator.cpp: Synthesis chain (for optional audio output)

Usage:
  python formant_trajectory.py --packs /path/to/packs --text "hello world" --voice en-gb --out trajectory.png
  python formant_trajectory.py --packs /path/to/packs --ipa "həˈləʊ" --out trajectory.png --wav out.wav
"""

from __future__ import annotations

import argparse
import math
import subprocess
import wave
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Optional

import numpy as np

# Optional matplotlib for visualization
try:
    import matplotlib.pyplot as plt
    import matplotlib.patches as mpatches
    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False


# =============================================================================
# Frame structure (mirrors frame.h)
# =============================================================================

FRAME_PARAM_NAMES = [
    "voicePitch", "vibratoPitchOffset", "vibratoSpeed", "voiceTurbulenceAmplitude",
    "glottalOpenQuotient", "voiceAmplitude", "aspirationAmplitude",
    "cf1", "cf2", "cf3", "cf4", "cf5", "cf6", "cfN0", "cfNP",
    "cb1", "cb2", "cb3", "cb4", "cb5", "cb6", "cbN0", "cbNP",
    "caNP",
    "fricationAmplitude",
    "pf1", "pf2", "pf3", "pf4", "pf5", "pf6",
    "pb1", "pb2", "pb3", "pb4", "pb5", "pb6",
    "pa1", "pa2", "pa3", "pa4", "pa5", "pa6",
    "parallelBypass", "preFormantGain", "outputGain", "endVoicePitch",
]

FRAME_PARAM_COUNT = len(FRAME_PARAM_NAMES)


@dataclass
class Frame:
    """Mirrors speechPlayer_frame_t from frame.h"""
    voicePitch: float = 0.0
    vibratoPitchOffset: float = 0.0
    vibratoSpeed: float = 0.0
    voiceTurbulenceAmplitude: float = 0.0
    glottalOpenQuotient: float = 0.0
    voiceAmplitude: float = 0.0
    aspirationAmplitude: float = 0.0
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
    fricationAmplitude: float = 0.0
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
    preFormantGain: float = 0.0
    outputGain: float = 0.0
    endVoicePitch: float = 0.0

    def get_param(self, idx: int) -> float:
        return getattr(self, FRAME_PARAM_NAMES[idx])

    def set_param(self, idx: int, val: float):
        setattr(self, FRAME_PARAM_NAMES[idx], val)

    def copy(self) -> "Frame":
        f = Frame()
        for name in FRAME_PARAM_NAMES:
            setattr(f, name, getattr(self, name))
        return f

    def to_array(self) -> np.ndarray:
        return np.array([getattr(self, name) for name in FRAME_PARAM_NAMES], dtype=np.float64)

    @staticmethod
    def from_array(arr: np.ndarray) -> "Frame":
        f = Frame()
        for i, name in enumerate(FRAME_PARAM_NAMES):
            setattr(f, name, float(arr[i]))
        return f


# =============================================================================
# Frame Request (mirrors frameRequest_t from frame.cpp)
# =============================================================================

@dataclass
class FrameRequest:
    min_num_samples: int = 0
    num_fade_samples: int = 0
    is_null: bool = True
    frame: Frame = field(default_factory=Frame)
    voice_pitch_inc: float = 0.0
    user_index: int = -1
    label: str = ""  # For visualization


# =============================================================================
# Frame Manager (accurate port of frame.cpp)
# =============================================================================

def lerp(old: float, new: float, ratio: float) -> float:
    """calculateValueAtFadePosition from utils.h"""
    return old + (new - old) * ratio


class FrameManager:
    """
    Accurate Python port of FrameManagerImpl from frame.cpp.
    
    This handles:
    - Frame queuing with duration (minNumSamples) and fade (numFadeSamples)
    - Linear interpolation during fade transitions
    - Pitch ramping via voicePitchInc
    - NULL frame handling (silence)
    """
    
    def __init__(self):
        self.frame_queue: list[FrameRequest] = []
        self.old_request: FrameRequest = FrameRequest()
        self.new_request: Optional[FrameRequest] = None
        self.cur_frame: Frame = Frame()
        self.cur_frame_is_null: bool = True
        self.sample_counter: int = 0
        self.last_user_index: int = -1

    def queue_frame(
        self,
        frame: Optional[Frame],
        min_num_samples: int,
        num_fade_samples: int,
        user_index: int = -1,
        purge_queue: bool = False,
        label: str = "",
    ):
        """Queue a frame for synthesis."""
        req = FrameRequest()
        req.min_num_samples = min_num_samples
        req.num_fade_samples = num_fade_samples
        req.user_index = user_index
        req.label = label

        if frame is not None:
            req.is_null = False
            req.frame = frame.copy()
            if min_num_samples > 0:
                req.voice_pitch_inc = (frame.endVoicePitch - frame.voicePitch) / min_num_samples
            else:
                req.voice_pitch_inc = 0.0
        else:
            req.is_null = True
            req.frame = Frame()
            req.voice_pitch_inc = 0.0

        if purge_queue:
            self.frame_queue.clear()
            self.sample_counter = self.old_request.min_num_samples
            if not self.cur_frame_is_null:
                self.old_request.is_null = False
                self.old_request.frame = self.cur_frame.copy()
            if self.new_request is not None:
                self.new_request = None

        self.frame_queue.append(req)

    def get_current_frame(self) -> Optional[Frame]:
        """
        Advance one sample and return the current interpolated frame.
        Returns None if in silence.
        """
        self._update_current_frame()
        return None if self.cur_frame_is_null else self.cur_frame

    def _update_current_frame(self):
        self.sample_counter += 1

        if self.new_request is not None:
            # Currently fading between old and new
            if self.sample_counter > self.new_request.num_fade_samples:
                # Fade complete
                self.old_request = self.new_request
                self.new_request = None
                self.cur_frame = self.old_request.frame.copy()
            else:
                # Interpolate all parameters
                ratio = self.sample_counter / self.new_request.num_fade_samples
                for i in range(FRAME_PARAM_COUNT):
                    old_val = self.old_request.frame.get_param(i)
                    new_val = self.new_request.frame.get_param(i)
                    self.cur_frame.set_param(i, lerp(old_val, new_val, ratio))
        elif self.sample_counter > self.old_request.min_num_samples:
            # Time to move to next frame
            if self.frame_queue:
                was_from_silence = self.cur_frame_is_null or self.old_request.is_null
                self.cur_frame_is_null = False
                self.new_request = self.frame_queue.pop(0)

                if self.new_request.is_null:
                    # Transitioning to silence
                    self.new_request.frame = self.old_request.frame.copy()
                    self.new_request.frame.preFormantGain = 0.0
                    self.new_request.frame.voicePitch = self.cur_frame.voicePitch
                    self.new_request.voice_pitch_inc = 0.0
                elif self.old_request.is_null:
                    # Transitioning from silence
                    self.old_request.frame = self.new_request.frame.copy()
                    self.old_request.frame.preFormantGain = 0.0
                    self.old_request.is_null = False

                if self.new_request is not None:
                    if self.new_request.user_index != -1:
                        self.last_user_index = self.new_request.user_index
                    self.sample_counter = 0
                    if was_from_silence:
                        self.cur_frame = self.old_request.frame.copy()
                    # Apply pitch increment over fade
                    self.new_request.frame.voicePitch += (
                        self.new_request.voice_pitch_inc * self.new_request.num_fade_samples
                    )
            else:
                # No more frames - go to silence
                self.cur_frame_is_null = True
                self.old_request.is_null = True
        else:
            # Still within current frame - apply pitch ramping
            self.cur_frame.voicePitch += self.old_request.voice_pitch_inc
            self.old_request.frame.voicePitch = self.cur_frame.voicePitch


# =============================================================================
# Trajectory Recorder
# =============================================================================

@dataclass
class TrajectoryPoint:
    time_ms: float
    frame: Frame
    label: str
    is_silence: bool


class TrajectoryRecorder:
    """
    Wraps FrameManager to record formant trajectories at specified resolution.
    """

    def __init__(self, sample_rate: int = 16000, resolution_ms: float = 1.0):
        self.sample_rate = sample_rate
        self.resolution_ms = resolution_ms
        self.fm = FrameManager()
        self.points: list[TrajectoryPoint] = []

    def queue_frame(
        self,
        frame: Optional[Frame],
        duration_ms: float,
        fade_ms: float,
        label: str = "",
    ):
        """Queue a frame with timing in milliseconds."""
        min_samples = int(duration_ms * self.sample_rate / 1000.0)
        fade_samples = int(fade_ms * self.sample_rate / 1000.0)
        self.fm.queue_frame(frame, min_samples, fade_samples, label=label)

    def run(self) -> list[TrajectoryPoint]:
        """
        Run the frame manager and record trajectories.
        Returns list of TrajectoryPoint at the specified resolution.
        """
        self.points = []
        samples_per_point = int(self.resolution_ms * self.sample_rate / 1000.0)
        if samples_per_point < 1:
            samples_per_point = 1

        sample_idx = 0
        time_ms = 0.0

        # Keep running until we hit silence
        silence_count = 0
        max_silence = int(50 * self.sample_rate / 1000.0)  # 50ms of silence to stop

        while silence_count < max_silence:
            f = self.fm.get_current_frame()

            if f is None:
                silence_count += 1
            else:
                silence_count = 0

            # Record at resolution intervals
            if sample_idx % samples_per_point == 0:
                pt = TrajectoryPoint(
                    time_ms=time_ms,
                    frame=f.copy() if f else Frame(),
                    label=self.fm.old_request.label if self.fm.old_request else "",
                    is_silence=(f is None),
                )
                self.points.append(pt)

            sample_idx += 1
            time_ms = sample_idx * 1000.0 / self.sample_rate

        return self.points


# =============================================================================
# Synthesis (simplified, for audio preview)
# =============================================================================

class NoiseGenerator:
    def __init__(self, seed: int = 0):
        self.rng = np.random.RandomState(seed)
        self.last_value = 0.0

    def reset(self):
        self.last_value = 0.0

    def get_next(self) -> float:
        x = self.rng.random() - 0.5
        self.last_value = x + 0.75 * self.last_value
        return self.last_value


class FrequencyGenerator:
    def __init__(self, sample_rate: int):
        self.sample_rate = sample_rate
        self.last_cycle_pos = 0.0

    def reset(self):
        self.last_cycle_pos = 0.0

    def get_next(self, frequency: float) -> float:
        if frequency <= 0:
            return self.last_cycle_pos
        cycle_pos = ((frequency / self.sample_rate) + self.last_cycle_pos) % 1.0
        self.last_cycle_pos = cycle_pos
        return cycle_pos


class Resonator:
    def __init__(self, sample_rate: int, anti: bool = False):
        self.sample_rate = sample_rate
        self.anti = anti
        self.frequency = 0.0
        self.bandwidth = 0.0
        self.a = 0.0
        self.b = 0.0
        self.c = 0.0
        self.p1 = 0.0
        self.p2 = 0.0
        self.set_once = False

    def reset(self):
        self.p1 = 0.0
        self.p2 = 0.0
        self.set_once = False

    def set_params(self, frequency: float, bandwidth: float):
        if (not self.set_once) or (frequency != self.frequency) or (bandwidth != self.bandwidth):
            self.frequency = frequency
            self.bandwidth = bandwidth

            if bandwidth <= 0:
                bandwidth = 50.0

            r = math.exp(-math.pi / self.sample_rate * bandwidth)
            self.c = -(r * r)
            self.b = r * math.cos((2 * math.pi / self.sample_rate) * -frequency) * 2.0
            self.a = 1.0 - self.b - self.c

            if self.anti and frequency != 0:
                self.a = 1.0 / self.a
                self.c *= -self.a
                self.b *= -self.a

            self.set_once = True

    def resonate(self, inp: float, frequency: float, bandwidth: float) -> float:
        self.set_params(frequency, bandwidth)
        out = self.a * inp + self.b * self.p1 + self.c * self.p2
        self.p2 = self.p1
        self.p1 = inp if self.anti else out
        return out


class SimpleSynthesizer:
    """
    Simplified synthesizer for audio preview.
    Mirrors the essential parts of speechWaveGenerator.cpp.
    """

    # Tuning constants from speechWaveGenerator.cpp
    K_BASE_PEAK_POS = 0.91
    K_RADIATION_MIX = 1.0
    K_FRIC_NOISE_SCALE = 0.175

    def __init__(self, sample_rate: int = 16000):
        self.sample_rate = sample_rate
        self.pitch_gen = FrequencyGenerator(sample_rate)
        self.vibrato_gen = FrequencyGenerator(sample_rate)
        self.asp_gen = NoiseGenerator(0)
        self.fric_gen = NoiseGenerator(1)

        self.cascade = [Resonator(sample_rate) for _ in range(6)]
        self.nasal_zero = Resonator(sample_rate, anti=True)
        self.nasal_pole = Resonator(sample_rate)

        self.parallel = [Resonator(sample_rate) for _ in range(6)]

        self.last_flow = 0.0
        self.last_voiced_in = 0.0
        self.last_voiced_out = 0.0
        self.last_input = 0.0
        self.last_output = 0.0
        self.glottis_open = False

        self.smooth_pre_gain = 0.0
        attack_ms = 1.0
        release_ms = 0.5
        self.pre_gain_attack_alpha = 1.0 - math.exp(-1.0 / (sample_rate * attack_ms * 0.001))
        self.pre_gain_release_alpha = 1.0 - math.exp(-1.0 / (sample_rate * release_ms * 0.001))

    def reset(self):
        self.pitch_gen.reset()
        self.vibrato_gen.reset()
        self.asp_gen.reset()
        self.fric_gen.reset()
        for r in self.cascade:
            r.reset()
        self.nasal_zero.reset()
        self.nasal_pole.reset()
        for r in self.parallel:
            r.reset()
        self.last_flow = 0.0
        self.last_voiced_in = 0.0
        self.last_voiced_out = 0.0
        self.last_input = 0.0
        self.last_output = 0.0
        self.smooth_pre_gain = 0.0

    def generate_sample(self, f: Frame) -> float:
        """Generate one audio sample from a frame."""
        # Vibrato
        vibrato = (math.sin(self.vibrato_gen.get_next(f.vibratoSpeed) * 2 * math.pi) *
                   0.06 * f.vibratoPitchOffset) + 1.0
        pitch_hz = f.voicePitch * vibrato

        cycle_pos = self.pitch_gen.get_next(pitch_hz if pitch_hz > 0 else 0)

        aspiration = self.asp_gen.get_next() * 0.1

        # Glottal open quotient
        effective_oq = f.glottalOpenQuotient
        if effective_oq <= 0:
            effective_oq = 0.4
        effective_oq = max(0.10, min(0.95, effective_oq))

        self.glottis_open = (pitch_hz > 0) and (cycle_pos >= effective_oq)

        # Glottal flow
        flow = 0.0
        if self.glottis_open:
            open_len = 1.0 - effective_oq
            if open_len < 0.0001:
                open_len = 0.0001

            peak_pos = self.K_BASE_PEAK_POS
            dt = pitch_hz / self.sample_rate if pitch_hz > 0 else 0
            denom = max(0.0001, open_len - dt)
            phase = max(0.0, min(1.0, (cycle_pos - effective_oq) / denom))

            if phase < peak_pos:
                flow = 0.5 * (1.0 - math.cos(phase * math.pi / peak_pos))
            else:
                flow = 0.5 * (1.0 + math.cos((phase - peak_pos) * math.pi / (1.0 - peak_pos)))

        flow_scale = 1.6
        flow *= flow_scale

        d_flow = flow - self.last_flow
        self.last_flow = flow
        voiced_src = flow + d_flow * self.K_RADIATION_MIX

        # Turbulence
        turbulence = aspiration * f.voiceTurbulenceAmplitude
        if self.glottis_open:
            flow01 = max(0, min(1, flow / flow_scale))
            turbulence *= flow01 ** 1.5
        else:
            turbulence = 0.0

        voiced_in = (voiced_src + turbulence) * f.voiceAmplitude

        # DC blocker
        dc_pole = 0.9995
        voiced = voiced_in - self.last_voiced_in + dc_pole * self.last_voiced_out
        self.last_voiced_in = voiced_in
        self.last_voiced_out = voiced

        asp_out = aspiration * f.aspirationAmplitude

        # PreFormant gain smoothing
        target = f.preFormantGain
        alpha = self.pre_gain_attack_alpha if target > self.smooth_pre_gain else self.pre_gain_release_alpha
        self.smooth_pre_gain += (target - self.smooth_pre_gain) * alpha

        voice = asp_out + voiced

        # Cascade formants
        cascade_in = voice * self.smooth_pre_gain / 2.0
        n0_out = self.nasal_zero.resonate(cascade_in, f.cfN0, f.cbN0)
        np_out = self.nasal_pole.resonate(n0_out, f.cfNP, f.cbNP)
        cascade_out = lerp(cascade_in, np_out, f.caNP)

        cf = [f.cf1, f.cf2, f.cf3, f.cf4, f.cf5, f.cf6]
        cb = [f.cb1, f.cb2, f.cb3, f.cb4, f.cb5, f.cb6]
        for i in range(5, -1, -1):
            cascade_out = self.cascade[i].resonate(cascade_out, cf[i], cb[i])

        # Parallel formants (frication)
        fric = self.fric_gen.get_next() * self.K_FRIC_NOISE_SCALE * f.fricationAmplitude
        parallel_in = fric * self.smooth_pre_gain / 2.0

        pf = [f.pf1, f.pf2, f.pf3, f.pf4, f.pf5, f.pf6]
        pb = [f.pb1, f.pb2, f.pb3, f.pb4, f.pb5, f.pb6]
        pa = [f.pa1, f.pa2, f.pa3, f.pa4, f.pa5, f.pa6]
        parallel_out = 0.0
        for i in range(6):
            parallel_out += (self.parallel[i].resonate(parallel_in, pf[i], pb[i]) - parallel_in) * pa[i]
        parallel_out = lerp(parallel_out, parallel_in, f.parallelBypass)

        out = (cascade_out + parallel_out) * f.outputGain

        # Final DC blocker
        filtered = out - self.last_input + 0.9995 * self.last_output
        self.last_input = out
        self.last_output = filtered

        return filtered


def synthesize_from_trajectory(points: list[TrajectoryPoint], sample_rate: int = 16000) -> np.ndarray:
    """
    Synthesize audio from trajectory points.
    Note: This is approximate since we're synthesizing from sampled points.
    """
    synth = SimpleSynthesizer(sample_rate)

    # Calculate total samples
    if not points:
        return np.zeros(0, dtype=np.float32)

    # Points are at resolution intervals - expand to full sample rate
    resolution_ms = points[1].time_ms - points[0].time_ms if len(points) > 1 else 1.0
    samples_per_point = int(resolution_ms * sample_rate / 1000.0)

    total_samples = len(points) * samples_per_point
    audio = np.zeros(total_samples, dtype=np.float32)

    sample_idx = 0
    for i, pt in enumerate(points):
        if pt.is_silence:
            synth.reset()
            for _ in range(samples_per_point):
                if sample_idx < total_samples:
                    audio[sample_idx] = 0.0
                    sample_idx += 1
        else:
            # Interpolate between this point and next
            next_pt = points[i + 1] if i + 1 < len(points) else pt
            for j in range(samples_per_point):
                if sample_idx >= total_samples:
                    break
                ratio = j / samples_per_point
                # Interpolate frame
                cur_arr = pt.frame.to_array()
                next_arr = next_pt.frame.to_array() if not next_pt.is_silence else cur_arr
                interp_arr = cur_arr + (next_arr - cur_arr) * ratio
                interp_frame = Frame.from_array(interp_arr)

                audio[sample_idx] = synth.generate_sample(interp_frame)
                sample_idx += 1

    return audio


# =============================================================================
# Phoneme Loading and IPA Tokenization (borrowed from klatt_tune_sim.py)
# =============================================================================

def parse_phonemes_yaml(path: str) -> dict[str, dict[str, Any]]:
    """Parse phonemes.yaml (simplified YAML subset)."""
    text = Path(path).read_text(encoding="utf-8").splitlines()
    phonemes: dict[str, dict[str, Any]] = {}
    current_key: Optional[str] = None

    for raw in text:
        line = raw.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        indent = len(line) - len(line.lstrip(" "))
        s = line.strip()

        if indent == 0:
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
                    if len(valstr) >= 2 and valstr[0] == valstr[-1] and valstr[0] in ("'", '"'):
                        val = valstr[1:-1]
                    else:
                        val = valstr

            phonemes[current_key][field] = val

    return phonemes


TRANSPARENT_IPA = {"ˈ", "ˌ", "ː", "ˑ", ".", "‿", "͡", " ", "\t", "\n", "\r"}


def espeak_ipa(voice: str, text: str) -> str:
    cmd = ["espeak-ng", "-q", "--ipa", "-v", voice, text]
    try:
        out = subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL)
    except FileNotFoundError:
        # Try espeak if espeak-ng not found
        cmd[0] = "espeak"
        out = subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL)
    return out.strip()


def tokenize_ipa(ipa: str, phoneme_keys: set[str]) -> list[str]:
    """Greedy tokenizer for IPA string."""
    keys = sorted(phoneme_keys, key=len, reverse=True)
    out = []
    i = 0
    while i < len(ipa):
        ch = ipa[i]
        if ch.isspace():
            out.append(" ")
            i += 1
            continue
        if ch in TRANSPARENT_IPA:
            out.append(ch)
            i += 1
            continue

        matched = None
        for k in keys:
            if ipa.startswith(k, i):
                matched = k
                break
        if matched is None:
            out.append(ch)
            i += 1
        else:
            out.append(matched)
            i += len(matched)

    # Clean duplicate spaces
    cleaned = []
    for t in out:
        if t == " " and cleaned and cleaned[-1] == " ":
            continue
        cleaned.append(t)
    return cleaned


def build_frame_from_phoneme(
    props: dict[str, Any],
    f0: float = 140.0,
    pre_formant_gain: float = 1.0,
    output_gain: float = 1.5,
) -> Frame:
    """Build a Frame from phoneme properties."""
    def getf(name: str, default: float = 0.0) -> float:
        v = props.get(name, default)
        try:
            return float(v)
        except Exception:
            return default

    f = Frame()
    f.voicePitch = f0
    f.endVoicePitch = f0
    f.vibratoPitchOffset = getf("vibratoPitchOffset")
    f.vibratoSpeed = getf("vibratoSpeed")
    f.voiceTurbulenceAmplitude = getf("voiceTurbulenceAmplitude")
    f.glottalOpenQuotient = getf("glottalOpenQuotient")
    f.voiceAmplitude = getf("voiceAmplitude")
    f.aspirationAmplitude = getf("aspirationAmplitude")
    f.cf1 = getf("cf1")
    f.cf2 = getf("cf2")
    f.cf3 = getf("cf3")
    f.cf4 = getf("cf4")
    f.cf5 = getf("cf5")
    f.cf6 = getf("cf6")
    f.cfN0 = getf("cfN0")
    f.cfNP = getf("cfNP")
    f.cb1 = getf("cb1")
    f.cb2 = getf("cb2")
    f.cb3 = getf("cb3")
    f.cb4 = getf("cb4")
    f.cb5 = getf("cb5")
    f.cb6 = getf("cb6")
    f.cbN0 = getf("cbN0")
    f.cbNP = getf("cbNP")
    f.caNP = getf("caNP")
    f.fricationAmplitude = getf("fricationAmplitude")
    f.pf1 = getf("pf1")
    f.pf2 = getf("pf2")
    f.pf3 = getf("pf3")
    f.pf4 = getf("pf4")
    f.pf5 = getf("pf5")
    f.pf6 = getf("pf6")
    f.pb1 = getf("pb1")
    f.pb2 = getf("pb2")
    f.pb3 = getf("pb3")
    f.pb4 = getf("pb4")
    f.pb5 = getf("pb5")
    f.pb6 = getf("pb6")
    f.pa1 = getf("pa1")
    f.pa2 = getf("pa2")
    f.pa3 = getf("pa3")
    f.pa4 = getf("pa4")
    f.pa5 = getf("pa5")
    f.pa6 = getf("pa6")
    f.parallelBypass = getf("parallelBypass")
    f.preFormantGain = pre_formant_gain
    f.outputGain = output_gain
    return f


def get_phoneme_duration_ms(props: dict[str, Any], speed: float = 1.0) -> float:
    """Estimate phoneme duration based on type."""
    base = 90.0  # ms
    if props.get("_isVowel"):
        base = 115.0
    elif props.get("_isStop"):
        base = 55.0
    elif props.get("_isSemivowel"):
        base = 60.0
    elif props.get("_isLiquid"):
        base = 70.0
    elif props.get("_isNasal"):
        base = 70.0
    return base / speed


# =============================================================================
# Visualization
# =============================================================================

def plot_formant_trajectory(
    points: list[TrajectoryPoint],
    title: str = "Formant Trajectory",
    show_bandwidths: bool = False,
) -> Optional[Any]:
    """Plot F1, F2, F3 trajectories over time."""
    if not HAS_MATPLOTLIB:
        print("matplotlib not available for plotting")
        return None

    times = [p.time_ms for p in points]
    f1 = [p.frame.cf1 if not p.is_silence else 0 for p in points]
    f2 = [p.frame.cf2 if not p.is_silence else 0 for p in points]
    f3 = [p.frame.cf3 if not p.is_silence else 0 for p in points]
    voice_amp = [p.frame.voiceAmplitude if not p.is_silence else 0 for p in points]
    fric_amp = [p.frame.fricationAmplitude if not p.is_silence else 0 for p in points]

    fig, axes = plt.subplots(4, 1, figsize=(14, 10), sharex=True)

    # F1, F2, F3 trajectories
    ax = axes[0]
    ax.plot(times, f1, label="F1", color="#e74c3c", linewidth=2)
    ax.plot(times, f2, label="F2", color="#3498db", linewidth=2)
    ax.plot(times, f3, label="F3", color="#2ecc71", linewidth=2)
    ax.set_ylabel("Frequency (Hz)")
    ax.set_title(title)
    ax.legend(loc="upper right")
    ax.grid(True, alpha=0.3)
    ax.set_ylim(0, 4000)

    # Voice amplitude
    ax = axes[1]
    ax.fill_between(times, voice_amp, alpha=0.5, color="#9b59b6", label="Voice Amp")
    ax.set_ylabel("Voice Amplitude")
    ax.set_ylim(0, 1.2)
    ax.legend(loc="upper right")
    ax.grid(True, alpha=0.3)

    # Frication amplitude
    ax = axes[2]
    ax.fill_between(times, fric_amp, alpha=0.5, color="#e67e22", label="Fric Amp")
    ax.set_ylabel("Frication Amplitude")
    ax.set_ylim(0, 1.2)
    ax.legend(loc="upper right")
    ax.grid(True, alpha=0.3)

    # Pitch
    pitch = [p.frame.voicePitch if not p.is_silence else 0 for p in points]
    ax = axes[3]
    ax.plot(times, pitch, color="#1abc9c", linewidth=2, label="F0")
    ax.set_ylabel("Pitch (Hz)")
    ax.set_xlabel("Time (ms)")
    ax.legend(loc="upper right")
    ax.grid(True, alpha=0.3)

    # Add phoneme labels
    current_label = ""
    label_positions = []
    for i, p in enumerate(points):
        if p.label and p.label != current_label:
            label_positions.append((times[i], p.label))
            current_label = p.label

    for ax in axes:
        for t, lbl in label_positions:
            ax.axvline(x=t, color="gray", linestyle="--", alpha=0.5, linewidth=0.5)
        # Add labels on top axis only
        if ax == axes[0]:
            for t, lbl in label_positions:
                ax.annotate(lbl, (t, ax.get_ylim()[1]), fontsize=8, ha="left", va="top")

    plt.tight_layout()
    return fig


def plot_vowel_space(points: list[TrajectoryPoint], title: str = "Vowel Space (F1 × F2)") -> Optional[Any]:
    """Plot F1 vs F2 vowel quadrilateral."""
    if not HAS_MATPLOTLIB:
        print("matplotlib not available for plotting")
        return None

    fig, ax = plt.subplots(figsize=(10, 8))

    # Collect vowel-like points (high voice amplitude, low frication)
    vowel_points = []
    current_label = ""
    for p in points:
        if p.is_silence:
            continue
        if p.frame.voiceAmplitude > 0.5 and p.frame.fricationAmplitude < 0.3:
            if p.frame.cf1 > 100 and p.frame.cf2 > 100:
                if p.label != current_label:
                    vowel_points.append((p.frame.cf2, p.frame.cf1, p.label))
                    current_label = p.label

    # Plot
    for f2, f1, label in vowel_points:
        ax.scatter(f2, f1, s=150, alpha=0.7)
        ax.annotate(label, (f2, f1), fontsize=12, ha="left", va="bottom",
                   xytext=(5, 5), textcoords="offset points")

    # Draw trajectory lines
    if len(vowel_points) > 1:
        f2s = [vp[0] for vp in vowel_points]
        f1s = [vp[1] for vp in vowel_points]
        ax.plot(f2s, f1s, "k--", alpha=0.3, linewidth=1)

    # Invert axes (phonetic convention)
    ax.invert_xaxis()
    ax.invert_yaxis()

    ax.set_xlabel("F2 (Hz) ← front ... back →")
    ax.set_ylabel("F1 (Hz) ← close ... open →")
    ax.set_title(title)
    ax.grid(True, alpha=0.3)

    return fig


# =============================================================================
# Main pipeline
# =============================================================================

def process_ipa(
    ipa: str,
    phoneme_map: dict[str, dict[str, Any]],
    f0: float = 140.0,
    speed: float = 1.0,
    sample_rate: int = 16000,
    fade_ms: float = 10.0,
) -> tuple[list[TrajectoryPoint], list[str]]:
    """
    Convert IPA string to trajectory points.
    Returns (points, tokens).
    """
    tokens = tokenize_ipa(ipa, set(phoneme_map.keys()))

    recorder = TrajectoryRecorder(sample_rate=sample_rate, resolution_ms=0.5)

    stress = None
    tie_next = False
    lengthened = False

    for tok in tokens:
        if tok == " ":
            # Word gap - small silence
            recorder.queue_frame(None, duration_ms=35.0 / speed, fade_ms=5.0, label=" ")
            continue
        if tok in {"ˈ", "ˌ"}:
            stress = tok
            continue
        if tok == "͡":
            tie_next = True
            continue
        if tok in {"ː", "ˑ"}:
            lengthened = True
            continue
        if tok in {".", "‿"}:
            continue

        props = phoneme_map.get(tok)
        if props is None:
            # Unknown token
            continue

        dur = get_phoneme_duration_ms(props, speed)

        # Length mark
        if lengthened:
            dur *= 1.5
            lengthened = False

        # Tie (offglide)
        if tie_next:
            dur *= 0.4
            tie_next = False

        # Stress adjustment
        pitch = f0
        if stress == "ˈ":
            dur *= 1.1
            pitch *= 1.05
        elif stress == "ˌ":
            dur *= 1.05
            pitch *= 1.02
        stress = None

        frame = build_frame_from_phoneme(props, f0=pitch)
        frame.endVoicePitch = pitch  # Could add contour here

        recorder.queue_frame(frame, duration_ms=dur, fade_ms=fade_ms, label=tok)

    # Run and collect trajectory
    points = recorder.run()
    return points, tokens


def write_wav(path: Path, audio: np.ndarray, sample_rate: int):
    """Write audio to WAV file."""
    if audio.size == 0:
        audio = np.zeros(1, dtype=np.float32)

    peak = float(np.max(np.abs(audio)))
    if peak < 1e-9:
        peak = 1.0
    audio = audio / peak * 0.85

    # Scale to int16
    pcm = np.clip(audio * 32767.0, -32767.0, 32767.0).astype(np.int16)

    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm.tobytes())


# =============================================================================
# CLI
# =============================================================================

def main():
    ap = argparse.ArgumentParser(description="Formant Trajectory Visualizer for NV Speech Player")
    ap.add_argument("--packs", required=True, help="Path to packs folder (contains packs/phonemes.yaml)")
    ap.add_argument("--voice", default="en-gb", help="eSpeak voice for --text")
    ap.add_argument("--text", help="Text to convert via eSpeak")
    ap.add_argument("--ipa", help="IPA string directly")
    ap.add_argument("--f0", type=float, default=140.0, help="Base pitch in Hz")
    ap.add_argument("--speed", type=float, default=1.0, help="Speed multiplier")
    ap.add_argument("--sr", type=int, default=16000, help="Sample rate")
    ap.add_argument("--out", help="Output PNG path for trajectory plot")
    ap.add_argument("--vowel-space", help="Output PNG path for vowel space plot")
    ap.add_argument("--wav", help="Output WAV path for synthesized audio")
    ap.add_argument("--show", action="store_true", help="Show plots interactively")
    args = ap.parse_args()

    packs_path = Path(args.packs)
    phon_path = packs_path / "packs" / "phonemes.yaml"
    if not phon_path.exists():
        print(f"ERROR: {phon_path} not found")
        return 1

    phoneme_map = parse_phonemes_yaml(str(phon_path))
    print(f"Loaded {len(phoneme_map)} phonemes")

    # Get IPA
    if args.ipa:
        ipa = args.ipa
    elif args.text:
        ipa = espeak_ipa(args.voice, args.text)
    else:
        print("ERROR: Provide --text or --ipa")
        return 1

    print(f"IPA: {ipa}")

    # Process
    points, tokens = process_ipa(
        ipa, phoneme_map,
        f0=args.f0, speed=args.speed, sample_rate=args.sr
    )
    print(f"Tokens: {' '.join([t for t in tokens if t.strip()])}")
    print(f"Trajectory points: {len(points)}")
    print(f"Duration: {points[-1].time_ms:.1f} ms" if points else "0 ms")

    # Plot trajectory
    if args.out or args.show:
        fig = plot_formant_trajectory(points, title=f"Formant Trajectory: {ipa}")
        if fig:
            if args.out:
                fig.savefig(args.out, dpi=150, bbox_inches="tight")
                print(f"Saved trajectory: {args.out}")
            if args.show:
                plt.show()

    # Plot vowel space
    if args.vowel_space or args.show:
        fig = plot_vowel_space(points, title=f"Vowel Space: {ipa}")
        if fig:
            if args.vowel_space:
                fig.savefig(args.vowel_space, dpi=150, bbox_inches="tight")
                print(f"Saved vowel space: {args.vowel_space}")
            if args.show:
                plt.show()

    # Synthesize audio
    if args.wav:
        print("Synthesizing audio...")
        audio = synthesize_from_trajectory(points, sample_rate=args.sr)
        write_wav(Path(args.wav), audio, args.sr)
        print(f"Saved audio: {args.wav}")

    return 0


if __name__ == "__main__":
    exit(main())