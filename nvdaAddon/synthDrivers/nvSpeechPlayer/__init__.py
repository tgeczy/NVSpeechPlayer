# -*- coding: utf-8 -*-
"""NV Speech Player - NVDA synth driver (modernized)

Pipeline:
- eSpeak (NVDA built-in) for text -> IPA/phonemes
- nvspFrontend.dll for IPA -> timed SpeechPlayer frames
- speechPlayer.dll for frame -> PCM synthesis
"""

from __future__ import annotations

import array
import ctypes
import math
import os
import queue
import re
import threading
import weakref
from collections import OrderedDict

from typing import Optional

import config
import nvwave

from logHandler import log
from synthDrivers import _espeak
from synthDriverHandler import SynthDriver, VoiceInfo, synthDoneSpeaking, synthIndexReached

# NVDA command classes moved around across versions; keep imports tolerant.
try:
    from speech.commands import IndexCommand, PitchCommand
except Exception:
    try:
        from speech.commands import IndexCommand  # type: ignore
        PitchCommand = None  # type: ignore
    except Exception:
        import speech  # fallback
        IndexCommand = getattr(speech, "IndexCommand", None)
        PitchCommand = getattr(speech, "PitchCommand", None)

from autoSettingsUtils.driverSetting import DriverSetting, NumericDriverSetting

# BooleanDriverSetting exists in modern NVDA, but keep a safe fallback for older builds.
try:
    from autoSettingsUtils.driverSetting import BooleanDriverSetting  # type: ignore
except Exception:  # pragma: no cover
    BooleanDriverSetting = None  # type: ignore

from . import speechPlayer
from ._dll_utils import findDllDir
from ._frontend import NvspFrontend

# --- Frontend DLL (IPA -> Frames) ---
# The frontend reads YAML packs from a local "packs" folder.
# DLL directory selection and frontend wrapper live in dedicated modules.



# Split on punctuation+space for clause pauses
re_textPause = re.compile(r"(?<=[.?!,:;])\s", re.DOTALL | re.UNICODE)

# Normalize whitespace before feeding eSpeak
_re_lineBreaks = re.compile(r"[\r\n\u2028\u2029]+", re.UNICODE)
_re_spaceRuns = re.compile(r"[\t \u00A0]+", re.UNICODE)


def _normalizeTextForEspeak(text: str) -> str:
    if not text:
        return ""
    # Convert newlines to spaces so line wrapping doesn't introduce pauses.
    text = _re_lineBreaks.sub(" ", text)
    # Collapse other common whitespace runs.
    text = _re_spaceRuns.sub(" ", text)
    return text.strip()


# Say All coalescing: delay/coalesce line breaks so eSpeak gets more context
_COALESCE_MAX_CHARS = 900
_COALESCE_MAX_INDEXES = 48
_SENT_END_RE = re.compile(r"(?:[.!?]+|\.{3})[)\]\"']*\s*$")


def _looksLikeSentenceEnd(s: str) -> bool:
    if not s:
        return False
    return bool(_SENT_END_RE.search(s.strip()))

# Language choices exposed in NVDA settings.
languages = OrderedDict([
    ("en-us", VoiceInfo("en-us", "English (US)")),
    ("en", VoiceInfo("en", "English (UK)")),
    ("zh", VoiceInfo("zh", "Chinese")),
    ("pt", VoiceInfo("pt", "Portuguese")),
    ("hu", VoiceInfo("hu", "Hungarian")),
    ("fi", VoiceInfo("fi", "Finnish")),
    ("bg", VoiceInfo("bg", "Bulgarian")),
    ("fr", VoiceInfo("fr", "French")),
    ("es", VoiceInfo("es", "Spanish (Spain)")),
    ("es-mx", VoiceInfo("es-mx", "Spanish (México)")),
    ("it", VoiceInfo("it", "Italian")),
    ("pt-br", VoiceInfo("pt-br", "Brazilian Portuguese")),
    ("ro", VoiceInfo("ro", "Romanian")),
    ("de", VoiceInfo("de", "German")),
    ("nl", VoiceInfo("nl", "Dutch")),
    ("sv", VoiceInfo("sv", "Swedish")),
    ("cs", VoiceInfo("cs", "Czech")),
    ("hr", VoiceInfo("hr", "Croatian")),
    ("pl", VoiceInfo("pl", "Polish")),
    ("ru", VoiceInfo("ru", "Russian")),
    ("sk", VoiceInfo("sk", "Slovak")),
    ("uk", VoiceInfo("uk", "Ukrainian")),
])


# Punctuation pause modes exposed in NVDA settings.
pauseModes = OrderedDict(
    (
        ("off", VoiceInfo("off", "Off")),
        ("short", VoiceInfo("short", "Short")),
        ("long", VoiceInfo("long", "Long")),
    )
)

# Sample rates exposed in NVDA settings
sampleRates = OrderedDict(
    (
        ("11025", VoiceInfo("11025", "11025 Hz")),
        ("16000", VoiceInfo("16000", "16000 Hz (default)")),
        ("22050", VoiceInfo("22050", "22050 Hz")),
        ("44100", VoiceInfo("44100", "44100 Hz")),
    )
)


# Voice presets: multipliers/overrides on generated frames
#
# Parameters ending with "_mul" are multipliers applied to frame values.
# Parameters without "_mul" are absolute values that override the frame.
#
# Key synthesis parameters:
#   - voicePitch/endVoicePitch: fundamental frequency (Hz)
#   - glottalOpenQuotient: 0.30-0.35 pressed/sharp, 0.40 neutral, 0.45-0.50 breathier
#   - voiceTurbulenceAmplitude: breathiness (0-1)
#   - cf1-cf6: cascade formant frequencies (vowel quality)
#   - cb1-cb6: cascade formant bandwidths (narrow=buzzy, wide=smooth)
#   - fricationAmplitude: frication noise level
#   - vibratoPitchOffset/vibratoSpeed: adds human instability
#   - pa1-pa6: parallel formant amplitudes
#   - parallelBypass: noise bypass amount
#
voices = {
    "Adam": {
        "cb1_mul": 1.3,
        "pa6_mul": 1.3,
        "fricationAmplitude_mul": 0.85,
    },
    "Benjamin": {
        "cf1_mul": 1.01,
        "cf2_mul": 1.02,
        "cf4": 3770,
        "cf5": 4100,
        "cf6": 5000,
        "cfNP_mul": 0.9,
        "cb1_mul": 1.3,
        "fricationAmplitude_mul": 0.7,
        "pa6_mul": 1.3,
    },
    "Caleb": {
        "aspirationAmplitude": 1,
        "voiceAmplitude": 0,
    },
    "David": {
        "voicePitch_mul": 0.75,
        "endVoicePitch_mul": 0.75,
        "cf1_mul": 0.75,
        "cf2_mul": 0.85,
        "cf3_mul": 0.85,
    },
    # -------------------------------------------------------------------------
    # Robert: Eloquence-inspired voice (BRIGHT, CRISP, SYNTHETIC)
    # DRAMATICALLY different from Adam:
    # - Slightly higher base pitch for brighter character
    # - Moderately higher upper formants (not extreme - preserves consonants)
    # - Narrow bandwidths for synthetic buzzy tone
    # - Pressed glottis, minimal breathiness
    # - Frication preserved for clear C, S, F sounds
    # -------------------------------------------------------------------------
    "Robert": {
        # Slightly higher pitch for brighter character
        "voicePitch_mul": 1.10,
        "endVoicePitch_mul": 1.10,
        # Moderate formant scaling - not too extreme on highs
        "cf1_mul": 1.02,
        "cf2_mul": 1.06,
        "cf3_mul": 1.08,
        # Upper formants - moderate boost, safe for 11025 Hz
        "cf4_mul": 1.08,
        "cf5_mul": 1.10,
        "cf6_mul": 1.05,
        # Narrow bandwidths for buzzy synthetic sound (but not extreme)
        "cb1_mul": 0.65,
        "cb2_mul": 0.68,
        "cb3_mul": 0.72,
        "cb4_mul": 0.75,
        "cb5_mul": 0.78,
        "cb6_mul": 0.80,
        # Pressed glottis: sharp, precise attack
        "glottalOpenQuotient": 0.30,
        # Minimal breathiness - clean synthetic sound
        "voiceTurbulenceAmplitude_mul": 0.20,
        # INCREASED frication to preserve C, S, F consonants
        "fricationAmplitude_mul": 0.75,
        # Moderate bypass for consonant clarity
        "parallelBypass_mul": 0.70,
        # Moderate high parallel formant boost (reduced from before)
        "pa3_mul": 1.08,
        "pa4_mul": 1.15,
        "pa5_mul": 1.20,
        "pa6_mul": 1.25,
        # Moderate parallel bandwidths (not too tight)
        "pb1_mul": 0.72,
        "pb2_mul": 0.75,
        "pb3_mul": 0.78,
        "pb4_mul": 0.80,
        "pb5_mul": 0.82,
        "pb6_mul": 0.85,
        # Match parallel formants to cascade
        "pf3_mul": 1.06,
        "pf4_mul": 1.08,
        "pf5_mul": 1.10,
        "pf6_mul": 1.05,
        # No vibrato - steady synthetic pitch
        "vibratoPitchOffset": 0.0,
        "vibratoSpeed": 0.0,
    },
}


# Pre-calculate per-voice operations for fast application
_frameFieldNames = {x[0] for x in speechPlayer.Frame._fields_}
_voiceOps = {}
for _voiceName, _voiceMap in voices.items():
    _absOps = []
    _mulOps = []
    for _k, _v in (_voiceMap or {}).items():
        if not isinstance(_k, str):
            continue
        if _k.endswith("_mul"):
            _param = _k[:-4]
            if _param in _frameFieldNames:
                _mulOps.append((_param, _v))
        else:
            if _k in _frameFieldNames:
                _absOps.append((_k, _v))
    _voiceOps[_voiceName] = (tuple(_absOps), tuple(_mulOps))
# Avoid leaking loop variables at module scope.
del _frameFieldNames, _voiceName, _voiceMap, _absOps, _mulOps, _k, _v


def applyVoiceToFrame(frame: speechPlayer.Frame, voiceName: str) -> None:
    absOps, mulOps = _voiceOps.get(voiceName) or _voiceOps.get("Adam", ((), ()))

    for paramName, absVal in absOps:
        setattr(frame, paramName, absVal)

    for paramName, mulVal in mulOps:
        setattr(frame, paramName, getattr(frame, paramName) * mulVal)


class _BgThread(threading.Thread):
    """Runs text->IPA->frames generation so speak() doesn't block NVDA."""
    def __init__(self, q: "queue.Queue", stopEvent: threading.Event):
        super().__init__(name=f"{self.__class__.__module__}.{self.__class__.__qualname__}")
        self.daemon = True
        self._q = q
        self._stop = stopEvent

    def run(self):
        while not self._stop.is_set():
            try:
                item = self._q.get(timeout=0.2)
            except queue.Empty:
                continue
            try:
                if item is None:
                    return
                func, args, kwargs = item
                func(*args, **kwargs)
            except Exception:
                log.error("nvSpeechPlayer: error in background thread", exc_info=True)
            finally:
                try:
                    self._q.task_done()
                except Exception:
                    # Should be extremely rare; log for diagnosability.
                    log.debug("nvSpeechPlayer: background queue task_done failed", exc_info=True)


class _AudioThread(threading.Thread):
    """Pulls synthesized audio from the DLL and feeds nvwave.WavePlayer."""
    
    # Pre-compute cosine fade table for fast lookup (256 entries)
    _FADE_TABLE_SIZE = 256
    _fadeTable = tuple((1.0 - math.cos(i * math.pi / 255)) / 2.0 
                       for i in range(256))
    
    def __init__(self, synth: "SynthDriver", player: speechPlayer.SpeechPlayer, sampleRate: int):
        super().__init__(name=f"{self.__class__.__module__}.{self.__class__.__qualname__}")
        self.daemon = True
        self._synthRef = weakref.ref(synth)
        self._player = player
        self._sampleRate = int(sampleRate)

        self._keepAlive = True
        self.isSpeaking = False

        self._wake = threading.Event()
        self._init = threading.Event()

        self._wavePlayer = None
        self._outputDevice = None

        # Avoid log spam for repeated backend failures.
        self._feedErrorLogged = False
        self._idleErrorLogged = False
        self._synthErrorLogged = False

        # Fade-in state: apply envelope to first audio chunk after stop()/idle()
        self._applyFadeIn = False
        # Fade duration in samples (~12ms for smooth transition that covers stop() discontinuity)
        self._fadeInSamples = int(sampleRate * 0.012)
        # Pre-allocated silence buffer (3ms) to prepend before faded audio
        # This gives the audio device time to stabilize before non-zero samples
        self._silencePrefix = bytes(int(sampleRate * 0.003) * 2)

        self.start()
        self._init.wait()

    def _getOutputDevice(self):
        try:
            return config.conf["audio"]["outputDevice"]
        except Exception:
            try:
                return config.conf["speech"]["outputDevice"]
            except Exception:
                return None

    def _feed(self, data: bytes, onDone=None) -> None:
        if not self._wavePlayer:
            return
        try:
            self._wavePlayer.feed(data, len(data), onDone=onDone)
        except TypeError:
            try:
                if onDone is None:
                    self._wavePlayer.feed(data)
                else:
                    self._wavePlayer.feed(data, onDone=onDone)
            except Exception:
                if not self._feedErrorLogged:
                    log.error("nvSpeechPlayer: WavePlayer.feed failed", exc_info=True)
                    self._feedErrorLogged = True

    def _applyFadeInEnvelope(self, audioBytes: bytes) -> bytes:
        """Apply fade-in envelope to audio samples. Returns modified bytes.
        
        Uses a modified cosine curve that starts at zero and ramps up.
        The first few samples are forced to zero to mask any click from stop().
        """
        samples = array.array('h')
        samples.frombytes(audioBytes)
        fadeLen = min(self._fadeInSamples, len(samples))
        
        if fadeLen > 0:
            # Force first few samples to absolute zero (mask any click)
            zeroSamples = min(fadeLen // 4, 30)  # ~0.7ms at 44.1kHz
            for i in range(zeroSamples):
                samples[i] = 0
            
            # Apply cosine fade to remaining samples
            tableSize = self._FADE_TABLE_SIZE
            fadeTable = self._fadeTable
            fadeStart = zeroSamples
            fadeRemaining = fadeLen - fadeStart
            
            for i in range(fadeStart, fadeLen):
                # Map sample index to table index (starting from where zeros end)
                progress = (i - fadeStart) / fadeRemaining if fadeRemaining > 0 else 1.0
                tableIdx = int(progress * (tableSize - 1))
                samples[i] = int(samples[i] * fadeTable[tableIdx])
        
        # Prepend silence to let audio device stabilize
        return self._silencePrefix + samples.tobytes()

    def terminate(self):
        self._keepAlive = False
        self.isSpeaking = False
        self._wake.set()
        self.join(timeout=2.0)
        try:
            if self._wavePlayer:
                self._wavePlayer.stop()
        except Exception:
            log.debug("nvSpeechPlayer: WavePlayer.stop failed during terminate", exc_info=True)

    def kick(self):
        self._wake.set()

    def run(self):
        try:
            self._outputDevice = self._getOutputDevice()
            # Try to create WavePlayer with AudioPurpose.SPEECH for NVDA's built-in
            # audio keepalive and trimming features
            try:
                self._wavePlayer = nvwave.WavePlayer(
                    channels=1,
                    samplesPerSec=self._sampleRate,
                    bitsPerSample=16,
                    outputDevice=self._outputDevice,
                    purpose=nvwave.AudioPurpose.SPEECH,
                )
            except (TypeError, AttributeError):
                # Older NVDA versions don't have purpose parameter or AudioPurpose
                try:
                    self._wavePlayer = nvwave.WavePlayer(
                        channels=1,
                        samplesPerSec=self._sampleRate,
                        bitsPerSample=16,
                        outputDevice=self._outputDevice,
                        buffered=True,
                    )
                except TypeError:
                    self._wavePlayer = nvwave.WavePlayer(
                        channels=1,
                        samplesPerSec=self._sampleRate,
                        bitsPerSample=16,
                        outputDevice=self._outputDevice,
                    )
        except Exception:
            log.error("nvSpeechPlayer: failed to initialize audio output", exc_info=True)
            self._wavePlayer = None
        finally:
            self._init.set()

        # Local references for faster access in tight loop
        player = self._player
        wavePlayer = self._wavePlayer
        wake = self._wake
        synthRef = self._synthRef
        
        while self._keepAlive:
            wake.wait()
            wake.clear()

            lastIndex = None
            isFirstChunk = True

            while self._keepAlive and self.isSpeaking:
                try:
                    data = player.synthesize(8192)
                except Exception:
                    if not self._synthErrorLogged:
                        log.error("nvSpeechPlayer: speechPlayer.synthesize failed", exc_info=True)
                        self._synthErrorLogged = True
                    break

                if data:
                    n = int(getattr(data, "length", 0) or 0)
                    if n <= 0:
                        continue

                    nbytes = n * 2  # 16-bit = 2 bytes per sample
                    audioBytes = ctypes.string_at(ctypes.addressof(data), nbytes)
                    
                    # Apply fade-in to first chunk after stop()/idle() to prevent click
                    if self._applyFadeIn and isFirstChunk:
                        audioBytes = self._applyFadeInEnvelope(audioBytes)
                        self._applyFadeIn = False
                    isFirstChunk = False

                    idx = int(player.getLastIndex())
                    s = synthRef()

                    if idx >= 0:
                        def cb(index=idx, synth=s):
                            if synth:
                                synthIndexReached.notify(synth=synth, index=index)
                        self._feed(audioBytes, onDone=cb)
                    else:
                        self._feed(audioBytes)

                    lastIndex = idx
                    continue

                # No audio was produced - check for index markers
                idx = int(player.getLastIndex())
                if idx >= 0 and idx != lastIndex:
                    s = synthRef()
                    if s:
                        def cb(index=idx, synth=s):
                            if synth:
                                synthIndexReached.notify(synth=synth, index=index)
                        self._feed(b"", onDone=cb)
                    lastIndex = idx
                    continue

                break

            # Stream finished - go idle and prepare for next stream
            try:
                if wavePlayer:
                    wavePlayer.idle()
                    # Next audio feed should have fade-in applied
                    self._applyFadeIn = True
            except Exception:
                if not self._idleErrorLogged:
                    log.debug("nvSpeechPlayer: WavePlayer.idle failed", exc_info=True)
                    self._idleErrorLogged = True

            s = synthRef()
            if s:
                synthDoneSpeaking.notify(synth=s)

            self.isSpeaking = False


class SynthDriver(SynthDriver):
    name = "nvSpeechPlayer"
    description = "NV Speech Player"

    _supportedSettings = [
        SynthDriver.VoiceSetting(),
        SynthDriver.RateSetting(),
        SynthDriver.PitchSetting(),
        SynthDriver.InflectionSetting(),
        SynthDriver.VolumeSetting(),
        DriverSetting("pauseMode", "Pause mode"),
        DriverSetting("sampleRate", "Sample rate"),
        DriverSetting("language", "Language"),

        # --- Language-pack quick settings (YAML: packs/lang/*.yaml -> settings:) ---
        DriverSetting("stopClosureMode", "Stop closure mode"),
        # Newer setting: how diphthongs are handled in spelled-out text.
        # (Supported values: none, monophthong)
        DriverSetting("spellingDiphthongMode", "Spelling diphthong mode"),
    ]

    if BooleanDriverSetting is not None:
        _supportedSettings.extend(
            [
                BooleanDriverSetting("stopClosureClusterGapsEnabled", "Insert brief closure pauses in consonant clusters"),  # type: ignore
                BooleanDriverSetting("stopClosureAfterNasalsEnabled", "Insert stop closure after nasal sounds"),  # type: ignore
                BooleanDriverSetting("autoTieDiphthongs", "Treat diphthongs as a single connected sound"),  # type: ignore
                BooleanDriverSetting("autoDiphthongOffglideToSemivowel", "Convert diphthong offglides into smoother semivowels"),  # type: ignore
                BooleanDriverSetting("segmentBoundarySkipVowelToVowel", "Skip join gap between speech chunks when a vowel follows a vowel"),  # type: ignore
                BooleanDriverSetting("segmentBoundarySkipVowelToLiquid", "Skip join gap between speech chunks when a liquid follows a vowel"),  # type: ignore
                BooleanDriverSetting("postStopAspirationEnabled", "Add aspiration after unvoiced stop consonants"),  # type: ignore
                # --- Coarticulation settings ---
                BooleanDriverSetting("coarticulationEnabled", "Enable formant coarticulation"),  # type: ignore
                BooleanDriverSetting("coarticulationFadeIntoConsonants", "Fade coarticulation into consonants"),  # type: ignore
                BooleanDriverSetting("coarticulationVelarPinchEnabled", "Enable velar pinch effect for coarticulation"),  # type: ignore
                BooleanDriverSetting("coarticulationGraduated", "Use graduated coarticulation blending"),  # type: ignore
            ]
        )
        # Coarticulation adjacency combo-box (grouped with coarticulation settings)
        _supportedSettings.append(DriverSetting("coarticulationAdjacencyMaxConsonants", "Coarticulation adjacency range"))

    if BooleanDriverSetting is not None:
        _supportedSettings.extend(
            [
                # --- Phrase-final lengthening settings ---
                BooleanDriverSetting("phraseFinalLengtheningEnabled", "Enable phrase-final lengthening"),  # type: ignore
                BooleanDriverSetting("phraseFinalLengtheningNucleusOnlyMode", "Apply phrase-final lengthening to nucleus only"),  # type: ignore
                # --- Microprosody settings ---
                BooleanDriverSetting("microprosodyEnabled", "Enable microprosody adjustments"),  # type: ignore
                BooleanDriverSetting("microprosodyVoicelessF0RaiseEnabled", "Raise F0 for voiceless consonants"),  # type: ignore
                BooleanDriverSetting("microprosodyVoicedF0LowerEnabled", "Lower F0 for voiced consonants"),  # type: ignore
                # --- Rate reduction settings ---
                BooleanDriverSetting("rateReductionEnabled", "Enable rate-dependent reduction"),  # type: ignore
                # --- Nasalization settings ---
                BooleanDriverSetting("nasalizationAnticipatoryEnabled", "Enable anticipatory nasalization"),  # type: ignore
                # --- Liquid dynamics settings ---
                BooleanDriverSetting("liquidDynamicsEnabled", "Enable liquid dynamics (lateral onglide transitions)"),  # type: ignore
                # --- Length contrast settings ---
                BooleanDriverSetting("lengthContrastEnabled", "Enable phonemic length contrast"),  # type: ignore
                # --- Positional allophones settings ---
                BooleanDriverSetting("positionalAllophonesEnabled", "Enable positional allophone variation"),  # type: ignore
                BooleanDriverSetting("positionalAllophonesGlottalReinforcementEnabled", "Enable glottal reinforcement for stops"),  # type: ignore
                # --- Boundary smoothing settings ---
                BooleanDriverSetting("boundarySmoothingEnabled", "Enable boundary smoothing between sounds"),  # type: ignore
                # --- Trajectory limit settings ---
                BooleanDriverSetting("trajectoryLimitEnabled", "Enable formant trajectory rate limiting"),  # type: ignore
                BooleanDriverSetting("trajectoryLimitApplyAcrossWordBoundary", "Apply trajectory limit across word boundaries"),  # type: ignore
                BooleanDriverSetting("legacyPitchMode", "Use classic pitch and intonation style"),  # type: ignore
                BooleanDriverSetting("tonal", "Enable tonal language behavior"),  # type: ignore
                BooleanDriverSetting("toneDigitsEnabled", "Interpret tone numbers in text"),  # type: ignore
            ]
        )

    _supportedSettings.append(DriverSetting("toneContoursMode", "Tone contour mode"))

    if BooleanDriverSetting is not None:
        _supportedSettings.extend(
            [
                BooleanDriverSetting("stripAllophoneDigits", "Strip allophone digits"),  # type: ignore
                BooleanDriverSetting("stripHyphen", "Strip hyphens from IPA output"),  # type: ignore
            ]
        )

    supportedSettings = tuple(_supportedSettings)

    supportedCommands = {c for c in (IndexCommand, PitchCommand) if c}
    supportedNotifications = {synthIndexReached, synthDoneSpeaking}

    exposeExtraParams = False
    _ESPEAK_PHONEME_MODE = 0x36100 + 0x82

    def __init__(self):
        super().__init__()

        # Suppress YAML writes during NVDA config replay (YAML is source of truth)
        self._suppressLangPackWrites = True
        self._scheduleEnableLangPackWrites()


        # NVDA 2025.x is 32-bit; NVDA 2026.x is 64-bit.
        # We ship both x86 and x64 DLLs and select the right ones at runtime.
        if ctypes.sizeof(ctypes.c_void_p) not in (4, 8):
            raise RuntimeError('nvSpeechPlayer: unsupported Python architecture')

        if self.exposeExtraParams:
            self._extraParamNames = [x[0] for x in speechPlayer.Frame._fields_]
            self._extraParamAttrNames = [f"speechPlayer_{x}" for x in self._extraParamNames]

            extraSettings = tuple(
                NumericDriverSetting(attrName, f"Frame: {paramName}")
                for paramName, attrName in zip(self._extraParamNames, self._extraParamAttrNames)
            )
            self.supportedSettings = self.supportedSettings + extraSettings
            for attrName in self._extraParamAttrNames:
                setattr(self, attrName, 50)

        self._sampleRate = 16000
        self._player = speechPlayer.SpeechPlayer(self._sampleRate)

        # Frontend: YAML packs + IPA->frames conversion.
        here = os.path.dirname(__file__)
        packsDir = os.path.join(here, "packs")
        packDir = packsDir

        # Cache packsDir for language-pack editing helpers.
        self._packsDir = packsDir
        self._langPackSettingsCache: dict[str, object] = {}

        # Initialize language tag early so cache refresh works correctly.
        # This will be updated again when self.language is set, but having a valid
        # initial value ensures _refreshLangPackSettingsCache can work if called early.
        self._language = "en-us"

        # Prime the settings cache immediately after _packsDir is set.
        # This ensures YAML-backed settings are available before NVDA queries them
        # during initial driver configuration.
        self._refreshLangPackSettingsCache()

        # Validate expected pack files so missing optional language YAML doesn't silently
        # fall back to default.
        requiredRel = [
            "phonemes.yaml",
            os.path.join("lang", "default.yaml"),
            os.path.join("lang", "bg.yaml"),
            os.path.join("lang", "zh.yaml"),
            os.path.join("lang", "hu.yaml"),
            os.path.join("lang", "pt.yaml"),
            os.path.join("lang", "pl.yaml"),
            os.path.join("lang", "es.yaml"),
        ]
        missingRel = []
        for rel in requiredRel:
            try:
                if not os.path.isfile(os.path.join(packsDir, rel)):
                    missingRel.append(rel)
            except Exception:
                missingRel.append(rel)

        # Hard fail if the core files are missing.
        if not os.path.isdir(packsDir) or "phonemes.yaml" in missingRel or os.path.join("lang", "default.yaml") in missingRel:
            msg = "nvSpeechPlayer: missing required packs under %s" % packsDir
            if missingRel:
                msg += ": " + ", ".join(missingRel)
            raise RuntimeError(msg)

        # Soft warning for missing optional language packs.
        optMissing = [x for x in missingRel if x not in ("phonemes.yaml", os.path.join("lang", "default.yaml"))]
        if optMissing:
            log.warning("nvSpeechPlayer: missing optional language packs: %s", ", ".join(optMissing))

        dllDir = findDllDir(here)
        if not dllDir:
            raise RuntimeError('nvSpeechPlayer: could not find DLLs for this architecture')
        feDllPath = os.path.join(dllDir, 'nvspFrontend.dll')
        self._frontend = NvspFrontend(feDllPath, packDir)

        # Load default explicitly now. If this fails, the frontend won't be usable.
        if not self._frontend.setLanguage("default"):
            raise RuntimeError(
                "nvSpeechPlayer: could not load default pack: %s" % (self._frontend.getLastError() or "unknown error")
            )

        # Preload language-specific packs you said you ship right now.
        # This doesn't lock you in: calling setLanguage() later will reload/merge packs.
        for tag in ("bg", "zh", "hu", "pt", "pl", "es"):
            try:
                if not self._frontend.setLanguage(tag):
                    log.error(f"nvSpeechPlayer: failed to load language pack '{tag}': {self._frontend.getLastError()}")
            except Exception:
                log.error("nvSpeechPlayer: error while preloading language packs", exc_info=True)

        _espeak.initialize()

        # Fix espeak_TextToPhonemes prototype for 64-bit Python
        try:
            if getattr(_espeak, "espeakDLL", None):
                _ttp = _espeak.espeakDLL.espeak_TextToPhonemes
                _ttp.argtypes = (ctypes.POINTER(ctypes.c_void_p), ctypes.c_int, ctypes.c_int)
                _ttp.restype = ctypes.c_void_p
        except Exception:
            log.debug("nvSpeechPlayer: failed to configure espeak_TextToPhonemes prototype", exc_info=True)

        # Note: self._language was already set earlier (before cache priming).
        self._curPitch = 50
        self._curVoice = "Adam"
        self._curInflection = 0.5
        self._curVolume = 1.0
        self._curRate = 1.0

        # Punctuation pause mode:
        # - off: do not insert extra pauses
        # - short/long: insert very small silences after punctuation to make
        #   clause boundaries more perceptible without introducing clicks.
        self._pauseMode = "short"

        # Trigger the language setter to configure espeak and frontend.
        # This also refreshes the cache (though it's already primed above).
        self.language = self._language

        self.pitch = 45
        self.rate = 50
        self.volume = 90
        self.inflection = 50

        self._audio = _AudioThread(self, self._player, self._sampleRate)

        self._bgQueue: "queue.Queue" = queue.Queue()
        self._bgStop = threading.Event()
        self._bgThread = _BgThread(self._bgQueue, self._bgStop)
        self._bgThread.start()
    @classmethod
    def check(cls):
        # Ensure DLLs exist for this NVDA / Python architecture (x86 vs x64).
        if ctypes.sizeof(ctypes.c_void_p) not in (4, 8):
            return False

        here = os.path.dirname(__file__)
        dllDir = findDllDir(here)
        if not dllDir:
            return False

        # Packs are expected in a local ./packs folder.
        packsDir = os.path.join(here, 'packs')
        if not os.path.isdir(packsDir):
            return False

        # Accept either casing on Windows, but check for both to be explicit.
        phonemesLower = os.path.join(packsDir, 'phonemes.yaml')
        phonemesUpper = os.path.join(packsDir, 'Phonemes.yaml')
        if not (os.path.isfile(phonemesLower) or os.path.isfile(phonemesUpper)):
            return False

        # default.yaml is required (others are optional at runtime).
        defaultYaml = os.path.join(packsDir, 'lang', 'default.yaml')
        if not os.path.isfile(defaultYaml):
            return False

        return True

    def _get_availableLanguages(self):
        return languages

    # ---- Punctuation pause mode (driver setting) ----

    def _get_availablePauseModes(self):
        return pauseModes

    # Alias for NVDA 2023.x compatibility (capitalize() lowercases the remainder).
    _get_availablePausemodes = _get_availablePauseModes

    def _get_pauseMode(self):
        return getattr(self, "_pauseMode", "short")

    def _set_pauseMode(self, mode):
        m = str(mode or "").strip().lower()
        if m not in pauseModes:
            m = "short"
        self._pauseMode = m

    # ---- Sample rate (driver setting) ----

    def _get_availableSampleRates(self):
        return sampleRates

    # Alias for NVDA 2023.x compatibility (capitalize() lowercases the remainder).
    _get_availableSamplerates = _get_availableSampleRates

    def _get_sampleRate(self):
        return str(getattr(self, "_sampleRate", 16000))

    def _set_sampleRate(self, rate):
        try:
            r = int(str(rate).strip())
        except (ValueError, TypeError):
            r = 22050
        if str(r) not in sampleRates:
            r = 22050
        
        # Only reinitialize if rate actually changed
        if r == getattr(self, "_sampleRate", None):
            return
        
        self._sampleRate = r
        
        # Only reinitialize if audio system already exists (not during initial construction)
        if hasattr(self, "_audio") and self._audio:
            self._reinitializeAudio()

    def _reinitializeAudio(self):
        """Reinitialize audio subsystem after sample rate change."""
        try:
            self.cancel()
        except Exception:
            log.debug("nvSpeechPlayer: cancel failed during audio reinit", exc_info=True)
        
        try:
            # Terminate old audio thread
            if hasattr(self, "_audio") and self._audio:
                self._audio.terminate()
            
            # Terminate old player
            if hasattr(self, "_player") and self._player:
                self._player.terminate()
            
            # Create new player with new sample rate
            self._player = speechPlayer.SpeechPlayer(self._sampleRate)
            
            # Create new audio thread
            self._audio = _AudioThread(self, self._player, self._sampleRate)
            
        except Exception:
            log.error("nvSpeechPlayer: failed to reinitialize audio", exc_info=True)

    def _get_language(self):
        return getattr(self, "_language", "en-us")

    def _set_language(self, langCode):
        code = str(langCode or "").strip().lower()
        if code not in languages:
            code = "en-us"

        try:
            self.cancel()
        except Exception:
            log.debug("nvSpeechPlayer: cancel failed while changing language", exc_info=True)

        applied = False
        for tryCode in (code, code.replace("_", "-"), code.replace("-", "_")):
            try:
                ok = _espeak.setVoiceByLanguage(tryCode)
                if ok is None or ok:
                    applied = True
                    code = tryCode.lower()
                    break
            except Exception:
                continue

        if not applied:
            try:
                _espeak.setVoiceByLanguage("en")
                code = "en"
                applied = True
            except Exception:
                log.error("nvSpeechPlayer: could not set language", exc_info=True)

        self._language = code

        # Keep frontend pack selection in sync with the driver language.
        try:
            if getattr(self, "_frontend", None):
                # Packs are stored by language tag. Region variants (e.g. "es-mx")
                # may not exist even when the base language ("es") does.
                tag = str(code or "").strip().lower().replace("_", "-")
                candidates = [tag]
                if "-" in tag:
                    candidates.append(tag.split("-", 1)[0])
                candidates.append("default")

                loaded = False
                for cand in candidates:
                    try:
                        if self._frontend.setLanguage(cand):
                            loaded = True
                            break
                    except Exception:
                        log.debug(
                            "nvSpeechPlayer: frontend.setLanguage failed for %r", cand, exc_info=True
                        )
                        continue

                if not loaded:
                    log.error(
                        f"nvSpeechPlayer: frontend could not load pack for '{code}' (tried {candidates}): {self._frontend.getLastError()}"
                    )
        except Exception:
            log.error("nvSpeechPlayer: error setting frontend language", exc_info=True)

        # Refresh cached language-pack settings for the (possibly) new language.
        try:
            self._refreshLangPackSettingsCache()
        except Exception:
            log.debug("nvSpeechPlayer: could not refresh language-pack cache", exc_info=True)

        # Trigger a GUI refresh so checkboxes update to reflect the new language pack.
        self._scheduleSettingsPanelRefresh()

    def _scheduleSettingsPanelRefresh(self) -> None:
        """Schedule a refresh of the VoiceSettingsPanel to update checkboxes.

        When the user changes language, the language pack's settings may differ.
        This method finds the open settings panel (if any) and triggers an update
        so checkboxes reflect the new language pack's values.
        """
        try:
            import wx
            wx.CallAfter(self._doSettingsPanelRefresh)
        except Exception:
            pass

    def _doSettingsPanelRefresh(self) -> None:
        """Actually perform the settings panel refresh (called via wx.CallAfter)."""
        try:
            import wx
            from gui import settingsDialogs

            # Look for an open NVDASettingsDialog.
            for win in wx.GetTopLevelWindows():
                # Handle different NVDA versions: NVDASettingsDialog or SettingsDialog.
                dlgCls = getattr(settingsDialogs, "NVDASettingsDialog", None)
                if dlgCls is None:
                    dlgCls = getattr(settingsDialogs, "SettingsDialog", None)
                if dlgCls is None:
                    continue

                if not isinstance(win, dlgCls):
                    continue

                # Found the settings dialog. Now find the VoiceSettingsPanel.
                # Different NVDA versions store panels differently.
                panels = []

                # NVDA 2024+: catIdToInstanceMap
                if hasattr(win, "catIdToInstanceMap"):
                    panels.extend(win.catIdToInstanceMap.values())

                # Older NVDA: categoryClasses or similar
                if hasattr(win, "_categoryPanel"):
                    panels.append(win._categoryPanel)

                # Try currentCategory attribute
                if hasattr(win, "currentCategory"):
                    cat = win.currentCategory
                    if cat is not None:
                        panels.append(cat)

                voicePanelCls = getattr(settingsDialogs, "VoiceSettingsPanel", None)
                for panel in panels:
                    if panel is None:
                        continue
                    # Check if it's a VoiceSettingsPanel or has updateDriverSettings
                    isVoicePanel = voicePanelCls and isinstance(panel, voicePanelCls)
                    hasUpdater = hasattr(panel, "updateDriverSettings")

                    if isVoicePanel or hasUpdater:
                        try:
                            panel.updateDriverSettings(changedSetting="language")
                        except Exception:
                            log.debug(
                                "nvSpeechPlayer: updateDriverSettings failed",
                                exc_info=True,
                            )
                        break
                break
        except Exception:
            # GUI may not be available (e.g., running in secure mode or during shutdown).
            log.debug("nvSpeechPlayer: could not refresh settings panel", exc_info=True)

    # ---- Startup guard for YAML writes (see __init__) ----

    def _enableLangPackWrites(self) -> None:
        """Re-enable writing language-pack settings back to YAML."""
        self._suppressLangPackWrites = False

    def _scheduleEnableLangPackWrites(self) -> None:
        """Schedule re-enabling YAML writes after NVDA finishes config replay."""
        try:
            import core
            callLater = getattr(core, "callLater", None)
            if callable(callLater):
                callLater(0, self._enableLangPackWrites)
                return
        except Exception:
            pass

        try:
            import wx
            if hasattr(wx, "CallAfter"):
                wx.CallAfter(self._enableLangPackWrites)
                return
        except Exception:
            pass

        self._enableLangPackWrites()

    # ---- Language-pack (YAML) helpers ----

    def _getCurrentLangTag(self) -> str:
        """Return the current language tag in the pack file format (lowercase, hyphen)."""
        return str(getattr(self, "_language", "default") or "default").strip().lower().replace("_", "-")

    def _applyFrontendLangTag(self, tag: str) -> bool:
        """Ask the frontend to (re)load packs for *tag*, trying sensible fallbacks.

        Returns True if the frontend reported a successful load.
        """
        if not getattr(self, "_frontend", None):
            return False

        tag = str(tag or "default").strip().lower().replace("_", "-")
        candidates = [tag]
        if "-" in tag:
            candidates.append(tag.split("-", 1)[0])
        candidates.append("default")

        for cand in candidates:
            try:
                if self._frontend.setLanguage(cand):
                    self._frontendLangTag = cand
                    return True
            except Exception:
                log.debug("nvSpeechPlayer: frontend.setLanguage failed for %r", cand, exc_info=True)
                continue

        try:
            lastErr = self._frontend.getLastError()
        except Exception:
            log.debug("nvSpeechPlayer: frontend.getLastError failed", exc_info=True)
            lastErr = None
        log.error("nvSpeechPlayer: frontend could not load pack for %r (tried %s). %s", tag, candidates, lastErr)
        return False

    def reloadLanguagePack(self, tag: str | None = None) -> bool:
        """Public helper (used by the settings panel) to reload frontend packs.

        If *tag* is omitted, reloads the currently selected driver language.
        """
        ok = self._applyFrontendLangTag(tag or self._getCurrentLangTag())
        if ok:
            try:
                self._refreshLangPackSettingsCache()
            except Exception:
                log.debug("nvSpeechPlayer: could not refresh language-pack cache after reload", exc_info=True)
        return ok

    def _refreshLangPackSettingsCache(self) -> None:
        """Rebuild the cached effective YAML ``settings:`` map for the current language."""
        try:
            from . import langPackYaml

            packsDir = getattr(self, "_packsDir", None)
            if not packsDir:
                self._langPackSettingsCache = {}
                return

            self._langPackSettingsCache = langPackYaml.getEffectiveSettings(
                packsDir=packsDir,
                langTag=self._getCurrentLangTag(),
            )

            # Clear previous error key on success.
            if getattr(self, "_lastLangPackCacheErrorKey", None) is not None:
                self._lastLangPackCacheErrorKey = None
        except Exception as e:
            # Avoid "log spam" if a corrupt YAML causes repeated refresh failures.
            try:
                tag = self._getCurrentLangTag()
            except Exception:
                tag = None
            errKey = (tag, type(e).__name__, str(e))
            if getattr(self, "_lastLangPackCacheErrorKey", None) != errKey:
                log.error("nvSpeechPlayer: failed to read language-pack settings for %r", tag, exc_info=True)
                self._lastLangPackCacheErrorKey = errKey
            self._langPackSettingsCache = {}

    def _getLangPackBool(self, key: str, default: bool = False) -> bool:
        # Users may edit YAML on disk (either via our settings panel, or via a
        # text editor). Refresh the cache opportunistically so the values shown
        # in NVDA's GUI don't go stale and then get written back to disk.
        self._refreshLangPackSettingsCache()
        try:
            from . import langPackYaml

            raw = getattr(self, "_langPackSettingsCache", {}).get(key)
            return langPackYaml.parseBool(raw, default)
        except Exception:
            return default

    def _getLangPackStr(self, key: str, default: str = "") -> str:
        self._refreshLangPackSettingsCache()
        raw = getattr(self, "_langPackSettingsCache", {}).get(key)
        if raw is None:
            return default
        return str(raw)

    def _setLangPackSetting(self, key: str, value: object) -> None:
        """Write a language-pack ``settings:`` key and reload packs."""
        try:
            from . import langPackYaml

            # During driver initialization NVDA may replay persisted settings
            # from config.conf by calling our property setters. For YAML-backed
            # language-pack settings we treat YAML as authoritative, so we
            # suppress writes during that replay window to avoid overwriting
            # edits made via Notepad or our settings panel.
            if getattr(self, "_suppressLangPackWrites", False):
                return

            # Ensure our effective-value comparison reflects the current files
            # on disk (the YAML may have been edited externally).
            self._refreshLangPackSettingsCache()

            packsDir = getattr(self, "_packsDir", None)
            if not packsDir:
                return

            langTag = self._getCurrentLangTag()

            # Avoid churn if no effective change.
            cur = getattr(self, "_langPackSettingsCache", {}).get(key)
            try:
                if isinstance(value, bool):
                    # If cur is None, the key doesn't exist in YAML yet - always write it.
                    if cur is not None and langPackYaml.parseBool(cur, default=value) == value:
                        return
                else:
                    # Packs store scalars as strings; normalize whitespace for comparison.
                    if cur is not None and str(cur).strip() == str(value).strip():
                        return
            except Exception:
                log.debug(
                    "nvSpeechPlayer: error comparing language-pack setting %s", key, exc_info=True
                )

            langPackYaml.upsertSetting(
                packsDir=packsDir,
                langTag=langTag,
                key=key,
                value=value,
            )
            # Reload so the frontend re-reads updated YAML.
            self.reloadLanguagePack(langTag)
        except Exception:
            log.error("nvSpeechPlayer: failed to update language-pack setting %s", key, exc_info=True)

    def _choiceToIdStr(self, value: object) -> str:
        """Return the underlying id string for a combo-box choice.

        NVDA's settings UI may pass either the raw id string or an object
        (for example a VoiceInfo).
        """
        if value is None:
            return ""
        # Common attribute spellings used across NVDA versions.
        for attr in ("id", "ID"):
            try:
                v = getattr(value, attr)
            except Exception:
                continue
            if v is not None and v != "":
                return str(v)
        return str(value)

    # ---- Language-pack quick settings exposed in the synth settings dialog ----

    # NOTE:
    # NVDA's driver settings dialog historically computed the "available..." attribute
    # name for string settings using Python's str.capitalize(), which *lowercases* the
    # remainder of the setting id (e.g. stopClosureMode -> Stopclosuremode).
    # Newer NVDA versions preserve camelCase.
    #
    # For maximum compatibility (NVDA 2023.2+), we provide BOTH spellings for each
    # choice/enum setting.

    _STOP_CLOSURE_MODES = OrderedDict(
        (
            ("always", VoiceInfo("always", "Always")),
            ("after-vowel", VoiceInfo("after-vowel", "After vowel")),
            ("vowel-and-cluster", VoiceInfo("vowel-and-cluster", "Vowel and cluster")),
            ("none", VoiceInfo("none", "None")),
        )
    )

    _SPELLING_DIPHTHONG_MODES = OrderedDict(
        (
            ("none", VoiceInfo("none", "None")),
            ("monophthong", VoiceInfo("monophthong", "Monophthong")),
        )
    )

    _TONE_CONTOURS_MODES = OrderedDict(
        (
            ("absolute", VoiceInfo("absolute", "Absolute")),
            ("relative", VoiceInfo("relative", "Relative")),
        )
    )

    _COARTICULATION_ADJACENCY_MODES = OrderedDict(
        (
            ("0", VoiceInfo("0", "Immediate neighbors only")),
            ("1", VoiceInfo("1", "Allow C_V (one consonant)")),
            ("2", VoiceInfo("2", "Allow CC_V (two consonants)")),
        )
    )

    def _makeLangPackAccessors(attrName, yamlKey, kind="str", default=None, choices=None):
        """Generate _get/_set (and available* when needed) methods for YAML-backed settings."""

        def getter(self, _key=yamlKey, _default=default, _kind=kind):
            if _kind == "bool":
                return self._getLangPackBool(_key, default=_default)
            return self._getLangPackStr(_key, default=_default)

        def setter(self, val, _key=yamlKey, _kind=kind):
            if _kind == "bool":
                self._setLangPackSetting(_key, bool(val))
            else:
                self._setLangPackSetting(_key, self._choiceToIdStr(val))

        accessors = {
            f"_get_{attrName}": getter,
            f"_set_{attrName}": setter,
        }

        if choices is not None:
            # Provide BOTH the camelCase spelling and the historical capitalize() spelling.
            camelPlural = attrName[0].upper() + attrName[1:] + "s"
            capPlural = attrName.capitalize() + "s"

            def availGetter(self, _choices=choices):
                return _choices

            accessors[f"_get_available{camelPlural}"] = availGetter
            accessors[f"_get_available{capPlural}"] = availGetter

        return accessors

    # Settings specs: (attrName, yamlKey, kind, default, choices)
    # - kind: "bool" or "enum" (treated as string)
    # - choices: OrderedDict for "enum" settings; None for bool settings.
    #
    # NOTE: Boolean settings for extra/optional features default to False so that
    # when the driver is first configured, it starts with a minimal/conservative
    # configuration. Users can then enable features as desired.
    _LANG_PACK_SPECS = (
        ("stopClosureMode", "stopClosureMode", "enum", "vowel-and-cluster", _STOP_CLOSURE_MODES),
        ("stopClosureClusterGapsEnabled", "stopClosureClusterGapsEnabled", "bool", False, None),
        ("stopClosureAfterNasalsEnabled", "stopClosureAfterNasalsEnabled", "bool", False, None),
        ("autoTieDiphthongs", "autoTieDiphthongs", "bool", False, None),
        ("autoDiphthongOffglideToSemivowel", "autoDiphthongOffglideToSemivowel", "bool", False, None),
        ("segmentBoundarySkipVowelToVowel", "segmentBoundarySkipVowelToVowel", "bool", False, None),
        ("segmentBoundarySkipVowelToLiquid", "segmentBoundarySkipVowelToLiquid", "bool", False, None),
        ("spellingDiphthongMode", "spellingDiphthongMode", "enum", "none", _SPELLING_DIPHTHONG_MODES),
        ("postStopAspirationEnabled", "postStopAspirationEnabled", "bool", False, None),
        # --- Coarticulation settings ---
        ("coarticulationEnabled", "coarticulationEnabled", "bool", False, None),
        ("coarticulationFadeIntoConsonants", "coarticulationFadeIntoConsonants", "bool", False, None),
        ("coarticulationVelarPinchEnabled", "coarticulationVelarPinchEnabled", "bool", False, None),
        ("coarticulationGraduated", "coarticulationGraduated", "bool", False, None),
        ("coarticulationAdjacencyMaxConsonants", "coarticulationAdjacencyMaxConsonants", "enum", "2", _COARTICULATION_ADJACENCY_MODES),
        # --- Phrase-final lengthening settings ---
        ("phraseFinalLengtheningEnabled", "phraseFinalLengtheningEnabled", "bool", False, None),
        ("phraseFinalLengtheningNucleusOnlyMode", "phraseFinalLengtheningNucleusOnlyMode", "bool", False, None),
        # --- Microprosody settings ---
        ("microprosodyEnabled", "microprosodyEnabled", "bool", False, None),
        ("microprosodyVoicelessF0RaiseEnabled", "microprosodyVoicelessF0RaiseEnabled", "bool", False, None),
        ("microprosodyVoicedF0LowerEnabled", "microprosodyVoicedF0LowerEnabled", "bool", False, None),
        # --- Rate reduction settings ---
        ("rateReductionEnabled", "rateReductionEnabled", "bool", False, None),
        # --- Nasalization settings ---
        ("nasalizationAnticipatoryEnabled", "nasalizationAnticipatoryEnabled", "bool", False, None),
        # --- Liquid dynamics settings ---
        ("liquidDynamicsEnabled", "liquidDynamics.enabled", "bool", False, None),
        # --- Length contrast settings ---
        ("lengthContrastEnabled", "lengthContrast.enabled", "bool", False, None),
        # --- Positional allophones settings ---
        ("positionalAllophonesEnabled", "positionalAllophones.enabled", "bool", False, None),
        ("positionalAllophonesGlottalReinforcementEnabled", "positionalAllophones.glottalReinforcement.enabled", "bool", False, None),
        # --- Boundary smoothing settings ---
        ("boundarySmoothingEnabled", "boundarySmoothing.enabled", "bool", False, None),
        # --- Trajectory limit settings ---
        ("trajectoryLimitEnabled", "trajectoryLimit.enabled", "bool", False, None),
        ("trajectoryLimitApplyAcrossWordBoundary", "trajectoryLimit.applyAcrossWordBoundary", "bool", False, None),
        ("legacyPitchMode", "legacyPitchMode", "bool", False, None),
        ("tonal", "tonal", "bool", False, None),
        ("toneDigitsEnabled", "toneDigitsEnabled", "bool", False, None),
        ("toneContoursMode", "toneContoursMode", "enum", "absolute", _TONE_CONTOURS_MODES),
        ("stripAllophoneDigits", "stripAllophoneDigits", "bool", False, None),
        ("stripHyphen", "stripHyphen", "bool", False, None),
    )

    for _attrName, _yamlKey, _kind, _default, _choices in _LANG_PACK_SPECS:
        for _methName, _meth in _makeLangPackAccessors(
            _attrName,
            _yamlKey,
            kind=_kind,
            default=_default,
            choices=_choices,
        ).items():
            locals()[_methName] = _meth

    # Clean up generator helpers so they don't become part of the public driver API.
    del _makeLangPackAccessors, _LANG_PACK_SPECS, _attrName, _yamlKey, _kind, _default, _choices, _methName, _meth

    def _enqueue(self, func, *args, **kwargs):
        if self._bgStop.is_set():
            return
        self._bgQueue.put((func, args, kwargs))

    def _notifyIndexesAndDone(self, indexes):
        for i in indexes:
            synthIndexReached.notify(synth=self, index=i)
        synthDoneSpeaking.notify(synth=self)

    def _espeakTextToIPA(self, text: str) -> str:
        if not text:
            return ""
        textBuf = ctypes.create_unicode_buffer(text)
        textPtr = ctypes.c_void_p(ctypes.addressof(textBuf))
        chunks = []
        lastPtr = None
        while textPtr and textPtr.value:
            if lastPtr == textPtr.value:
                break
            lastPtr = textPtr.value
            phonemeBuf = _espeak.espeakDLL.espeak_TextToPhonemes(
                ctypes.byref(textPtr),
                _espeak.espeakCHARS_WCHAR,
                self._ESPEAK_PHONEME_MODE,
            )
            if phonemeBuf:
                chunks.append(ctypes.string_at(phonemeBuf))
            else:
                break
        ipaBytes = b"".join(chunks)
        try:
            return ipaBytes.decode("utf8", errors="ignore").strip()
        except Exception:
            return ""

    def _buildBlocks(self, speechSequence, coalesceSayAll: bool = False):
        """Convert an NVDA speechSequence into blocks: (text, [indexesAfterText], pitchOffset).

        When coalesceSayAll is True, we *delay* IndexCommands that occur mid-sentence.
        This prevents audible gaps when NVDA inserts index markers at visual line wraps
        during Say All.
        """
        blocks = []  # list[tuple[str, list[int], int]]
        textBuf = []
        pendingIndexes = []
        seenNonEmptyText = False

        pitchOffset = 0
        bufPitchOffset = pitchOffset

        def flush():
            nonlocal seenNonEmptyText, bufPitchOffset
            raw = _normalizeTextForEspeak(" ".join(textBuf))
            textBuf.clear()
            blocks.append((raw, pendingIndexes.copy(), bufPitchOffset))
            pendingIndexes.clear()
            seenNonEmptyText = False
            bufPitchOffset = pitchOffset

        for item in speechSequence:
            # Treat pitch changes as hard boundaries.
            if PitchCommand and isinstance(item, PitchCommand):
                if textBuf or pendingIndexes:
                    flush()
                pitchOffset = getattr(item, "offset", 0) or 0
                bufPitchOffset = pitchOffset
                continue

            if isinstance(item, str):
                if item:
                    if not textBuf and not pendingIndexes:
                        bufPitchOffset = pitchOffset
                    textBuf.append(item)
                    if item.strip():
                        seenNonEmptyText = True
                continue

            if IndexCommand and isinstance(item, IndexCommand):
                # Leading indexes (no text yet) should fire immediately.
                if not seenNonEmptyText and not textBuf:
                    blocks.append(("", [item.index], pitchOffset))
                    continue

                pendingIndexes.append(item.index)

                if not coalesceSayAll:
                    flush()
                    continue

                # Coalesce across wrapped lines: flush only at a "real" boundary
                # (sentence end) or if the buffer becomes too large.
                safeSoFar = _normalizeTextForEspeak(" ".join(textBuf))
                if (
                    _looksLikeSentenceEnd(safeSoFar)
                    or len(safeSoFar) >= _COALESCE_MAX_CHARS
                    or len(pendingIndexes) >= _COALESCE_MAX_INDEXES
                ):
                    flush()
                continue

            # Ignore other command types.

        # Trailing text (and/or delayed indexes)
        if textBuf or pendingIndexes:
            flush()

        # Remove trailing empty blocks with no indexes
        while blocks and (not blocks[-1][0]) and (not blocks[-1][1]):
            blocks.pop()

        return blocks

    def speak(self, speechSequence):
        indexes = []
        anyText = False
        for item in speechSequence:
            if IndexCommand and isinstance(item, IndexCommand):
                indexes.append(item.index)
            elif isinstance(item, str) and item.strip():
                anyText = True

        if (not anyText):
            self._enqueue(self._notifyIndexesAndDone, indexes)
            return

        self._enqueue(self._speakBg, list(speechSequence))

    def _speakBg(self, speakList):
        hadRealSpeech = False
        hasIndex = bool(IndexCommand) and any(isinstance(i, IndexCommand) for i in speakList)
        blocks = self._buildBlocks(speakList, coalesceSayAll=hasIndex)

        endPause = 0.0
        leadingSilenceMs = 1.0
        minFadeInMs = 3.0
        minFadeOutMs = 3.0
        lastStreamWasVoiced = False
        pauseMode = str(getattr(self, "_pauseMode", "short") or "short").strip().lower()

        def _punctuationPauseMs(punctToken: str | None) -> float:
            """Return pause duration in ms for punctuation.
            
            Short mode: subtle pauses for natural flow
            Long mode: deliberate pauses for clarity
            """
            if not punctToken or pauseMode == "off":
                return 0.0
            if punctToken in (".", "!", "?", "...", ":", ";"):
                return 60.0 if pauseMode == "long" else 35.0
            if punctToken == ",":
                return 10.0 if pauseMode == "long" else 5.0
            return 0.0

        for (text, indexesAfter, blockPitchOffset) in blocks:
            # Speak text for this block.
            if text:
                for chunk in re_textPause.split(text):
                    if not chunk:
                        continue

                    chunk = _normalizeTextForEspeak(chunk)
                    if not chunk:
                        continue

                    # Determine punctuation at the *end* of the chunk.
                    # This influences two things:
                    # - clauseType passed to the frontend (intonation hints)
                    # - optional micro-pause insertion after the chunk
                    punctToken = None
                    s = chunk.rstrip()
                    if s.endswith("..."):
                        punctToken = "..."
                        # Frontend only reads 1 byte; treat ellipsis as '.' for prosody.
                        clauseType = "."
                    elif s and (s[-1] in ".?!,:;"):
                        punctToken = s[-1]
                        clauseType = s[-1]
                    elif s and (s[-1] == ","):
                        punctToken = ","
                        clauseType = ","
                    else:
                        clauseType = None

                    punctPauseMs = _punctuationPauseMs(punctToken)

                    ipaText = self._espeakTextToIPA(chunk)
                    if not ipaText:
                        # Nothing speakable, but don't drop indexes (they are queued after the block).
                        continue

                    pitch = float(self._curPitch) + float(blockPitchOffset)
                    basePitch = 25.0 + (21.25 * (pitch / 12.5))

                    # nvspFrontend.dll: IPA -> frames.
                    queuedCount = 0

                    # Some generators (including nvspFrontend) may emit an initial silence
                    # frame for each queued utterance. When NVDA feeds us multiple chunks
                    # back-to-back (e.g. Say All reading "visual" lines), that redundant
                    # leading silence can become a perceptible pause. Once we've already
                    # queued real speech for this speak operation (or we're appending to
                    # existing output), suppress those leading silence frames.
                    suppressLeadingSilence = hadRealSpeech or bool(getattr(self._audio, "isSpeaking", False))
                    sawRealFrameInThisUtterance = False
                    sawSilenceAfterVoice = False

                    # Pre-calculate extra parameter multipliers once per utterance.
                    # This avoids repeated getattr(self, f"speechPlayer_{p}") lookups on every frame.
                    extraParamMultipliers = ()
                    if self.exposeExtraParams:
                        try:
                            names = getattr(self, "_extraParamNames", ()) or ()
                            attrNames = getattr(self, "_extraParamAttrNames", None)
                            if not attrNames or len(attrNames) != len(names):
                                attrNames = [f"speechPlayer_{x}" for x in names]

                            pairs = []
                            for paramName, attrName in zip(names, attrNames):
                                try:
                                    ratio = float(getattr(self, attrName, 50)) / 50.0
                                except Exception:
                                    continue
                                # Skip default (ratio=1.0) to keep the per-frame hot path tiny.
                                if ratio != 1.0:
                                    pairs.append((paramName, ratio))

                            extraParamMultipliers = tuple(pairs)
                        except Exception:
                            extraParamMultipliers = ()

                    def _onFrame(framePtr, frameDuration, fadeDuration, idxToSet):
                        nonlocal queuedCount, hadRealSpeech, sawRealFrameInThisUtterance, sawSilenceAfterVoice, lastStreamWasVoiced

                        # Reduce leading silence frames to minimize gaps
                        if (not framePtr) and suppressLeadingSilence and (not sawRealFrameInThisUtterance):
                            dur = min(float(frameDuration), float(leadingSilenceMs))
                            if dur > 0:
                                self._player.queueFrame(None, dur, min(float(fadeDuration), dur), userIndex=idxToSet)
                                queuedCount += 1
                                lastStreamWasVoiced = False
                            return

                        # Silence frame: queue as pause
                        if not framePtr:
                            if sawRealFrameInThisUtterance and (not sawSilenceAfterVoice):
                                fd = max(float(fadeDuration), float(minFadeOutMs))
                                if float(frameDuration) > 0:
                                    fd = min(fd, float(frameDuration))
                                else:
                                    fd = 0.0
                                fadeDuration = fd
                                sawSilenceAfterVoice = True
                            self._player.queueFrame(None, frameDuration, fadeDuration, userIndex=idxToSet)
                            queuedCount += 1
                            lastStreamWasVoiced = False
                            return

                        # Ensure fade-in on first voiced frame
                        if not sawRealFrameInThisUtterance:
                            fadeDuration = max(float(fadeDuration), float(minFadeInMs))

                        sawRealFrameInThisUtterance = True
                        hadRealSpeech = True

                        # Copy C frame to Python-owned Frame
                        frame = speechPlayer.Frame()
                        ctypes.memmove(ctypes.byref(frame), framePtr, ctypes.sizeof(speechPlayer.Frame))

                        applyVoiceToFrame(frame, self._curVoice)

                        if extraParamMultipliers:
                            for paramName, ratio in extraParamMultipliers:
                                setattr(frame, paramName, getattr(frame, paramName) * ratio)

                        frame.preFormantGain *= self._curVolume
                        self._player.queueFrame(frame, frameDuration, fadeDuration, userIndex=idxToSet)
                        queuedCount += 1
                        lastStreamWasVoiced = True

                    ok = False
                    try:
                        ok = self._frontend.queueIPA(
                            ipaText,
                            speed=self._curRate,
                            basePitch=basePitch,
                            inflection=self._curInflection,
                            clauseType=clauseType,
                            userIndex=None,
                            onFrame=_onFrame,
                        )
                    except Exception:
                        log.error("nvSpeechPlayer: frontend queueIPA failed", exc_info=True)
                        ok = False

                    if not ok:
                        err = self._frontend.getLastError()
                        if err:
                            log.error(f"nvSpeechPlayer: frontend error: {err}")

                    # If the frontend fails or outputs nothing, keep going (indexes are still queued).
                    if (not ok) or queuedCount <= 0:
                        continue

                    # Optional punctuation pause (micro-silence) after the clause.
                    # Insert only when we actually queued a voiced frame; otherwise we'd
                    # be adding silence after silence.
                    if punctPauseMs and sawRealFrameInThisUtterance:
                        try:
                            dur = float(punctPauseMs)
                            fd = float(min(float(minFadeOutMs), dur))
                            self._player.queueFrame(None, dur, fd)
                            lastStreamWasVoiced = False
                        except Exception:
                            log.debug("nvSpeechPlayer: failed inserting punctuation pause", exc_info=True)

            # Emit IndexCommands after this block
            if indexesAfter:
                for idx in indexesAfter:
                    try:
                        if lastStreamWasVoiced:
                            dur = float(minFadeOutMs)
                            self._player.queueFrame(None, dur, dur, userIndex=int(idx))
                            lastStreamWasVoiced = False
                        else:
                            self._player.queueFrame(None, 0.0, 0.0, userIndex=int(idx))
                    except Exception:
                        log.debug("nvSpeechPlayer: failed to queue index marker %r", idx, exc_info=True)

        if endPause and endPause > 0:
            self._player.queueFrame(None, float(endPause), min(float(endPause), 5.0))

        # Tiny tail fade to smooth utterance end
        if hadRealSpeech:
            self._player.queueFrame(None, 1.0, 1.0)

        self._audio.isSpeaking = True
        self._audio.kick()

    def cancel(self):
        """Cancel current speech immediately."""
        try:
            self._player.queueFrame(None, 3.0, 3.0, purgeQueue=True)
            self._audio.isSpeaking = False
            self._audio.kick()
            if self._audio and self._audio._wavePlayer:
                self._audio._wavePlayer.stop()
            self._audio._applyFadeIn = True
        except Exception:
            log.debug("nvSpeechPlayer: cancel failed", exc_info=True)

    def pause(self, switch):
        try:
            if self._audio and self._audio._wavePlayer:
                self._audio._wavePlayer.pause(switch)
        except Exception:
            log.debug("nvSpeechPlayer: pause failed", exc_info=True)

    def terminate(self):
        try:
            self.cancel()
            self._bgStop.set()
            self._bgQueue.put(None)
            self._bgThread.join(timeout=2.0)
            try:
                if getattr(self, "_frontend", None):
                    self._frontend.terminate()
            except Exception:
                log.debug("nvSpeechPlayer: frontend terminate failed", exc_info=True)
            self._audio.terminate()
            self._player.terminate()
            _espeak.terminate()
        except Exception:
            log.debug("nvSpeechPlayer: terminate failed", exc_info=True)

    def _get_rate(self):
        return int(math.log(self._curRate / 0.25, 2) * 25.0)

    def _set_rate(self, val):
        self._curRate = 0.25 * (2 ** (float(val) / 25.0))

    def _get_pitch(self):
        return int(self._curPitch)

    def _set_pitch(self, val):
        self._curPitch = int(val)

    def _get_volume(self):
        return int(self._curVolume * 75)

    def _set_volume(self, val):
        self._curVolume = float(val) / 75.0

    def _get_inflection(self):
        return int(self._curInflection / 0.01)

    def _set_inflection(self, val):
        self._curInflection = float(val) * 0.01

    def _get_voice(self):
        return self._curVoice

    def _set_voice(self, voice):
        if voice not in self.availableVoices:
            voice = "Adam"
        self._curVoice = voice
        if self.exposeExtraParams:
            for paramName in self._extraParamNames:
                setattr(self, f"speechPlayer_{paramName}", 50)

    def _getAvailableVoices(self):
        d = OrderedDict()
        for name in sorted(voices):
            d[name] = VoiceInfo(name, name)
        return d