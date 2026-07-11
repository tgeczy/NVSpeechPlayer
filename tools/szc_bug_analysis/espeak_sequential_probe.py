"""Sequential espeak_TextToPhonemes state probe (issue #100 stress-initial bug).

Device flow: ONE espeak instance phonemizes probe words sequentially.
Desktop isolation always used fresh-process one-shots. eSpeak translator
state is known to leak across calls (historic synth-switch spelling bug).

Phonemize the b5 probe word list in order through one initialize, print
each IPA; run twice with different orderings to expose order dependence.
"""
import ctypes
import os
from ctypes import c_char_p, c_int, c_void_p, POINTER, byref

DLL = r"C:\git\espeak-ng\libespeak-ng.dll"
DATA = r"C:\git\TGSpeechBox\resources"


class VOICE(ctypes.Structure):
    _fields_ = [("name", c_char_p), ("languages", c_char_p),
                ("identifier", c_char_p), ("gender", ctypes.c_ubyte),
                ("age", ctypes.c_ubyte), ("variant", ctypes.c_ubyte),
                ("xx1", ctypes.c_ubyte), ("score", c_int),
                ("spare", c_void_p)]


def phonemize_sequence(words, label):
    # fresh process per RUN is impossible in one script; use os.spawn?
    # Simpler: this function is called once per python process (see main).
    es = ctypes.CDLL(DLL)
    es.espeak_Initialize.argtypes = [c_int, c_int, c_char_p, c_int]
    es.espeak_Initialize.restype = c_int
    es.espeak_SetVoiceByProperties.argtypes = [POINTER(VOICE)]
    es.espeak_SetVoiceByProperties.restype = c_int
    es.espeak_TextToPhonemes.argtypes = [POINTER(c_void_p), c_int, c_int]
    es.espeak_TextToPhonemes.restype = c_char_p

    es.espeak_Initialize(2, 0, DATA.encode(), 0)
    v = VOICE()
    v.languages = b"es-mx"  # exactly what the Android LangDef passes
    es.espeak_SetVoiceByProperties(byref(v))

    print(f"--- {label} ---")
    for w in words:
        buf = ctypes.create_string_buffer(w.encode("utf-8"))
        ptr = ctypes.cast(buf, c_void_p)
        chunks = []
        while ptr.value:
            p = es.espeak_TextToPhonemes(byref(ptr), 1, 0x02)
            if p:
                s = p.decode("utf-8", "replace")
                if s:
                    chunks.append(s)
            if not ptr.value or not ctypes.string_at(ptr.value):
                break
        print(f"  {w:14} -> {' '.join(chunks)!r}")


PROBE_ORDER = ["Restablecer", "Seleccionado", "Español", "casa", "rosa",
               "mismo", "suspendido", "escuchar", "espuela"]

if __name__ == "__main__":
    import sys
    if len(sys.argv) > 1 and sys.argv[1] == "espanol-first":
        phonemize_sequence(["Español", "escuchar", "casa"], "espanol FIRST")
    else:
        phonemize_sequence(PROBE_ORDER, "probe order (device sequence)")
