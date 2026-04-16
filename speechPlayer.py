###
# This file is a part of the TGSpeechBox project.
# URL: https://bitbucket.org/nvaccess/speechplayer
# Copyright 2014 NV Access Limited.
# GNU GPL v2
#
# Modernization patch:
# - Explicit ctypes prototypes for all exported DLL functions.
# - terminate() method so NVDA can clean up deterministically.
# - queueFrame() accepts durations in milliseconds, converts to samples (DLL expects samples).
# - Supports both 32-bit and 64-bit NVDA by loading DLLs from ./x86 or ./x64.
# - setVoicingTone() for DSP-level voice quality adjustments.
# - VoicingTone v2 struct with version detection and new pitch-sync params.
###

from __future__ import annotations

import ctypes
from ctypes import (
    Structure,
    POINTER,
    byref,
    c_double,
    c_int,
    c_short,
    c_uint,
    c_uint32,
    c_void_p,
    cdll,
)
import os
from typing import Optional

from logHandler import log

speechPlayer_frameParam_t = c_double

# VoicingTone struct versioning constants (must match voicingTone.h)
SPEECHPLAYER_VOICINGTONE_MAGIC = 0x32544F56  # "VOT2" in little-endian
SPEECHPLAYER_VOICINGTONE_VERSION = 3


class Frame(Structure):
    # Keep this field order exactly in sync with the C++ struct used to build speechPlayer.dll.
    # If you swap in an older DLL with a different struct layout, you'll get clicks/silence.
    _fields_ = [(name, speechPlayer_frameParam_t) for name in [
        "voicePitch",
        "vibratoPitchOffset",
        "vibratoSpeed",
        "voiceTurbulenceAmplitude",
        "glottalOpenQuotient",
        "voiceAmplitude",
        "aspirationAmplitude",
        "cf1", "cf2", "cf3", "cf4", "cf5", "cf6", "cfN0", "cfNP",
        "cb1", "cb2", "cb3", "cb4", "cb5", "cb6", "cbN0", "cbNP",
        "caNP",
        "fricationAmplitude",
        "pf1", "pf2", "pf3", "pf4", "pf5", "pf6",
        "pb1", "pb2", "pb3", "pb4", "pb5", "pb6",
        "pa1", "pa2", "pa3", "pa4", "pa5", "pa6",
        "parallelBypass",
        "preFormantGain",
        "outputGain",
        "endVoicePitch",
    ]]


class FrameEx(Structure):
    """Optional per-frame voice quality extensions (DSP v5+).
    
    These parameters are kept separate from Frame so the original 47-parameter
    ABI stays stable. All fields are expected in range [0.0, 1.0] unless noted.
    
    Use with queueFrameEx() for features like Danish stød (creaky voice),
    formant ramping (DECTalk-style transitions), and Fujisaki pitch contours.
    If you don't need these, just use queueFrame() as before.
    
    IMPORTANT: This struct must match nvspFrontend_FrameEx / speechPlayer_frameEx_t
    exactly (29 doubles = 232 bytes). Field order matters for ctypes.memmove().
    """
    _fields_ = [
        # Voice quality parameters (DSP v5)
        ("creakiness", c_double),   # Laryngealization / creaky voice (e.g. Danish stød)
        ("breathiness", c_double),  # Breath noise mixed into voicing
        ("jitter", c_double),       # Pitch period variation (irregular F0)
        ("shimmer", c_double),      # Amplitude variation (irregular loudness)
        ("sharpness", c_double),    # Glottal sharpness MULTIPLIER (0=SR default, 0.5-2.0 typical)
        
        # Formant end targets for within-frame ramping (DECTalk-style transitions)
        # NAN = no ramping (use base formant value throughout frame)
        # Any other value = ramp from base to this value over the frame duration
        ("endCf1", c_double),       # Cascade F1 end target (Hz), NAN = no ramp
        ("endCf2", c_double),       # Cascade F2 end target (Hz), NAN = no ramp
        ("endCf3", c_double),       # Cascade F3 end target (Hz), NAN = no ramp
        ("endPf1", c_double),       # Parallel F1 end target (Hz), NAN = no ramp
        ("endPf2", c_double),       # Parallel F2 end target (Hz), NAN = no ramp
        ("endPf3", c_double),       # Parallel F3 end target (Hz), NAN = no ramp
        
        # Fujisaki-Bartman pitch contour model (DSP v6+)
        # Enables Eloquence-style phrase/accent pitch shaping in the DSP
        ("fujisakiEnabled", c_double),   # 0.0 = off, >0.5 = on
        ("fujisakiReset", c_double),     # Rising edge resets model state
        ("fujisakiPhraseAmp", c_double), # Phrase command amplitude (e.g. 1.3)
        ("fujisakiPhraseLen", c_double), # Phrase filter L (samples). 0 = use default
        ("fujisakiAccentAmp", c_double), # Accent command amplitude (e.g. 0.4)
        ("fujisakiAccentDur", c_double), # Accent duration D (samples). 0 = use default
        ("fujisakiAccentLen", c_double), # Accent filter L (samples). 0 = use default

        # Per-parameter transition speed scales (DSP v7)
        # 0.0 = no override, <1.0 = reach target faster, 1.0 = normal fade rate
        ("transF1Scale", c_double),     # cf1, pf1, cb1, pb1
        ("transF2Scale", c_double),     # cf2, pf2, cb2, pb2
        ("transF3Scale", c_double),     # cf3, pf3, cb3, pb3
        ("transNasalScale", c_double),  # cfN0, cfNP, cbN0, cbNP, caNP

        # Amplitude crossfade mode (DSP v7.1)
        # 0.0 = linear (legacy), 1.0 = equal-power (sin/cos, prevents energy dips)
        ("transAmplitudeMode", c_double),

        # Higher cascade formants F7/F8 (DSP v8, Rabiner 1968 defaults)
        ("cf7", c_double),          # F7 frequency (Hz), default 6500.0
        ("cb7", c_double),          # F7 bandwidth (Hz), default 720.0
        ("cf8", c_double),          # F8 frequency (Hz), default 7500.0
        ("cb8", c_double),          # F8 bandwidth (Hz), default 1250.0

        # Source amplitude timing (DSP v8)
        # Delays fadeout of old noise sources during crossfade.
        ("transSourceHoldRatio", c_double),  # 0.0 = legacy, 0.3 = hold 30%

        # Voicing onset hold (DSP v8)
        # Delays ramp-in of new voicing during crossfade.
        ("transVoicingHoldRatio", c_double),  # 0.0 = legacy, 0.25 = hold 25%
    ]
    
    @classmethod
    def create(cls, creakiness: float = 0.0, breathiness: float = 0.0,
               jitter: float = 0.0, shimmer: float = 0.0,
               sharpness: float = 0.0) -> "FrameEx":
        """Create a FrameEx with specified voice quality values (all default to 0.0).
        
        Note: sharpness is a MULTIPLIER on the sample-rate-appropriate base value.
        0.0 = use SR default, 0.5 = softer, 1.0 = default, 2.0 = sharper.
        
        Formant end targets and Fujisaki fields are set to their neutral defaults
        (NAN for formants, 0.0 for Fujisaki = disabled).
        """
        import math
        ex = cls()
        ex.creakiness = creakiness
        ex.breathiness = breathiness
        ex.jitter = jitter
        ex.shimmer = shimmer
        ex.sharpness = sharpness
        # Formant end targets: NAN = no ramping
        ex.endCf1 = math.nan
        ex.endCf2 = math.nan
        ex.endCf3 = math.nan
        ex.endPf1 = math.nan
        ex.endPf2 = math.nan
        ex.endPf3 = math.nan
        # Fujisaki: 0.0 = disabled
        ex.fujisakiEnabled = 0.0
        ex.fujisakiReset = 0.0
        ex.fujisakiPhraseAmp = 0.0
        ex.fujisakiPhraseLen = 0.0
        ex.fujisakiAccentAmp = 0.0
        ex.fujisakiAccentDur = 0.0
        ex.fujisakiAccentLen = 0.0
        # Transition scales: 0.0 = no override (use base fade rate)
        ex.transF1Scale = 0.0
        ex.transF2Scale = 0.0
        ex.transF3Scale = 0.0
        ex.transNasalScale = 0.0
        # Amplitude crossfade: 0.0 = linear (legacy default)
        ex.transAmplitudeMode = 0.0
        # Higher cascade formants F7/F8 (Rabiner 1968 defaults)
        ex.cf7 = 6500.0
        ex.cb7 = 720.0
        ex.cf8 = 7500.0
        ex.cb8 = 1250.0
        return ex


class VoicingTone(Structure):
    """DSP-level voice quality parameters (v2 struct with version detection).
    
    These affect the wave generator's glottal pulse shape, voiced pre-emphasis,
    spectral tilt, high-shelf EQ, and pitch-synchronous F1 modulation.
    Set once per voice change, not per-frame.
    
    This struct matches speechPlayer_voicingTone_t in voicingTone.h (v2 layout).
    The magic/structSize/structVersion header enables backward compatibility
    with older DLLs that only know the original 7-field layout.
    """
    _fields_ = [
        # Version detection header
        ("magic", c_uint32),              # Must be SPEECHPLAYER_VOICINGTONE_MAGIC
        ("structSize", c_uint32),         # sizeof(VoicingTone)
        ("structVersion", c_uint32),      # Must be SPEECHPLAYER_VOICINGTONE_VERSION
        ("dspVersion", c_uint32),         # DSP version (informational)
        
        # Original v1 parameters
        ("voicingPeakPos", c_double),     # Glottal pulse peak position (0.85-0.95, default 0.91)
        ("voicedPreEmphA", c_double),     # Pre-emphasis coefficient (0.0-0.97, default 0.92)
        ("voicedPreEmphMix", c_double),   # Pre-emphasis mix (0.0-1.0, default 0.35)
        ("highShelfGainDb", c_double),    # High-shelf EQ gain in dB (default 4.0)
        ("highShelfFcHz", c_double),      # High-shelf corner frequency (default 2000.0)
        ("highShelfQ", c_double),         # High-shelf Q factor (default 0.7)
        ("voicedTiltDbPerOct", c_double), # Spectral tilt in dB/octave (default 0.0)
        
        # New v2 parameters
        ("noiseGlottalModDepth", c_double),  # Noise modulation by glottal cycle (0.0-1.0, default 0.0)
        ("pitchSyncF1DeltaHz", c_double),    # F1 delta during glottal open phase (default 0.0)
        ("pitchSyncB1DeltaHz", c_double),    # B1 delta during glottal open phase (default 0.0)
        
        # New v3 parameters
        ("speedQuotient", c_double),         # Glottal pulse asymmetry (0.5-4.0, default 2.0)
        ("aspirationTiltDbPerOct", c_double),  # Aspiration noise tilt (default 0.0)
        ("cascadeBwScale", c_double),            # Cascade bandwidth multiplier (0.4-1.4, default 1.0)
        ("tremorDepth", c_double),               # Tremor depth for elderly/shaky voice (0-0.5)

        # New v4 parameters — vocal tract shape
        ("nasalBwScale", c_double),              # Nasal resonator bandwidth multiplier (0.5-2.0, default 1.0)
        ("f4FreqScale", c_double),               # F4 frequency multiplier (0.7-1.5, default 1.0)
        ("nasalGainScale", c_double),            # Nasal pole coupling amplitude multiplier (0.5-1.5, default 1.0)

        # New v5 parameters — dual-oscillator chorus (vocal fold asymmetry)
        # MUST stay in sync with speechPlayer_voicingTone_t in src/voicingTone.h.
        # ctypes silently accepts attribute writes on undeclared fields, so missing
        # entries here cause slider values to be stored as Python-only attributes
        # that never reach the DLL — see issue #93 and commit ec60b11.
        ("chorusDepth", c_double),               # Chorus blend amount (0.0-1.0, default 0.0 = off)
        ("chorusDetuneHz", c_double),            # Chorus pitch offset in Hz (0.5-5.0, default 2.0)
    ]
    
    @classmethod
    def defaults(cls) -> "VoicingTone":
        """Return a VoicingTone with default values matching voicingTone.h."""
        tone = cls()
        # Version header
        tone.magic = SPEECHPLAYER_VOICINGTONE_MAGIC
        tone.structSize = ctypes.sizeof(cls)
        tone.structVersion = SPEECHPLAYER_VOICINGTONE_VERSION
        tone.dspVersion = 8  # Current DSP version
        
        # Original parameters
        tone.voicingPeakPos = 0.91
        tone.voicedPreEmphA = 0.92
        tone.voicedPreEmphMix = 0.35
        tone.highShelfGainDb = 4.0
        tone.highShelfFcHz = 2000.0
        tone.highShelfQ = 0.7
        tone.voicedTiltDbPerOct = 0.0
        
        # New v2 parameters
        tone.noiseGlottalModDepth = 0.0
        tone.pitchSyncF1DeltaHz = 0.0   # Slider 50 = neutral (0 Hz)
        tone.pitchSyncB1DeltaHz = 0.0   # Slider 50 = neutral (0 Hz)
        
        # New v3 parameters
        tone.speedQuotient = 2.0        # Neutral asymmetry
        tone.aspirationTiltDbPerOct = 0.0
        tone.cascadeBwScale = 1.0               # No scaling (default)
        tone.tremorDepth = 0.0                  # No tremor (default)

        # New v4 parameters — vocal tract shape
        tone.nasalBwScale = 1.0                 # No scaling (default)
        tone.f4FreqScale = 1.0                  # No scaling (default)
        tone.nasalGainScale = 1.0               # No scaling (default)

        # New v5 parameters — dual-oscillator chorus
        tone.chorusDepth = 0.0                  # Off by default
        tone.chorusDetuneHz = 2.0               # Natural vocal fold asymmetry range
        return tone


def _archFolderName() -> Optional[str]:
    """Return the subfolder name containing native DLLs for this Python process."""
    ptrSize = ctypes.sizeof(ctypes.c_void_p)
    if ptrSize == 4:
        return "x86"
    if ptrSize == 8:
        return "x64"
    return None


def getDllDir(baseDir: Optional[str] = None) -> str:
    """Return the directory that should contain speechPlayer.dll for this process."""
    here = baseDir or os.path.dirname(__file__)
    arch = _archFolderName()
    if arch:
        candidate = os.path.join(here, arch)
        if os.path.isdir(candidate):
            return candidate
    # Legacy layout: DLLs live next to this Python file.
    return here


# Exposed for other modules (e.g., __init__.py) to reuse if needed.
dllDir = getDllDir()
dllPath = os.path.join(dllDir, "speechPlayer.dll")


class SpeechPlayer(object):
    """Thin ctypes wrapper over speechPlayer.dll.

    queueFrame() expects minFrameDuration and fadeDuration in *milliseconds*.
    The DLL expects durations in *samples*.
    """

    def __init__(self, sampleRate: int):
        self.sampleRate = int(sampleRate)

        self._dllDirCookie = None
        # Python 3.8+ tightened Windows DLL search rules.
        # Ensure the folder containing speechPlayer.dll is on the DLL search path so its
        # dependencies can be found reliably.
        if hasattr(os, "add_dll_directory"):
            try:
                self._dllDirCookie = os.add_dll_directory(os.path.dirname(dllPath))
            except Exception:
                self._dllDirCookie = None

        self._dll = cdll.LoadLibrary(dllPath)
        self._setupPrototypes()
        
        # Sanity check: verify Frame struct size matches expected layout.
        # If this fails, the Python struct is out of sync with the DLL's C++ struct,
        # which would cause memory corruption, crashes, or weird audio artifacts.
        _expectedFrameSize = len(Frame._fields_) * ctypes.sizeof(c_double)
        _actualFrameSize = ctypes.sizeof(Frame)
        if _actualFrameSize != _expectedFrameSize:
            log.warning(
                f"TGSpeechBox: Frame struct size mismatch! "
                f"Expected {_expectedFrameSize} bytes ({len(Frame._fields_)} fields), "
                f"got {_actualFrameSize}. This may indicate ABI incompatibility."
            )

        self._speechHandle = self._dll.speechPlayer_initialize(self.sampleRate)
        if not self._speechHandle:
            raise RuntimeError("speechPlayer_initialize failed")

    def _setupPrototypes(self) -> None:
        # void* speechPlayer_initialize(int sampleRate);
        self._dll.speechPlayer_initialize.argtypes = (c_int,)
        self._dll.speechPlayer_initialize.restype = c_void_p

        # void speechPlayer_queueFrame(void* handle, Frame* frame, uint minSamples, uint fadeSamples,
        #                              int userIndex, bool purgeQueue);
        # Use c_int for purgeQueue (0/1) for ABI safety.
        self._dll.speechPlayer_queueFrame.argtypes = (
            c_void_p,
            POINTER(Frame),
            c_uint,
            c_uint,
            c_int,
            c_int,
        )
        self._dll.speechPlayer_queueFrame.restype = None

        # Extended frame queue (DSP v5+): optional per-frame voice quality params.
        # void speechPlayer_queueFrameEx(void* handle, Frame* frame, const FrameEx* frameEx,
        #                                uint frameExSize, uint minSamples, uint fadeSamples, 
        #                                int userIndex, bool purgeQueue);
        self._hasQueueFrameExApi = False
        try:
            _queueFrameEx = getattr(self._dll, "speechPlayer_queueFrameEx", None)
            if _queueFrameEx is not None:
                self._dll.speechPlayer_queueFrameEx.argtypes = (
                    c_void_p,
                    POINTER(Frame),
                    POINTER(FrameEx),
                    c_uint,         # frameExSize - size of FrameEx struct for ABI safety
                    c_uint,         # minSamples
                    c_uint,         # fadeSamples
                    c_int,          # userIndex
                    c_int,          # purgeQueue
                )
                self._dll.speechPlayer_queueFrameEx.restype = None
                self._hasQueueFrameExApi = True
        except (AttributeError, OSError):
            # Older DLL without frameEx support - that's fine
            pass

        # int speechPlayer_synthesize(void* handle, uint numSamples, short* out);
        self._dll.speechPlayer_synthesize.argtypes = (c_void_p, c_uint, POINTER(c_short))
        self._dll.speechPlayer_synthesize.restype = c_int

        # int speechPlayer_getLastIndex(void* handle);
        self._dll.speechPlayer_getLastIndex.argtypes = (c_void_p,)
        self._dll.speechPlayer_getLastIndex.restype = c_int

        # void speechPlayer_terminate(void* handle);
        self._dll.speechPlayer_terminate.argtypes = (c_void_p,)
        self._dll.speechPlayer_terminate.restype = None

        # Voicing tone API (optional - may not exist in older DLLs)
        self._hasVoicingToneApi = False
        try:
            # Check if the function exists by getting it explicitly
            # ctypes won't throw until you try to access a non-existent function
            _setTone = getattr(self._dll, "speechPlayer_setVoicingTone", None)
            _getTone = getattr(self._dll, "speechPlayer_getVoicingTone", None)
            
            if _setTone is not None and _getTone is not None:
                # void speechPlayer_setVoicingTone(void* handle, const VoicingTone* tone);
                self._dll.speechPlayer_setVoicingTone.argtypes = (c_void_p, POINTER(VoicingTone))
                self._dll.speechPlayer_setVoicingTone.restype = None
                
                # void speechPlayer_getVoicingTone(void* handle, VoicingTone* tone);
                self._dll.speechPlayer_getVoicingTone.argtypes = (c_void_p, POINTER(VoicingTone))
                self._dll.speechPlayer_getVoicingTone.restype = None
                
                self._hasVoicingToneApi = True
        except (AttributeError, OSError):
            # Older DLL without voicing tone support - that's fine
            pass

        # Output gain API (optional - may not exist in older DLLs)
        self._hasOutputGainApi = False
        try:
            _setGain = getattr(self._dll, "speechPlayer_setOutputGain", None)
            if _setGain is not None:
                self._dll.speechPlayer_setOutputGain.argtypes = (c_void_p, c_double)
                self._dll.speechPlayer_setOutputGain.restype = None
                self._hasOutputGainApi = True
        except (AttributeError, OSError):
            pass

        # Time-stretch API (optional - DSP-level rate boost via cycle skipping)
        self._hasTimeStretchApi = False
        try:
            _setTS = getattr(self._dll, "speechPlayer_setTimeStretch", None)
            if _setTS is not None:
                self._dll.speechPlayer_setTimeStretch.argtypes = (c_void_p, c_double)
                self._dll.speechPlayer_setTimeStretch.restype = None
                self._hasTimeStretchApi = True
        except (AttributeError, OSError):
            pass

    def setTimeStretch(self, factor: float) -> bool:
        """Set DSP-level time-stretch factor for rate boost.

        1.0 = normal, 2.0 = skip every other glottal cycle (2x speedup).
        Returns True if the API is available.
        """
        if not self._hasTimeStretchApi:
            return False
        self._dll.speechPlayer_setTimeStretch(self._speechHandle, c_double(factor))
        return True

    def setOutputGain(self, gain: float) -> bool:
        """Set output gain applied before the DSP limiter.

        This ensures consistent clipping/limiting behavior across platforms.
        Default is 1.0 (no gain). Returns True if the API is available.
        """
        if not self._hasOutputGainApi:
            return False
        self._dll.speechPlayer_setOutputGain(self._speechHandle, c_double(gain))
        return True

    def queueFrame(self, frame, minFrameDuration, fadeDuration, userIndex: int = -1, purgeQueue: bool = False) -> None:
        framePtr = byref(frame) if frame else None

        # Convert ms -> samples for the DLL.
        minSamples = int(float(minFrameDuration) * (self.sampleRate / 1000.0))
        fadeSamples = int(float(fadeDuration) * (self.sampleRate / 1000.0))

        if minSamples < 0:
            minSamples = 0
        if fadeSamples < 0:
            fadeSamples = 0

        self._dll.speechPlayer_queueFrame(
            self._speechHandle,
            framePtr,
            c_uint(minSamples),
            c_uint(fadeSamples),
            c_int(int(userIndex) if userIndex is not None else -1),
            c_int(1 if purgeQueue else 0),
        )

    def queueFrameEx(self, frame, frameEx, minFrameDuration, fadeDuration,
                     userIndex: int = -1, purgeQueue: bool = False) -> bool:
        """Queue a frame with optional extended voice quality parameters (DSP v5+).
        
        This is for features like Danish stød (creaky voice), breathiness, jitter,
        and shimmer that can't be expressed through the standard Frame parameters.
        
        Args:
            frame: Frame struct with standard parameters (or None for silence).
            frameEx: FrameEx struct with voice quality params, or None for defaults.
            minFrameDuration: Minimum duration in milliseconds.
            fadeDuration: Fade/interpolation duration in milliseconds.
            userIndex: Optional index for tracking (default -1).
            purgeQueue: If True, clear pending frames before queueing.
            
        Returns:
            True if frameEx was used, False if fell back to queueFrame (older DLL).
        """
        # Convert ms -> samples for the DLL.
        minSamples = int(float(minFrameDuration) * (self.sampleRate / 1000.0))
        fadeSamples = int(float(fadeDuration) * (self.sampleRate / 1000.0))

        if minSamples < 0:
            minSamples = 0
        if fadeSamples < 0:
            fadeSamples = 0

        framePtr = byref(frame) if frame else None
        
        # Try the extended API first
        if getattr(self, "_hasQueueFrameExApi", False):
            frameExPtr = byref(frameEx) if frameEx else None
            # Pass sizeof(FrameEx) so the DLL knows how much data to read safely
            frameExSize = ctypes.sizeof(FrameEx) if frameEx else 0
            self._dll.speechPlayer_queueFrameEx(
                self._speechHandle,
                framePtr,
                frameExPtr,
                c_uint(frameExSize),
                c_uint(minSamples),
                c_uint(fadeSamples),
                c_int(int(userIndex) if userIndex is not None else -1),
                c_int(1 if purgeQueue else 0),
            )
            return True
        
        # Fall back to legacy API (frameEx params will be ignored)
        self._dll.speechPlayer_queueFrame(
            self._speechHandle,
            framePtr,
            c_uint(minSamples),
            c_uint(fadeSamples),
            c_int(int(userIndex) if userIndex is not None else -1),
            c_int(1 if purgeQueue else 0),
        )
        return False

    def hasFrameExSupport(self) -> bool:
        """Check if the DLL supports extended frame parameters (DSP v5+)."""
        return getattr(self, "_hasQueueFrameExApi", False)

    def synthesize(self, numSamples: int):
        n = int(numSamples)
        if n <= 0:
            return None
        buf = (c_short * n)()
        res = self._dll.speechPlayer_synthesize(self._speechHandle, c_uint(n), buf)
        if res > 0:
            buf.length = min(int(res), n)
            return buf
        return None

    def getLastIndex(self) -> int:
        return int(self._dll.speechPlayer_getLastIndex(self._speechHandle))

    def hasVoicingToneSupport(self) -> bool:
        """Check if the DLL supports voicing tone adjustments."""
        return getattr(self, "_hasVoicingToneApi", False)

    def setVoicingTone(self, tone: Optional[VoicingTone]) -> bool:
        """Set DSP-level voice quality parameters.
        
        This affects the wave generator's glottal pulse shape, voiced pre-emphasis,
        high-shelf EQ, and pitch-synchronous F1 modulation. Call this once when 
        switching voices, not per-frame.
        
        Args:
            tone: VoicingTone struct with parameters, or None to reset to defaults.
            
        Returns:
            True on success, False if API not available.
        """
        if not getattr(self, "_hasVoicingToneApi", False):
            return False
        if not self._speechHandle:
            return False
        try:
            tonePtr = byref(tone) if tone else None
            self._dll.speechPlayer_setVoicingTone(self._speechHandle, tonePtr)
            return True
        except Exception:
            log.debug("TGSpeechBox: setVoicingTone failed", exc_info=True)
            return False

    def getVoicingTone(self) -> Optional[VoicingTone]:
        """Get current DSP-level voice quality parameters.
        
        Returns:
            VoicingTone struct with current values, or None if API not available.
        """
        if not getattr(self, "_hasVoicingToneApi", False):
            return None
        if not self._speechHandle:
            return None
        try:
            tone = VoicingTone()
            self._dll.speechPlayer_getVoicingTone(self._speechHandle, byref(tone))
            return tone
        except Exception:
            log.debug("TGSpeechBox: getVoicingTone failed", exc_info=True)
            return None

    def terminate(self) -> None:
        # First terminate the speech handle
        if getattr(self, "_speechHandle", None):
            try:
                self._dll.speechPlayer_terminate(self._speechHandle)
            except (OSError, AttributeError):
                # OSError: DLL call failed (DLL already unloaded)
                # AttributeError: _dll is None
                pass
            except Exception:
                log.debug("TGSpeechBox: speechPlayer_terminate raised unexpected error", exc_info=True)
            self._speechHandle = None

        # Close the DLL directory cookie (Python 3.8+)
        if getattr(self, "_dllDirCookie", None):
            try:
                self._dllDirCookie.close()
            except (OSError, AttributeError):
                pass
            except Exception:
                log.debug("TGSpeechBox: DLL directory cookie close failed", exc_info=True)
            self._dllDirCookie = None

        # Unload the DLL so the file can be replaced/deleted
        # Import here to avoid circular import issues
        if self._dll:
            try:
                from ._dll_utils import freeDll
                freeDll(self._dll)
            except (ImportError, OSError, AttributeError):
                # ImportError: module not available during shutdown
                # OSError: FreeLibrary failed
                # AttributeError: dll._handle is None
                pass
            except Exception:
                # Non-fatal - DLL will be unloaded when NVDA exits
                log.debug("TGSpeechBox: freeDll failed for speechPlayer.dll", exc_info=True)
        self._dll = None

    # NOTE: We intentionally do NOT implement __del__.
    # Python's __del__ is unreliable during interpreter shutdown (globals may be None).
    # Instead, we rely on NVDA calling terminate() explicitly during normal shutdown.
    # If NVDA dies rudely, the OS will reclaim our memory - that's fine.
