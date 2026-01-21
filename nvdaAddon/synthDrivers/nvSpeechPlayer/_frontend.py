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


class NvspFrontend(object):
    """Thin ctypes wrapper around nvspFrontend.dll.

    - create(packDir) makes a handle.
    - setLanguage(langTag) loads packs for that language.
    - queueIPA() converts IPA -> frames and calls you back per frame.

    All strings are UTF-8.
    """

    def __init__(self, dllPath: str, packDir: str):
        self._dllPath = dllPath
        self._packDir = packDir
        self._dll = None
        self._h = None
        self._dllDirCookie = None

        # Python 3.8+ tightened Windows DLL search rules. If nvspFrontend.dll ever
        # grows extra local dependencies, keeping its directory on the DLL search
        # path makes loading more reliable.
        if hasattr(os, "add_dll_directory"):
            try:
                self._dllDirCookie = os.add_dll_directory(os.path.dirname(self._dllPath))
            except OSError:
                log.debug("nvSpeechPlayer: os.add_dll_directory failed for %r", self._dllPath, exc_info=True)
                self._dllDirCookie = None
            except Exception:
                # Be defensive: this should never crash the synth driver.
                log.debug("nvSpeechPlayer: unexpected error in os.add_dll_directory", exc_info=True)
                self._dllDirCookie = None

        try:
            self._dll = ctypes.cdll.LoadLibrary(self._dllPath)
        except OSError:
            log.error("nvSpeechPlayer: failed to load nvspFrontend.dll from %r", self._dllPath, exc_info=True)
            raise

        self._setupPrototypes()

        packDirUtf8 = (self._packDir or "").encode("utf-8")
        try:
            self._h = self._dll.nvspFrontend_create(packDirUtf8)
        except Exception:
            log.error("nvSpeechPlayer: nvspFrontend_create raised an exception", exc_info=True)
            self._h = None

        if not self._h:
            err = self.getLastError() or "unknown error"
            raise RuntimeError(f"nvSpeechPlayer: nvspFrontend_create failed ({err})")

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

    def terminate(self) -> None:
        if self._dll and self._h:
            try:
                self._dll.nvspFrontend_destroy(self._h)
            except Exception:
                # Usually non-fatal (shutdown race), but log for diagnosability.
                log.debug("nvSpeechPlayer: nvspFrontend_destroy failed", exc_info=True)
        self._h = None

        if getattr(self, "_dllDirCookie", None):
            try:
                self._dllDirCookie.close()
            except Exception:
                log.debug("nvSpeechPlayer: failed closing dll directory cookie", exc_info=True)
            self._dllDirCookie = None

    def getLastError(self) -> str:
        try:
            if not self._dll or not self._h:
                return ""
            msg = self._dll.nvspFrontend_getLastError(self._h)
            if not msg:
                return ""
            return msg.decode("utf-8", errors="replace")
        except Exception:
            log.debug("nvSpeechPlayer: getLastError failed", exc_info=True)
            return ""

    def setLanguage(self, langTag: str) -> bool:
        if not self._dll or not self._h:
            return False
        tag = (langTag or "").strip().lower().replace("_", "-")
        ok = int(self._dll.nvspFrontend_setLanguage(self._h, tag.encode("utf-8")))
        return bool(ok)

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
        """Call onFrame(framePtrOrNone, durationMs, fadeMs, indexOrNone) for each frame."""
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
