"""A/B render for the voiced-fricative pitch-sync noise modulation.

Renders /z/-bearing words (English phonemic /z/ + Spanish voiced-/s/ contexts)
plus /s/ controls, at a given sample rate, into a tagged output dir.

Usage: python voiced_fric_ab.py <tag> [sr]
  tag = baseline | proto   (just names the output subdir)
  sr  = 22050 (default) | 11025
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render_tgsb import Renderer, get_ipa

HERE = os.path.dirname(os.path.abspath(__file__))

# (text, espeak voice, tgsb lang, basename)
WORDS = [
    # English phonemic /z/
    ("zoo",     "en-us", "en-us", "en_zoo"),
    ("buzz",    "en-us", "en-us", "en_buzz"),
    ("rose",    "en-us", "en-us", "en_rose"),
    ("zebra",   "en-us", "en-us", "en_zebra"),
    ("music",   "en-us", "en-us", "en_music"),
    ("visit",   "en-us", "en-us", "en_visit"),  # /v/ and /z/
    # English /s/ controls (must stay identical)
    ("sun",     "en-us", "en-us", "en_sun"),
    ("bus",     "en-us", "en-us", "en_bus"),
    # Spanish voiced-/s/ (s -> [z] before voiced consonant)
    ("mismo",   "es-419", "es-mx", "es_mismo"),
    ("desde",   "es-419", "es-mx", "es_desde"),
    ("isla",    "es-419", "es-mx", "es_isla"),
    ("rasgo",   "es-419", "es-mx", "es_rasgo"),
    # Spanish /s/ control
    ("casa",    "es-419", "es-mx", "es_casa"),
]


def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else "out"
    sr = int(sys.argv[2]) if len(sys.argv) > 2 else 22050
    outdir = os.path.join(HERE, f"vfric_{tag}_{sr}")
    os.makedirs(outdir, exist_ok=True)

    # Group by (voice, lang) to reuse the renderer/handle where possible,
    # but use a FRESH renderer per word to avoid segment-gap / noise-stream
    # cross-contamination in the A/B.
    for text, voice, lang, base in WORDS:
        ipa = get_ipa(text, voice=voice)
        r = Renderer(pack_dir=r"C:\git\TGSpeechBox\packs", lang=lang, sr=sr)
        dur = r.render(ipa, os.path.join(outdir, base + ".wav"))
        print(f"{base:12} [{lang}] ipa={ipa!r:24} {dur:.2f}s")


if __name__ == "__main__":
    main()
