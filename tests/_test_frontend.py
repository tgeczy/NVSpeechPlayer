"""Minimal standalone ctypes wrapper for nvspFrontend.dll.

This is the test-suite-only mirror of nvdaAddon/synthDrivers/tgSpeechBox/_frontend.py,
stripped of the NVDA dependency (no `from logHandler import log`) so it can run
under plain Python. Only wraps the API surface the tests actually use.

If you add a test that needs a new DLL function, add the prototype + thin
Python wrapper here.
"""
from __future__ import annotations

import ctypes
import json
from collections import namedtuple
from ctypes import (
    c_char_p, c_void_p, c_int, c_double,
    CFUNCTYPE, POINTER, Structure,
)
from typing import Callable, List, Optional

# Data domain constants (must match NVSP_DATA_* in src/frontend/nvspFrontend.h).
NVSP_DATA_FRAMETRACE = 3

# One entry per phoneme emitted during the most recent queueIPA_Ex call.
# frame_index: position into the FrameRecord list that capture_frames() returned.
#              records[frame_index] is the first frame of this phoneme.
# phoneme_key: UTF-8 IPA string (e.g. "ɣ", "l", "a").
TraceEntry = namedtuple("TraceEntry", ["frame_index", "phoneme_key"])

# Frame struct (47 doubles, ABI-stable since v1).
# Field names match nvspFrontend_Frame in src/frontend/nvspFrontend.h.
class Frame(Structure):
    _fields_ = [(name, c_double) for name in [
        "voicePitch", "vibratoPitchOffset", "vibratoSpeed",
        "voiceTurbulenceAmplitude", "glottalOpenQuotient",
        "voiceAmplitude", "aspirationAmplitude",
        "cf1", "cf2", "cf3", "cf4", "cf5", "cf6", "cfN0", "cfNP",
        "cb1", "cb2", "cb3", "cb4", "cb5", "cb6", "cbN0", "cbNP",
        "caNP",
        "fricationAmplitude",
        "pf1", "pf2", "pf3", "pf4", "pf5", "pf6",
        "pb1", "pb2", "pb3", "pb4", "pb5", "pb6",
        "pa1", "pa2", "pa3", "pa4", "pa5", "pa6",
        "parallelBypass", "preFormantGain", "outputGain",
        "endVoicePitch",
    ]]


# Tests don't currently introspect FrameEx fields, so we treat it as opaque
# (void*). If a future test needs FrameEx access, mirror the struct here using
# the same codegen pattern as src/frame.h (or import the codegen output).
FrameExCallback = CFUNCTYPE(
    None,
    c_void_p,                # userData
    POINTER(Frame),          # frameOrNull (NULL = silence)
    c_void_p,                # frameExOrNull (opaque to tests)
    c_double,                # durationMs
    c_double,                # fadeMs
    c_int,                   # userIndex
)


class FrameRecord:
    """A single captured callback invocation."""
    __slots__ = ("is_silence", "frame_dict", "duration_ms", "fade_ms", "user_index")

    def __init__(self, is_silence: bool, frame_dict: Optional[dict],
                 duration_ms: float, fade_ms: float, user_index: int):
        self.is_silence = is_silence
        self.frame_dict = frame_dict
        self.duration_ms = duration_ms
        self.fade_ms = fade_ms
        self.user_index = user_index

    def __repr__(self) -> str:
        if self.is_silence:
            return f"<silence {self.duration_ms:.1f}ms>"
        f = self.frame_dict
        return (f"<frame pitch={f['voicePitch']:.0f}Hz "
                f"F1={f['cf1']:.0f} F2={f['cf2']:.0f} F3={f['cf3']:.0f} "
                f"voiceAmp={f['voiceAmplitude']:.2f} "
                f"fricAmp={f['fricationAmplitude']:.2f} "
                f"{self.duration_ms:.1f}ms>")


class TestFrontend:
    """Owns one loaded copy of nvspFrontend.dll. Create one per test session."""

    def __init__(self, dll_path: str):
        self._dll = ctypes.CDLL(dll_path)
        self._setup_prototypes()
        # Keep callback objects alive for the duration of any in-flight call.
        # ctypes will GC them otherwise and the DLL gets a dangling pointer.
        self._cb_keepalive: List = []

    def _setup_prototypes(self) -> None:
        d = self._dll

        d.nvspFrontend_create.argtypes = [c_char_p]
        d.nvspFrontend_create.restype = c_void_p

        d.nvspFrontend_destroy.argtypes = [c_void_p]
        d.nvspFrontend_destroy.restype = None

        d.nvspFrontend_setLanguage.argtypes = [c_void_p, c_char_p]
        d.nvspFrontend_setLanguage.restype = c_int

        d.nvspFrontend_prepareText.argtypes = [c_void_p, c_char_p]
        d.nvspFrontend_prepareText.restype = c_void_p  # malloc'd char* — we copy + free

        d.nvspFrontend_freeString.argtypes = [c_void_p]
        d.nvspFrontend_freeString.restype = None

        d.nvspFrontend_queueIPA_Ex.argtypes = [
            c_void_p, c_char_p,
            c_double, c_double, c_double,
            c_char_p, c_int,
            FrameExCallback, c_void_p,
        ]
        d.nvspFrontend_queueIPA_Ex.restype = c_int

        d.nvspFrontend_getLastError.argtypes = [c_void_p]
        d.nvspFrontend_getLastError.restype = c_char_p

        d.nvspFrontend_getABIVersion.argtypes = []
        d.nvspFrontend_getABIVersion.restype = c_int

        d.nvspFrontend_queryData.argtypes = [
            c_void_p, c_int, c_char_p, c_int, c_int,
        ]
        d.nvspFrontend_queryData.restype = c_void_p  # malloc'd — copy + free

    # ---- Lifecycle ----

    def create_handle(self, pack_dir: str) -> int:
        h = self._dll.nvspFrontend_create(pack_dir.encode("utf-8"))
        if not h:
            raise RuntimeError(f"nvspFrontend_create failed for packDir={pack_dir!r}")
        return h

    def destroy_handle(self, handle: int) -> None:
        self._dll.nvspFrontend_destroy(handle)

    def set_language(self, handle: int, lang_tag: str) -> None:
        ok = self._dll.nvspFrontend_setLanguage(handle, lang_tag.encode("utf-8"))
        if not ok:
            err = self._dll.nvspFrontend_getLastError(handle)
            err_str = err.decode("utf-8", "replace") if err else "(unknown)"
            raise RuntimeError(f"setLanguage({lang_tag!r}) failed: {err_str}")

    def abi_version(self) -> int:
        return self._dll.nvspFrontend_getABIVersion()

    # ---- Text preparation (text -> text-for-eSpeak) ----

    def prepare_text(self, handle: int, text: str) -> str:
        """Returns the preprocessed text the frontend would feed to eSpeak.

        The C API has a quirk: it returns NULL when no transformation is
        needed (input == output). Real callers (Android JNI, iOS bridge,
        NVDA driver) interpret NULL as "send the original text to eSpeak
        unchanged." We mirror that here so tests can assert against the
        actual eSpeak-bound text without caring which path produced it.

        Used to test text-normalization behavior (letter dict lookups,
        thousands-separator handling, year splitting, compound merging) without
        invoking the synthesis pipeline at all.
        """
        ptr = self._dll.nvspFrontend_prepareText(handle, text.encode("utf-8"))
        if not ptr:
            return text  # NULL == "no change needed"
        try:
            result = ctypes.string_at(ptr).decode("utf-8", "replace")
        finally:
            self._dll.nvspFrontend_freeString(ptr)
        return result

    # ---- IPA -> Frame stream capture ----

    def capture_frames(
        self,
        handle: int,
        ipa: str,
        speed: float = 50.0,
        base_pitch: float = 140.0,
        inflection: float = 0.5,
        clause_type: str = ".",
    ) -> List[FrameRecord]:
        """Synthesize IPA and capture every frame callback into a list.

        Returns a list of FrameRecord objects in emission order. Silence frames
        have is_silence=True and frame_dict=None.

        Use this to test phoneme-level regressions (was a phoneme emitted at
        all? did its formants land in the expected range?) without playing audio.
        """
        records: List[FrameRecord] = []

        def _cb(user_data, frame_ptr, frame_ex_ptr, duration_ms, fade_ms, user_index):
            if frame_ptr:
                f = ctypes.cast(frame_ptr, POINTER(Frame)).contents
                d = {name: getattr(f, name) for name, _ in Frame._fields_}
                records.append(FrameRecord(False, d, duration_ms, fade_ms, user_index))
            else:
                records.append(FrameRecord(True, None, duration_ms, fade_ms, user_index))

        cb = FrameExCallback(_cb)
        self._cb_keepalive.append(cb)
        try:
            ok = self._dll.nvspFrontend_queueIPA_Ex(
                handle, ipa.encode("utf-8"),
                c_double(speed), c_double(base_pitch), c_double(inflection),
                clause_type.encode("utf-8"),
                c_int(0), cb, None,
            )
            if not ok:
                err = self._dll.nvspFrontend_getLastError(handle)
                err_str = err.decode("utf-8", "replace") if err else "(unknown)"
                raise RuntimeError(f"queueIPA_Ex({ipa!r}) failed: {err_str}")
        finally:
            self._cb_keepalive.remove(cb)
        return records


def voiced_frames(records: List[FrameRecord]) -> List[FrameRecord]:
    """Filter to non-silence frames with audible voicing (excludes pure stops/silence)."""
    return [r for r in records
            if not r.is_silence
            and r.frame_dict
            and r.frame_dict["voiceAmplitude"] > 0.05]


def read_frame_trace(fe: "TestFrontend", handle: int) -> List[TraceEntry]:
    """Return the per-utterance phoneme→frame-index trace from the most recent
    capture_frames() call on this handle.

    Each entry's frame_index is the position into the FrameRecord list returned
    by capture_frames() — records[entry.frame_index] is the first frame of that
    phoneme. Silence/gap tokens are NOT included.

    Use to pin word-context acoustic regressions: synthesize a whole word, then
    pick out the frame for a specific phoneme by its IPA key to assert on
    formants / amplitudes without being drowned out by vowel frames.
    """
    ptr = fe._dll.nvspFrontend_queryData(
        handle, c_int(NVSP_DATA_FRAMETRACE), b"", c_int(0), c_int(0),
    )
    if not ptr:
        return []
    try:
        raw = ctypes.string_at(ptr).decode("utf-8", "replace")
    finally:
        fe._dll.nvspFrontend_freeString(ptr)
    data = json.loads(raw)
    return [TraceEntry(int(e["frameIndex"]), e["phonemeKey"]) for e in data]
