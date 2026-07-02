"""
Standalone desktop render harness for TGSpeechBox.

Drives the REAL engine (nvspFrontend.dll + speechPlayer.dll, 64-bit from
build-x64/MinSizeRel) from IPA -> WAV, reading the live pack at a given
packDir. No NVDA / logHandler dependency (mirrors the ctypes prototypes
from speechPlayer.py + _frontend.py).

Purpose: prototype Spanish /s/ robustness without rebuilding the APK, and
cross-validate Windows output against the Android device.

Usage:
    python render_tgsb.py [packDir] [outDir]
Renders the 6 szc test words at es-mx into outDir/<base>.wav.
"""
import ctypes
import os
import sys
import subprocess
import wave
from ctypes import (c_double, c_int, c_uint, c_short, c_void_p, c_char_p,
                    POINTER, CFUNCTYPE, Structure)

DLL_DIR = r"C:\git\TGSpeechBox\build-x64\MinSizeRel"
DEFAULT_PACK = r"C:\git\TGSpeechBox\packs"
ESPEAK = r"C:\Program Files\eSpeak NG\espeak-ng.exe"

FRAME_FIELDS = [
    "voicePitch", "vibratoPitchOffset", "vibratoSpeed",
    "voiceTurbulenceAmplitude", "glottalOpenQuotient", "voiceAmplitude",
    "aspirationAmplitude",
    "cf1", "cf2", "cf3", "cf4", "cf5", "cf6", "cfN0", "cfNP",
    "cb1", "cb2", "cb3", "cb4", "cb5", "cb6", "cbN0", "cbNP", "caNP",
    "fricationAmplitude",
    "pf1", "pf2", "pf3", "pf4", "pf5", "pf6",
    "pb1", "pb2", "pb3", "pb4", "pb5", "pb6",
    "pa1", "pa2", "pa3", "pa4", "pa5", "pa6",
    "parallelBypass", "preFormantGain", "outputGain", "endVoicePitch",
]
FRAMEEX_FIELDS = [
    "creakiness", "breathiness", "jitter", "shimmer", "sharpness",
    "endCf1", "endCf2", "endCf3", "endPf1", "endPf2", "endPf3",
    "fujisakiEnabled", "fujisakiReset", "fujisakiPhraseAmp", "fujisakiPhraseLen",
    "fujisakiAccentAmp", "fujisakiAccentDur", "fujisakiAccentLen",
    "transF1Scale", "transF2Scale", "transF3Scale", "transNasalScale",
    "transAmplitudeMode", "cf7", "cb7", "cf8", "cb8",
    "transSourceHoldRatio", "transVoicingHoldRatio", "fricationTiltDb",
]


class Frame(Structure):
    _fields_ = [(n, c_double) for n in FRAME_FIELDS]


class FrameEx(Structure):
    _fields_ = [(n, c_double) for n in FRAMEEX_FIELDS]


CB = CFUNCTYPE(None, c_void_p, POINTER(Frame), POINTER(FrameEx),
               c_double, c_double, c_int)


def get_ipa(word, voice="es"):
    r = subprocess.run([ESPEAK, "-v", voice, "-q", "--ipa", word],
                       capture_output=True, text=True, encoding="utf-8")
    return r.stdout.strip()


class Renderer:
    def __init__(self, pack_dir=DEFAULT_PACK, lang="es-mx", sr=22050):
        self.sr = sr
        self._cookie = os.add_dll_directory(DLL_DIR)
        self.fe = ctypes.CDLL(os.path.join(DLL_DIR, "nvspFrontend.dll"))
        self.sp = ctypes.CDLL(os.path.join(DLL_DIR, "speechPlayer.dll"))
        fe, sp = self.fe, self.sp
        fe.nvspFrontend_create.argtypes = [c_char_p]
        fe.nvspFrontend_create.restype = c_void_p
        fe.nvspFrontend_setLanguage.argtypes = [c_void_p, c_char_p]
        fe.nvspFrontend_setLanguage.restype = c_int
        fe.nvspFrontend_getLastError.argtypes = [c_void_p]
        fe.nvspFrontend_getLastError.restype = c_char_p
        fe.nvspFrontend_queueIPA_Ex.argtypes = [
            c_void_p, c_char_p, c_double, c_double, c_double,
            c_char_p, c_int, CB, c_void_p]
        fe.nvspFrontend_queueIPA_Ex.restype = c_int
        sp.speechPlayer_initialize.argtypes = [c_int]
        sp.speechPlayer_initialize.restype = c_void_p
        sp.speechPlayer_queueFrameEx.argtypes = [
            c_void_p, POINTER(Frame), POINTER(FrameEx),
            c_uint, c_uint, c_uint, c_int, c_int]
        sp.speechPlayer_queueFrameEx.restype = None
        sp.speechPlayer_synthesize.argtypes = [c_void_p, c_uint, POINTER(c_short)]
        sp.speechPlayer_synthesize.restype = c_int
        sp.speechPlayer_terminate.argtypes = [c_void_p]
        sp.speechPlayer_terminate.restype = None

        self.h = fe.nvspFrontend_create(pack_dir.encode("utf-8"))
        if not self.h:
            raise RuntimeError("nvspFrontend_create failed for %r" % pack_dir)
        if not fe.nvspFrontend_setLanguage(self.h, lang.encode("utf-8")):
            err = fe.nvspFrontend_getLastError(self.h)
            raise RuntimeError("setLanguage(%s) failed: %s"
                               % (lang, err.decode() if err else "?"))

    def render(self, ipa, out_wav, speed=1.0, pitch=140.0, infl=0.5):
        player = self.sp.speechPlayer_initialize(self.sr)
        sr = self.sr

        def _cb(ud, fp, fxp, dur, fade, idx):
            minS = c_uint(max(0, int(dur * sr / 1000.0)))
            fadeS = c_uint(max(0, int(fade * sr / 1000.0)))
            if fp:
                self.sp.speechPlayer_queueFrameEx(
                    player, fp, fxp, c_uint(ctypes.sizeof(FrameEx)),
                    minS, fadeS, c_int(-1), c_int(0))
            else:
                self.sp.speechPlayer_queueFrameEx(
                    player, None, None, c_uint(0),
                    minS, fadeS, c_int(-1), c_int(0))

        cb = CB(_cb)
        rc = self.fe.nvspFrontend_queueIPA_Ex(
            self.h, ipa.encode("utf-8"),
            c_double(speed), c_double(pitch), c_double(infl),
            b".", c_int(0), cb, None)
        if not rc:
            err = self.fe.nvspFrontend_getLastError(self.h)
            raise RuntimeError("queueIPA_Ex(%r) failed: %s"
                               % (ipa, err.decode() if err else "?"))

        pcm = bytearray()
        buf = (c_short * 4096)()
        while True:
            n = self.sp.speechPlayer_synthesize(player, c_uint(4096), buf)
            if n <= 0:
                break
            pcm += ctypes.string_at(buf, n * ctypes.sizeof(c_short))
        self.sp.speechPlayer_terminate(player)

        with wave.open(out_wav, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(sr)
            w.writeframes(bytes(pcm))
        return (len(pcm) // 2) / sr


def apply3(d, keys=("s", "s_es", "s_mx")):
    """Build an edits dict applying the same param changes to all 3 /s/ phonemes."""
    return {k: dict(d) for k in keys}


def make_variant_pack(edits, base_pack=DEFAULT_PACK):
    """Copy base_pack to a temp dir, rewrite phonemes.yaml per `edits`
    ({phoneme: {param: value}}), return the temp packDir."""
    import tempfile
    import shutil
    import re as _re
    tmp = tempfile.mkdtemp(prefix="tgsbvar_")
    packdir = os.path.join(tmp, "packs")
    shutil.copytree(base_pack, packdir)
    src = os.path.join(base_pack, "phonemes.yaml")
    dst = os.path.join(packdir, "phonemes.yaml")
    cur = None
    out = []
    with open(src, encoding="utf-8") as f:
        for line in f:
            m = _re.match(r"^  (\S+):\s*$", line)
            if m:
                cur = m.group(1)
            else:
                m2 = _re.match(r"^    (\w+):", line)
                if m2 and cur in edits and m2.group(1) in edits[cur]:
                    line = f"    {m2.group(1)}: {edits[cur][m2.group(1)]}\n"
            out.append(line)
    with open(dst, "w", encoding="utf-8") as f:
        f.writelines(out)
    return packdir


WORDS = [
    ("espanol", "espanol"), ("restablecer", "restablecer"),
    ("seleccionado", "seleccionado"), ("suspendido", "suspendido"),
    ("casa", "casa_ctrl"), ("espuela", "espuela"),
]

if __name__ == "__main__":
    pack = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PACK
    outdir = sys.argv[2] if len(sys.argv) > 2 else "windows_probe"
    os.makedirs(outdir, exist_ok=True)
    r = Renderer(pack_dir=pack, lang="es-mx")
    for text, base in WORDS:
        ipa = get_ipa(text, voice="es-419")
        dur = r.render(ipa, os.path.join(outdir, base + ".wav"))
        print(f"{base:14} ipa={ipa!r} dur={dur:.2f}s")
