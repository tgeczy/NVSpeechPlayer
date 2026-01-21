###
# This file is a part of the NV Speech Player project.
# URL: https://bitbucket.org/nvaccess/speechplayer
# Copyright 2014 NV Access Limited.
# GNU GPL v2
#
# Modernization patch:
# - Explicit ctypes prototypes for all exported DLL functions.
# - terminate() method so NVDA can clean up deterministically.
# - queueFrame() accepts durations in milliseconds, converts to samples (DLL expects samples).
# - Supports both 32-bit and 64-bit NVDA by loading DLLs from ./x86 or ./x64.
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
    c_void_p,
    cdll,
)
import os
from typing import Optional

speechPlayer_frameParam_t = c_double


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

        # int speechPlayer_synthesize(void* handle, uint numSamples, short* out);
        self._dll.speechPlayer_synthesize.argtypes = (c_void_p, c_uint, POINTER(c_short))
        self._dll.speechPlayer_synthesize.restype = c_int

        # int speechPlayer_getLastIndex(void* handle);
        self._dll.speechPlayer_getLastIndex.argtypes = (c_void_p,)
        self._dll.speechPlayer_getLastIndex.restype = c_int

        # void speechPlayer_terminate(void* handle);
        self._dll.speechPlayer_terminate.argtypes = (c_void_p,)
        self._dll.speechPlayer_terminate.restype = None

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

    def terminate(self) -> None:
        if getattr(self, "_speechHandle", None):
            try:
                self._dll.speechPlayer_terminate(self._speechHandle)
            except Exception:
                pass
            self._speechHandle = None

        if getattr(self, "_dllDirCookie", None):
            try:
                self._dllDirCookie.close()
            except Exception:
                pass
            self._dllDirCookie = None

    def __del__(self):
        try:
            self.terminate()
        except Exception:
            pass
