"""Stress-initial /s/ alignment bug (b6): desktop reproduction probe.

Two experiments:

A) queueIPA_ExWithText vs queueIPA_Ex through the real DLL, same IPA,
   live pack, es-mx @ 22050.  Byte-identical WAVs => the text-parser path
   changed nothing for that word; differing WAVs => parser fork reproduced.

B) espeak_TextToPhonemes(mode 0x02) exactly as tgsb_jni.cpp calls it
   (SetVoiceByProperties, bundled resources data) vs the CLI `--ipa` output
   the b5 isolation used.  A mismatch here means the device never saw the
   IPA we validated on desktop.
"""
import ctypes
import hashlib
import os
import sys
from ctypes import c_char_p, c_int, c_void_p, POINTER, byref

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render_tgsb import Renderer, get_ipa, DEFAULT_PACK, CB, c_double

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "stress_wt")
RESOURCES = r"C:\git\TGSpeechBox\resources"

WORDS = [("Español", "espanol"), ("escuchar", "escuchar"),
         ("suspendido", "suspendido"), ("casa", "casa"),
         ("restablecer", "restablecer")]


class WtRenderer(Renderer):
    def __init__(self, **kw):
        super().__init__(**kw)
        self.fe.nvspFrontend_queueIPA_ExWithText.argtypes = [
            c_void_p, c_char_p, c_char_p, c_double, c_double, c_double,
            c_char_p, c_int, CB, c_void_p]
        self.fe.nvspFrontend_queueIPA_ExWithText.restype = c_int
        self.fe.nvspFrontend_prepareText.argtypes = [c_void_p, c_char_p]
        self.fe.nvspFrontend_prepareText.restype = c_void_p  # manual free
        self.fe.nvspFrontend_freeString.argtypes = [c_void_p]

    def prepare_text(self, text):
        p = self.fe.nvspFrontend_prepareText(self.h, text.encode("utf-8"))
        if not p:
            return text  # unchanged
        s = ctypes.string_at(p).decode("utf-8")
        self.fe.nvspFrontend_freeString(p)
        return s

    def render_with_text(self, text, ipa, out_wav, speed=1.0, pitch=140.0,
                         infl=0.5):
        import wave
        from ctypes import c_short, c_uint
        player = self.sp.speechPlayer_initialize(self.sr)
        sr = self.sr

        def _cb(ud, fp, fxp, dur, fade, idx):
            minS = c_uint(max(0, int(dur * sr / 1000.0)))
            fadeS = c_uint(max(0, int(fade * sr / 1000.0)))
            if fp:
                self.sp.speechPlayer_queueFrameEx(
                    player, fp, fxp, c_uint(ctypes.sizeof(
                        type(fxp.contents))), minS, fadeS, c_int(-1), c_int(0))
            else:
                self.sp.speechPlayer_queueFrameEx(
                    player, None, None, c_uint(0), minS, fadeS,
                    c_int(-1), c_int(0))

        cb = CB(_cb)
        rc = self.fe.nvspFrontend_queueIPA_ExWithText(
            self.h, text.encode("utf-8"), ipa.encode("utf-8"),
            c_double(speed), c_double(pitch), c_double(infl),
            b".", c_int(0), cb, None)
        if not rc:
            err = self.fe.nvspFrontend_getLastError(self.h)
            raise RuntimeError("queueIPA_ExWithText(%r) failed: %s"
                               % (text, err))
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


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def experiment_b():
    """espeak API (as JNI calls it) vs CLI --ipa."""
    print("=== B: espeak API (JNI-style) vs CLI --ipa ===")
    dll = None
    for cand in (r"C:\Program Files\eSpeak NG\libespeak-ng.dll",
                 r"C:\Program Files (x86)\eSpeak NG\libespeak-ng.dll"):
        if os.path.exists(cand):
            dll = cand
            break
    if not dll:
        print("  !! libespeak-ng.dll not found, skipping")
        return
    es = ctypes.CDLL(dll)
    es.espeak_Initialize.argtypes = [c_int, c_int, c_char_p, c_int]
    es.espeak_Initialize.restype = c_int

    class VOICE(ctypes.Structure):
        _fields_ = [("name", c_char_p), ("languages", c_char_p),
                    ("identifier", c_char_p), ("gender", ctypes.c_ubyte),
                    ("age", ctypes.c_ubyte), ("variant", ctypes.c_ubyte),
                    ("xx1", ctypes.c_ubyte), ("score", c_int),
                    ("spare", c_void_p)]

    es.espeak_SetVoiceByProperties.argtypes = [POINTER(VOICE)]
    es.espeak_SetVoiceByProperties.restype = c_int
    es.espeak_TextToPhonemes.argtypes = [POINTER(c_void_p), c_int, c_int]
    es.espeak_TextToPhonemes.restype = c_char_p

    data = RESOURCES if os.path.exists(
        os.path.join(RESOURCES, "espeak-ng-data")) else None
    sr = es.espeak_Initialize(2, 0, data.encode() if data else None, 0)
    print(f"  espeak_Initialize -> {sr} (data={data})")
    v = VOICE()
    v.languages = b"es-419"
    rc = es.espeak_SetVoiceByProperties(byref(v))
    print(f"  SetVoiceByProperties(es-419) -> {rc}")

    for text, base in WORDS:
        raw = text.encode("utf-8")
        buf = ctypes.create_string_buffer(raw)
        ptr = ctypes.cast(buf, c_void_p)
        chunks = []
        while ptr.value:
            p = es.espeak_TextToPhonemes(byref(ptr), 1, 0x02)
            if not p:
                break
            s = p.decode("utf-8", "replace")
            if s:
                chunks.append(s)
            # espeak advances ptr; stops at NUL
            if not ptr.value:
                break
            # safety: check remaining text is non-empty
            rest = ctypes.string_at(ptr.value)
            if not rest:
                break
        api_ipa = " ".join(chunks)
        cli_ipa = get_ipa(text, voice="es-419")
        mark = "SAME" if api_ipa == cli_ipa else "DIFF"
        print(f"  {base:12} api={api_ipa!r:30} cli={cli_ipa!r:30} {mark}")


def experiment_a():
    """WithText vs direct render, byte comparison.

    Fresh handle per render — a shared handle poisons the comparison via
    streamHasSpeech (first call on a handle skips the segment-boundary
    gap, later calls prepend it; es.yaml gapMs=20).
    """
    print("=== A: queueIPA_ExWithText vs queueIPA_Ex (es-mx, 22050) ===")
    os.makedirs(OUT, exist_ok=True)
    for text, base in WORDS:
        ipa = get_ipa(text, voice="es-419")
        d = os.path.join(OUT, f"{base}_direct.wav")
        w = os.path.join(OUT, f"{base}_withtext.wav")
        rd = WtRenderer(pack_dir=DEFAULT_PACK, lang="es-mx", sr=22050)
        rd.render(ipa, d)
        rw = WtRenderer(pack_dir=DEFAULT_PACK, lang="es-mx", sr=22050)
        prepped = rw.prepare_text(text)
        rw.render_with_text(prepped, ipa, w)
        mark = "IDENTICAL" if md5(d) == md5(w) else "DIFF  <<<<"
        print(f"  {base:12} ipa={ipa!r:22} prepped={prepped!r:14} {mark}")


def experiment_c():
    """Android flow replication: setLanguage(en-us) BEFORE es-mx.

    nativeCreate on Android loads en-us first, then the service switches
    to es-mx. If en-us dict state survives the switch, the parser gate
    opens for Spanish and runTextParser runs with English dicts.
    """
    print("=== C: en-us -> es-mx language switch (Android sequence) ===")
    os.makedirs(OUT, exist_ok=True)
    for text, base in WORDS:
        ipa = get_ipa(text, voice="es-419")
        d = os.path.join(OUT, f"{base}_seq_direct.wav")
        w = os.path.join(OUT, f"{base}_seq_withtext.wav")

        rd = WtRenderer(pack_dir=DEFAULT_PACK, lang="en-us", sr=22050)
        if not rd.fe.nvspFrontend_setLanguage(rd.h, b"es-mx"):
            raise RuntimeError("setLanguage es-mx failed")
        rd.render(ipa, d)

        rw = WtRenderer(pack_dir=DEFAULT_PACK, lang="en-us", sr=22050)
        if not rw.fe.nvspFrontend_setLanguage(rw.h, b"es-mx"):
            raise RuntimeError("setLanguage es-mx failed")
        rw.render_with_text(text, ipa, w)

        mark = "IDENTICAL" if md5(d) == md5(w) else "DIFF  <<<<"
        print(f"  {base:12} ipa={ipa!r:22} {mark}")


if __name__ == "__main__":
    import sys
    if len(sys.argv) > 1 and sys.argv[1] == "c":
        experiment_c()
    else:
        experiment_b()
        experiment_a()
