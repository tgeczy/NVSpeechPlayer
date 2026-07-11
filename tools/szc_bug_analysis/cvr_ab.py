"""A/B render for the consonant clarity boost (CVR).

Fricative-heavy words + short sentences, English + Spanish, so the
consonant-vs-vowel level change is easy to hear. Renders at a given SR into
a tagged dir. Baseline = current DLL; rebuild with CVR then run again.

Usage: python cvr_ab.py <tag> [sr]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render_tgsb import Renderer, get_ipa

HERE = os.path.dirname(os.path.abspath(__file__))

ITEMS = [
    ("success",              "en-us", "en-us", "en_success"),
    ("Mississippi",          "en-us", "en-us", "en_mississippi"),
    ("emphasis",             "en-us", "en-us", "en_emphasis"),
    ("she sells sea shells", "en-us", "en-us", "en_sentence"),
    ("restablecer",          "es-419", "es-mx", "es_restablecer"),
    ("seleccionado",         "es-419", "es-mx", "es_seleccionado"),
    ("suspendido",           "es-419", "es-mx", "es_suspendido"),
    ("casa",                 "es-419", "es-mx", "es_casa"),
    ("eso es necesario",     "es-419", "es-mx", "es_sentence"),
]


def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else "out"
    sr = int(sys.argv[2]) if len(sys.argv) > 2 else 22050
    outdir = os.path.join(HERE, f"cvr_{tag}_{sr}")
    os.makedirs(outdir, exist_ok=True)
    for text, voice, lang, base in ITEMS:
        ipa = get_ipa(text, voice=voice)
        r = Renderer(pack_dir=r"C:\git\TGSpeechBox\packs", lang=lang, sr=sr)
        dur = r.render(ipa, os.path.join(outdir, base + ".wav"))
        print(f"{base:16} [{lang}] {dur:.2f}s  {ipa!r}")


if __name__ == "__main__":
    main()
