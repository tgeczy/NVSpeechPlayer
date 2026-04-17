# -*- coding: utf-8 -*-
"""ctypes wrapper for nvspFrontend.dll.

Keeping the low-level DLL glue in a dedicated module makes the synth driver's
``__init__.py`` easier to read/maintain.
"""

from __future__ import annotations

import ctypes
import os
from typing import Optional

from logHandler import log

from . import speechPlayer


# FrameEx struct matching nvspFrontend_FrameEx in the DLL (ABI v2+)
# IMPORTANT: Must match speechPlayer.FrameEx exactly (29 doubles = 232 bytes)
class FrameEx(ctypes.Structure):
    # >>> AUTO-GENERATED FROM src/frame.h - DO NOT EDIT MANUALLY (regenerate: python tools/gen_frame_ex.py) >>>
    _fields_ = [
        # Voice quality parameters (DSP v5)
        ("creakiness", ctypes.c_double),  # laryngealization / creaky voice (e.g. Danish stød)
        ("breathiness", ctypes.c_double),  # breath noise mixed into voicing
        ("jitter", ctypes.c_double),  # pitch period variation (irregular F0)
        ("shimmer", ctypes.c_double),  # amplitude variation (irregular loudness)
        ("sharpness", ctypes.c_double),  # glottal closure sharpness MULTIPLIER (0=use SR default, 0.5-2.0 typical)
        # Formant end targets for within-frame ramping (NAN = no ramp)
        ("endCf1", ctypes.c_double),  # Cascade F1 end target (Hz), NAN = no ramp
        ("endCf2", ctypes.c_double),  # Cascade F2 end target (Hz), NAN = no ramp
        ("endCf3", ctypes.c_double),  # Cascade F3 end target (Hz), NAN = no ramp
        ("endPf1", ctypes.c_double),  # Parallel F1 end target (Hz), NAN = no ramp
        ("endPf2", ctypes.c_double),  # Parallel F2 end target (Hz), NAN = no ramp
        ("endPf3", ctypes.c_double),  # Parallel F3 end target (Hz), NAN = no ramp
        # Fujisaki-Bartman pitch contour model (DSP v6+)
        ("fujisakiEnabled", ctypes.c_double),  # 0.0 = off (legacy behavior), >0.5 = on
        ("fujisakiReset", ctypes.c_double),  # rising edge resets model filter state
        ("fujisakiPhraseAmp", ctypes.c_double),  # phrase command amplitude (e.g. 1.3)
        ("fujisakiPhraseLen", ctypes.c_double),  # phrase filter L (samples to peak). 0 = use default
        ("fujisakiAccentAmp", ctypes.c_double),  # accent command amplitude (e.g. 0.4)
        ("fujisakiAccentDur", ctypes.c_double),  # accent duration D (samples). 0 = use default
        ("fujisakiAccentLen", ctypes.c_double),  # accent filter L (samples to peak). 0 = use default
        # Per-parameter transition speed scales (DSP v7). 0.0 = no override.
        ("transF1Scale", ctypes.c_double),  # cf1, pf1, cb1, pb1
        ("transF2Scale", ctypes.c_double),  # cf2, pf2, cb2, pb2
        ("transF3Scale", ctypes.c_double),  # cf3, pf3, cb3, pb3
        ("transNasalScale", ctypes.c_double),  # cfN0, cfNP, cbN0, cbNP, caNP
        # Amplitude crossfade mode (DSP v7.1). 0=linear, 1=equal-power.
        ("transAmplitudeMode", ctypes.c_double),
        # Higher cascade formants F7/F8 (DSP v8, Rabiner 1968 defaults)
        ("cf7", ctypes.c_double),  # F7 frequency (Hz).  Default 6500.0
        ("cb7", ctypes.c_double),  # F7 bandwidth (Hz).  Default 720.0
        ("cf8", ctypes.c_double),  # F8 frequency (Hz).  Default 7500.0
        ("cb8", ctypes.c_double),  # F8 bandwidth (Hz).  Default 1250.0
        # Source amplitude timing (DSP v8). 0.0 = legacy, no hold.
        ("transSourceHoldRatio", ctypes.c_double),
        # Voicing onset hold (DSP v8). 0.0 = legacy, no hold.
        ("transVoicingHoldRatio", ctypes.c_double),
    ]
    # <<< END AUTO-GENERATED <<<


# VoicingTone struct matching nvspFrontend_VoicingTone in the DLL (ABI v2+)
class VoicingTone(ctypes.Structure):
    _fields_ = [
        # >>> AUTO-GENERATED FROM src/voicingTone.h - DO NOT EDIT MANUALLY (regenerate: python tools/gen_voicing_tone.py) >>>
        # V1 parameters
        ("voicingPeakPos", ctypes.c_double),
        ("voicedPreEmphA", ctypes.c_double),
        ("voicedPreEmphMix", ctypes.c_double),
        ("highShelfGainDb", ctypes.c_double),
        ("highShelfFcHz", ctypes.c_double),
        ("highShelfQ", ctypes.c_double),
        ("voicedTiltDbPerOct", ctypes.c_double),
        # V2 parameters
        ("noiseGlottalModDepth", ctypes.c_double),
        ("pitchSyncF1DeltaHz", ctypes.c_double),
        ("pitchSyncB1DeltaHz", ctypes.c_double),
        # V3 parameters
        ("speedQuotient", ctypes.c_double),
        ("aspirationTiltDbPerOct", ctypes.c_double),
        ("cascadeBwScale", ctypes.c_double),
        ("tremorDepth", ctypes.c_double),
        # V4 parameters - vocal tract shape
        ("nasalBwScale", ctypes.c_double),
        ("f4FreqScale", ctypes.c_double),
        ("nasalGainScale", ctypes.c_double),
        # V5 parameters - dual-oscillator chorus (vocal fold asymmetry)
        ("chorusDepth", ctypes.c_double),
        ("chorusDetuneHz", ctypes.c_double),
        # <<< END AUTO-GENERATED <<<
    ]


# VoiceProfileSliders struct - the 13 user-adjustable slider values (ABI v2+)
class VoiceProfileSliders(ctypes.Structure):
    _fields_ = [
        # VoicingTone sliders (8)
        ("voicedTiltDbPerOct", ctypes.c_double),
        ("noiseGlottalModDepth", ctypes.c_double),
        ("pitchSyncF1DeltaHz", ctypes.c_double),
        ("pitchSyncB1DeltaHz", ctypes.c_double),
        ("speedQuotient", ctypes.c_double),
        ("aspirationTiltDbPerOct", ctypes.c_double),
        ("cascadeBwScale", ctypes.c_double),
        ("tremorDepth", ctypes.c_double),
        # FrameEx sliders (5)
        ("creakiness", ctypes.c_double),
        ("breathiness", ctypes.c_double),
        ("jitter", ctypes.c_double),
        ("shimmer", ctypes.c_double),
        ("sharpness", ctypes.c_double),
    ]


class NvspFrontend(object):
    """Thin ctypes wrapper around nvspFrontend.dll.

    - create(packDir) makes a handle.
    - setLanguage(langTag) loads packs for that language.
    - queueIPA() converts IPA -> frames and calls you back per frame.
    - queueIPA_Ex() is the extended version that also emits FrameEx data.

    All strings are UTF-8.
    """

    def __init__(self, dllPath: str, packDir: str):
        self._dllPath = dllPath
        self._packDir = packDir
        self._dll = None
        self._h = None
        self._dllDirCookie = None
        self._abiVersion = 1  # assume v1 until we check

        # Keep nvspFrontend.dll's directory on the DLL search path in case
        # it ever grows extra local dependencies.
        try:
            self._dllDirCookie = os.add_dll_directory(os.path.dirname(self._dllPath))
        except OSError:
            log.debug("TGSpeechBox: os.add_dll_directory failed for %r", self._dllPath, exc_info=True)
            self._dllDirCookie = None

        try:
            self._dll = ctypes.cdll.LoadLibrary(self._dllPath)
        except OSError:
            log.error("TGSpeechBox: failed to load nvspFrontend.dll from %r", self._dllPath, exc_info=True)
            raise

        self._setupPrototypes()

        packDirUtf8 = (self._packDir or "").encode("utf-8")
        try:
            self._h = self._dll.nvspFrontend_create(packDirUtf8)
        except Exception:
            log.error("TGSpeechBox: nvspFrontend_create raised an exception", exc_info=True)
            self._h = None

        if not self._h:
            err = self.getLastError() or "unknown error"
            raise RuntimeError(f"TGSpeechBox: nvspFrontend_create failed ({err})")

    def _setupPrototypes(self) -> None:
        # nvspFrontend_handle_t nvspFrontend_create(const char* packDirUtf8);
        self._dll.nvspFrontend_create.argtypes = [ctypes.c_char_p]
        self._dll.nvspFrontend_create.restype = ctypes.c_void_p

        # void nvspFrontend_destroy(nvspFrontend_handle_t handle);
        self._dll.nvspFrontend_destroy.argtypes = [ctypes.c_void_p]
        self._dll.nvspFrontend_destroy.restype = None

        # int nvspFrontend_setLanguage(nvspFrontend_handle_t handle, const char* langTagUtf8);
        self._dll.nvspFrontend_setLanguage.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        self._dll.nvspFrontend_setLanguage.restype = ctypes.c_int

        # const char* nvspFrontend_getLastError(nvspFrontend_handle_t handle);
        self._dll.nvspFrontend_getLastError.argtypes = [ctypes.c_void_p]
        self._dll.nvspFrontend_getLastError.restype = ctypes.c_char_p

        # int nvspFrontend_queueIPA(..., nvspFrontend_FrameCallback cb, void* userData);
        self._CBTYPE = ctypes.CFUNCTYPE(
            None,
            ctypes.c_void_p,  # userData
            ctypes.POINTER(speechPlayer.Frame),  # frameOrNull
            ctypes.c_double,  # durationMs
            ctypes.c_double,  # fadeMs
            ctypes.c_int,  # userIndexBase (we ignore and manage index on Python side)
        )

        self._dll.nvspFrontend_queueIPA.argtypes = [
            ctypes.c_void_p,  # handle
            ctypes.c_char_p,  # ipaUtf8
            ctypes.c_double,  # speed
            ctypes.c_double,  # basePitch
            ctypes.c_double,  # inflection
            ctypes.c_char_p,  # clauseTypeUtf8
            ctypes.c_int,  # userIndexBase
            self._CBTYPE,  # cb
            ctypes.c_void_p,  # userData
        ]
        self._dll.nvspFrontend_queueIPA.restype = ctypes.c_int

        # Voice profile API (optional - may not exist in older DLLs)
        self._hasVoiceProfileApi = False
        try:
            self._dll.nvspFrontend_setVoiceProfile.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
            self._dll.nvspFrontend_setVoiceProfile.restype = ctypes.c_int
            self._dll.nvspFrontend_getVoiceProfile.argtypes = [ctypes.c_void_p]
            self._dll.nvspFrontend_getVoiceProfile.restype = ctypes.c_char_p
            self._hasVoiceProfileApi = True
        except AttributeError:
            pass

        # Pack warnings API (optional - may not exist in older DLLs)
        self._hasPackWarningsApi = False
        try:
            self._dll.nvspFrontend_getPackWarnings.argtypes = [ctypes.c_void_p]
            self._dll.nvspFrontend_getPackWarnings.restype = ctypes.c_char_p
            self._hasPackWarningsApi = True
        except AttributeError:
            pass

        # FrameEx API (ABI v2+) - optional, may not exist in older DLLs
        self._hasFrameExApi = False
        try:
            # int nvspFrontend_getABIVersion(void);
            self._dll.nvspFrontend_getABIVersion.argtypes = []
            self._dll.nvspFrontend_getABIVersion.restype = ctypes.c_int
            self._abiVersion = self._dll.nvspFrontend_getABIVersion()
            
            if self._abiVersion >= 2:
                # void nvspFrontend_setFrameExDefaults(handle, creak, breath, jitter, shimmer, sharp);
                self._dll.nvspFrontend_setFrameExDefaults.argtypes = [
                    ctypes.c_void_p,  # handle
                    ctypes.c_double,  # creakiness
                    ctypes.c_double,  # breathiness
                    ctypes.c_double,  # jitter
                    ctypes.c_double,  # shimmer
                    ctypes.c_double,  # sharpness
                ]
                self._dll.nvspFrontend_setFrameExDefaults.restype = None

                # int nvspFrontend_getFrameExDefaults(handle, outDefaults);
                self._dll.nvspFrontend_getFrameExDefaults.argtypes = [
                    ctypes.c_void_p,  # handle
                    ctypes.POINTER(FrameEx),  # outDefaults
                ]
                self._dll.nvspFrontend_getFrameExDefaults.restype = ctypes.c_int

                # Extended callback type: includes FrameEx pointer
                self._CBTYPE_EX = ctypes.CFUNCTYPE(
                    None,
                    ctypes.c_void_p,  # userData
                    ctypes.POINTER(speechPlayer.Frame),  # frameOrNull
                    ctypes.POINTER(FrameEx),  # frameExOrNull
                    ctypes.c_double,  # durationMs
                    ctypes.c_double,  # fadeMs
                    ctypes.c_int,  # userIndexBase
                )

                # int nvspFrontend_queueIPA_Ex(...);
                self._dll.nvspFrontend_queueIPA_Ex.argtypes = [
                    ctypes.c_void_p,  # handle
                    ctypes.c_char_p,  # ipaUtf8
                    ctypes.c_double,  # speed
                    ctypes.c_double,  # basePitch
                    ctypes.c_double,  # inflection
                    ctypes.c_char_p,  # clauseTypeUtf8
                    ctypes.c_int,  # userIndexBase
                    self._CBTYPE_EX,  # cb
                    ctypes.c_void_p,  # userData
                ]
                self._dll.nvspFrontend_queueIPA_Ex.restype = ctypes.c_int

                # int nvspFrontend_getVoicingTone(handle, outTone);
                self._dll.nvspFrontend_getVoicingTone.argtypes = [
                    ctypes.c_void_p,  # handle
                    ctypes.POINTER(VoicingTone),  # outTone
                ]
                self._dll.nvspFrontend_getVoicingTone.restype = ctypes.c_int

                # const char* nvspFrontend_getVoiceProfileNames(handle);
                self._dll.nvspFrontend_getVoiceProfileNames.argtypes = [ctypes.c_void_p]
                self._dll.nvspFrontend_getVoiceProfileNames.restype = ctypes.c_char_p

                # int nvspFrontend_saveVoiceProfileSliders(handle, profileNameUtf8, sliders);
                self._dll.nvspFrontend_saveVoiceProfileSliders.argtypes = [
                    ctypes.c_void_p,  # handle
                    ctypes.c_char_p,  # profileNameUtf8
                    ctypes.POINTER(VoiceProfileSliders),  # sliders
                ]
                self._dll.nvspFrontend_saveVoiceProfileSliders.restype = ctypes.c_int

                self._hasFrameExApi = True
                log.debug("TGSpeechBox: frontend ABI v%d, FrameEx API available", self._abiVersion)

            # Text parser API (ABI v4+) — accepts original text for stress correction.
            self._hasTextParserApi = False
            self._hasPrepareTextApi = False
            if self._abiVersion >= 4:
                try:
                    self._dll.nvspFrontend_queueIPA_ExWithText.argtypes = [
                        ctypes.c_void_p,  # handle
                        ctypes.c_char_p,  # textUtf8
                        ctypes.c_char_p,  # ipaUtf8
                        ctypes.c_double,  # speed
                        ctypes.c_double,  # basePitch
                        ctypes.c_double,  # inflection
                        ctypes.c_char_p,  # clauseTypeUtf8
                        ctypes.c_int,  # userIndexBase
                        self._CBTYPE_EX,  # cb
                        ctypes.c_void_p,  # userData
                    ]
                    self._dll.nvspFrontend_queueIPA_ExWithText.restype = ctypes.c_int
                    self._hasTextParserApi = True
                    log.debug("TGSpeechBox: text parser API available (ABI v4+)")

                    # Pre-eSpeak text normalization API (also ABI v4+).
                    self._dll.nvspFrontend_prepareText.argtypes = [
                        ctypes.c_void_p,  # handle
                        ctypes.c_char_p,  # textUtf8
                    ]
                    # Use c_void_p (not c_char_p) to keep raw pointer for freeString.
                    self._dll.nvspFrontend_prepareText.restype = ctypes.c_void_p
                    self._dll.nvspFrontend_freeString.argtypes = [ctypes.c_void_p]
                    self._dll.nvspFrontend_freeString.restype = None
                    self._hasPrepareTextApi = True
                    log.debug("TGSpeechBox: prepareText API available")
                except AttributeError:
                    log.debug("TGSpeechBox: text parser API not available")
        except AttributeError:
            log.debug("TGSpeechBox: frontend FrameEx API not available (older DLL)")

    def terminate(self) -> None:
        # First destroy the frontend handle
        if self._dll and self._h:
            try:
                self._dll.nvspFrontend_destroy(self._h)
            except Exception:
                # Usually non-fatal (shutdown race), but log for diagnosability.
                log.debug("TGSpeechBox: nvspFrontend_destroy failed", exc_info=True)
        self._h = None

        # Close the DLL directory cookie (Python 3.8+)
        if getattr(self, "_dllDirCookie", None):
            try:
                self._dllDirCookie.close()
            except Exception:
                log.debug("TGSpeechBox: failed closing dll directory cookie", exc_info=True)
            self._dllDirCookie = None

        # Unload the DLL so the file can be replaced/deleted
        # Import here to avoid circular import issues
        if self._dll:
            try:
                from ._dll_utils import freeDll
                freeDll(self._dll)
            except Exception:
                # Non-fatal - DLL will be unloaded when NVDA exits
                log.debug("TGSpeechBox: freeDll failed for nvspFrontend.dll", exc_info=True)
        self._dll = None

    def getLastError(self) -> str:
        try:
            if not self._dll or not self._h:
                return ""
            msg = self._dll.nvspFrontend_getLastError(self._h)
            if not msg:
                return ""
            return msg.decode("utf-8", errors="replace")
        except Exception:
            log.debug("TGSpeechBox: getLastError failed", exc_info=True)
            return ""

    def getABIVersion(self) -> int:
        """Get the ABI version of the loaded frontend DLL."""
        return self._abiVersion

    def hasFrameExSupport(self) -> bool:
        """Check if the DLL supports FrameEx API (ABI v2+)."""
        return self._hasFrameExApi

    def setLanguage(self, langTag: str) -> bool:
        if not self._dll or not self._h:
            return False
        tag = (langTag or "").strip().lower().replace("_", "-")
        ok = int(self._dll.nvspFrontend_setLanguage(self._h, tag.encode("utf-8")))
        return bool(ok)

    def setVoiceProfile(self, profileName: str) -> bool:
        """Set the voice profile for parameter transformation.
        
        Args:
            profileName: Name of the profile (e.g., "female"). Empty string disables.
            
        Returns:
            True on success, False if API not available or call failed.
        """
        if not self._dll or not self._h:
            return False
        if not getattr(self, "_hasVoiceProfileApi", False):
            return False
        try:
            name = (profileName or "").encode("utf-8")
            ok = int(self._dll.nvspFrontend_setVoiceProfile(self._h, name))
            return bool(ok)
        except Exception:
            log.debug("TGSpeechBox: setVoiceProfile failed", exc_info=True)
            return False

    def getVoiceProfile(self) -> str:
        """Get the currently active voice profile name."""
        if not self._dll or not self._h:
            return ""
        if not getattr(self, "_hasVoiceProfileApi", False):
            return ""
        try:
            result = self._dll.nvspFrontend_getVoiceProfile(self._h)
            if not result:
                return ""
            return result.decode("utf-8", errors="replace")
        except Exception:
            log.debug("TGSpeechBox: getVoiceProfile failed", exc_info=True)
            return ""

    def getPackWarnings(self) -> str:
        """Get non-fatal warnings from pack loading."""
        if not self._dll or not self._h:
            return ""
        if not getattr(self, "_hasPackWarningsApi", False):
            return ""
        try:
            result = self._dll.nvspFrontend_getPackWarnings(self._h)
            if not result:
                return ""
            return result.decode("utf-8", errors="replace")
        except Exception:
            log.debug("TGSpeechBox: getPackWarnings failed", exc_info=True)
            return ""

    def hasVoiceProfileSupport(self) -> bool:
        """Check if the DLL supports voice profiles."""
        return getattr(self, "_hasVoiceProfileApi", False)

    def setFrameExDefaults(
        self,
        creakiness: float = 0.0,
        breathiness: float = 0.0,
        jitter: float = 0.0,
        shimmer: float = 0.0,
        sharpness: float = 1.0,
    ) -> bool:
        """Set user-level FrameEx defaults for voice quality.
        
        These values are mixed with per-phoneme values when emitting frames.
        Call this when the user adjusts voice quality sliders.
        
        Args:
            creakiness: 0.0-1.0, laryngealization/creaky voice
            breathiness: 0.0-1.0, breath noise in voicing
            jitter: 0.0-1.0, pitch period variation
            shimmer: 0.0-1.0, amplitude variation
            sharpness: multiplier (0.5-2.0 typical), glottal closure sharpness
            
        Returns:
            True on success, False if API not available.
        """
        if not self._dll or not self._h:
            return False
        if not self._hasFrameExApi:
            return False
        try:
            self._dll.nvspFrontend_setFrameExDefaults(
                self._h,
                float(creakiness),
                float(breathiness),
                float(jitter),
                float(shimmer),
                float(sharpness),
            )
            return True
        except Exception:
            log.debug("TGSpeechBox: setFrameExDefaults failed", exc_info=True)
            return False

    def getFrameExDefaults(self) -> Optional[FrameEx]:
        """Get the current FrameEx defaults.
        
        Returns:
            FrameEx struct with current defaults, or None if API not available.
        """
        if not self._dll or not self._h:
            return None
        if not self._hasFrameExApi:
            return None
        try:
            defaults = FrameEx()
            ok = self._dll.nvspFrontend_getFrameExDefaults(self._h, ctypes.byref(defaults))
            if ok:
                return defaults
            return None
        except Exception:
            log.debug("TGSpeechBox: getFrameExDefaults failed", exc_info=True)
            return None

    def queueIPA(
        self,
        ipaText: str,
        *,
        speed: float,
        basePitch: float,
        inflection: float,
        clauseType: Optional[str],
        userIndex: Optional[int],
        onFrame,
    ) -> bool:
        """Call onFrame(framePtrOrNone, durationMs, fadeMs, indexOrNone) for each frame.
        
        This is the legacy API that does not emit FrameEx data.
        Use queueIPA_Ex for the extended version.
        """
        if not self._dll or not self._h:
            return False

        ipaUtf8 = (ipaText or "").encode("utf-8")
        clauseUtf8 = None
        if clauseType:
            # Frontend reads the first byte only.
            clauseUtf8 = str(clauseType)[0].encode("ascii", errors="ignore") or b"."

        first = True

        @self._CBTYPE
        def _cb(userData, framePtr, durationMs, fadeMs, userIndexBase):
            nonlocal first

            # Only attach the NVDA index to the first *real* frame.
            # If the frontend emits a silence frame first (framePtr is None),
            # do not consume the "first" slot.
            idx = None
            if framePtr:
                if first and userIndex is not None:
                    idx = userIndex
                first = False

            onFrame(framePtr, float(durationMs), float(fadeMs), idx)

        ok = int(
            self._dll.nvspFrontend_queueIPA(
                self._h,
                ipaUtf8,
                float(speed),
                float(basePitch),
                float(inflection),
                clauseUtf8,
                int(-1),  # we manage index-per-first-frame in Python
                _cb,
                None,
            )
        )
        return bool(ok)

    def queueIPA_Ex(
        self,
        ipaText: str,
        *,
        speed: float,
        basePitch: float,
        inflection: float,
        clauseType: Optional[str],
        userIndex: Optional[int],
        onFrame,
    ) -> bool:
        """Call onFrame(framePtrOrNone, frameExPtrOrNone, durationMs, fadeMs, indexOrNone) for each frame.
        
        This is the extended API (ABI v2+) that emits FrameEx data alongside Frame data.
        The FrameEx values are the result of mixing per-phoneme values with user defaults
        set via setFrameExDefaults().
        
        Falls back to queueIPA (without FrameEx) if the DLL doesn't support it.
        
        Args:
            ipaText: IPA string to convert
            speed: Speech speed multiplier
            basePitch: Base pitch in Hz
            inflection: Pitch range scaling (0-1)
            clauseType: Punctuation type (".", ",", "?", "!")
            userIndex: NVDA index for text position mapping
            onFrame: Callback with signature (framePtr, frameExPtr, durationMs, fadeMs, index)
        """
        if not self._dll or not self._h:
            return False

        # Fall back to legacy API if FrameEx not supported
        if not self._hasFrameExApi:
            # Wrap callback to match legacy signature (no frameEx)
            def legacyOnFrame(framePtr, durationMs, fadeMs, idx):
                onFrame(framePtr, None, durationMs, fadeMs, idx)
            return self.queueIPA(
                ipaText,
                speed=speed,
                basePitch=basePitch,
                inflection=inflection,
                clauseType=clauseType,
                userIndex=userIndex,
                onFrame=legacyOnFrame,
            )

        ipaUtf8 = (ipaText or "").encode("utf-8")
        clauseUtf8 = None
        if clauseType:
            clauseUtf8 = str(clauseType)[0].encode("ascii", errors="ignore") or b"."

        first = True

        @self._CBTYPE_EX
        def _cb(userData, framePtr, frameExPtr, durationMs, fadeMs, userIndexBase):
            nonlocal first

            idx = None
            if framePtr:
                if first and userIndex is not None:
                    idx = userIndex
                first = False

            onFrame(framePtr, frameExPtr, float(durationMs), float(fadeMs), idx)

        ok = int(
            self._dll.nvspFrontend_queueIPA_Ex(
                self._h,
                ipaUtf8,
                float(speed),
                float(basePitch),
                float(inflection),
                clauseUtf8,
                int(-1),
                _cb,
                None,
            )
        )
        return bool(ok)

    def queueIPA_ExWithText(
        self,
        ipaText: str,
        *,
        originalText: str,
        speed: float,
        basePitch: float,
        inflection: float,
        clauseType: Optional[str],
        userIndex: Optional[int],
        onFrame,
    ) -> bool:
        """Like queueIPA_Ex but also passes original text for stress correction.

        If the DLL doesn't support the text parser API (ABI < 4), falls back
        to queueIPA_Ex (text is silently ignored).
        """
        if not self._hasTextParserApi:
            return self.queueIPA_Ex(
                ipaText,
                speed=speed,
                basePitch=basePitch,
                inflection=inflection,
                clauseType=clauseType,
                userIndex=userIndex,
                onFrame=onFrame,
            )

        if not self._dll or not self._h:
            return False

        textUtf8 = (originalText or "").encode("utf-8")
        ipaUtf8 = (ipaText or "").encode("utf-8")
        clauseUtf8 = None
        if clauseType:
            clauseUtf8 = str(clauseType)[0].encode("ascii", errors="ignore") or b"."

        first = True

        @self._CBTYPE_EX
        def _cb(userData, framePtr, frameExPtr, durationMs, fadeMs, userIndexBase):
            nonlocal first

            idx = None
            if framePtr:
                if first and userIndex is not None:
                    idx = userIndex
                first = False

            onFrame(framePtr, frameExPtr, float(durationMs), float(fadeMs), idx)

        ok = int(
            self._dll.nvspFrontend_queueIPA_ExWithText(
                self._h,
                textUtf8,
                ipaUtf8,
                float(speed),
                float(basePitch),
                float(inflection),
                clauseUtf8,
                int(-1),
                _cb,
                None,
            )
        )
        return bool(ok)

    def prepareText(self, text: str) -> str:
        """Pre-eSpeak text normalization.

        Applies compound splitting, date ordinals, and other text-level
        transforms before eSpeak phonemization.  The caller should feed
        the result to eSpeak for correct phonemization.

        Returns the original text unchanged if no transforms apply or
        the API is unavailable.
        """
        if not self._hasPrepareTextApi or not self._dll or not self._h:
            return text
        try:
            ptr = self._dll.nvspFrontend_prepareText(
                self._h, text.encode("utf-8")
            )
            if ptr:
                result = ctypes.string_at(ptr).decode("utf-8")
                self._dll.nvspFrontend_freeString(ptr)
                return result
        except Exception:
            pass
        return text

    # Backwards-compatible alias.
    splitCompounds = prepareText

    def getVoicingTone(self) -> Optional[VoicingTone]:
        """Get the voicing tone parameters for the current voice profile (ABI v2+).
        
        Returns:
            VoicingTone struct with DSP-level voice parameters, or None if:
            - API not available (older DLL)
            - No voice profile is active
            - Profile doesn't have voicingTone settings
            
        The returned struct contains the 12 voicing tone parameters that control
        glottal pulse shape, spectral tilt, and EQ at the DSP level.
        """
        if not self._dll or not self._h:
            return None
        if not self._hasFrameExApi:
            return None
        try:
            tone = VoicingTone()
            hasExplicit = self._dll.nvspFrontend_getVoicingTone(self._h, ctypes.byref(tone))
            # Return the tone even if hasExplicit is 0 (defaults are valid)
            # Caller can check hasExplicitVoicingTone() to know if profile had settings
            return tone
        except Exception:
            log.debug("TGSpeechBox: getVoicingTone failed", exc_info=True)
            return None

    def hasExplicitVoicingTone(self) -> bool:
        """Check if the current voice profile has explicit voicingTone settings.
        
        Returns True if the active profile has a voicingTone: block in YAML.
        This can be used to decide whether to use frontend voicing tone or
        fall back to Python-side defaults.
        """
        if not self._dll or not self._h:
            return False
        if not self._hasFrameExApi:
            return False
        try:
            tone = VoicingTone()
            hasExplicit = self._dll.nvspFrontend_getVoicingTone(self._h, ctypes.byref(tone))
            return bool(hasExplicit)
        except Exception:
            log.debug("TGSpeechBox: hasExplicitVoicingTone failed", exc_info=True)
            return False

    def getVoiceProfileNames(self) -> list:
        """Get list of voice profile names from the frontend (ABI v2+).
        
        Returns:
            List of profile name strings, or empty list if API not available.
            
        This can replace the Python-side discoverVoiceProfiles() function.
        """
        if not self._dll or not self._h:
            return []
        if not self._hasFrameExApi:
            return []
        try:
            result = self._dll.nvspFrontend_getVoiceProfileNames(self._h)
            if not result:
                return []
            # Parse newline-separated string
            names = result.decode("utf-8", errors="replace").strip().split('\n')
            return [n for n in names if n]  # Filter empty strings
        except Exception:
            log.debug("TGSpeechBox: getVoiceProfileNames failed", exc_info=True)
            return []

    def saveVoiceProfileSliders(
        self,
        profileName: str,
        voicedTiltDbPerOct: float,
        noiseGlottalModDepth: float,
        pitchSyncF1DeltaHz: float,
        pitchSyncB1DeltaHz: float,
        speedQuotient: float,
        aspirationTiltDbPerOct: float,
        cascadeBwScale: float,
        tremorDepth: float,
        creakiness: float,
        breathiness: float,
        jitter: float,
        shimmer: float,
        sharpness: float,
    ) -> bool:
        """Save voice profile slider values to phonemes.yaml (ABI v2+).
        
        Writes the 13 user-adjustable slider values to the voicingTone block
        for the specified profile in phonemes.yaml.
        
        Args:
            profileName: Name of the profile (e.g., "Adam", "Beth")
            voicedTiltDbPerOct: Spectral tilt in dB/octave
            noiseGlottalModDepth: Noise modulation (0.0-1.0)
            pitchSyncF1DeltaHz: Pitch-sync F1 delta in Hz
            pitchSyncB1DeltaHz: Pitch-sync B1 delta in Hz
            speedQuotient: Glottal speed quotient (0.5-4.0)
            aspirationTiltDbPerOct: Aspiration tilt in dB/octave
            cascadeBwScale: Global cascade bandwidth multiplier (0.4-1.4, 1.0 = neutral)
            tremorDepth: Voice tremor depth for elderly/shaky voice (0.0-0.5)
            creakiness: Laryngealization (0.0-1.0)
            breathiness: Breathiness (0.0-1.0)
            jitter: Pitch variation (0.0-1.0)
            shimmer: Amplitude variation (0.0-1.0)
            sharpness: Glottal sharpness multiplier (0.5-2.0)
            
        Returns:
            True on success, False on failure (check getLastError for details)
        """
        if not self._dll or not self._h:
            return False
        if not self._hasFrameExApi:
            return False
        try:
            profileNameUtf8 = (profileName or "").encode("utf-8")
            
            sliders = VoiceProfileSliders()
            sliders.voicedTiltDbPerOct = float(voicedTiltDbPerOct)
            sliders.noiseGlottalModDepth = float(noiseGlottalModDepth)
            sliders.pitchSyncF1DeltaHz = float(pitchSyncF1DeltaHz)
            sliders.pitchSyncB1DeltaHz = float(pitchSyncB1DeltaHz)
            sliders.speedQuotient = float(speedQuotient)
            sliders.aspirationTiltDbPerOct = float(aspirationTiltDbPerOct)
            sliders.cascadeBwScale = float(cascadeBwScale)
            sliders.tremorDepth = float(tremorDepth)
            sliders.creakiness = float(creakiness)
            sliders.breathiness = float(breathiness)
            sliders.jitter = float(jitter)
            sliders.shimmer = float(shimmer)
            sliders.sharpness = float(sharpness)
            
            result = self._dll.nvspFrontend_saveVoiceProfileSliders(
                self._h, profileNameUtf8, ctypes.byref(sliders)
            )
            return bool(result)
        except Exception:
            log.debug("TGSpeechBox: saveVoiceProfileSliders failed", exc_info=True)
            return False
