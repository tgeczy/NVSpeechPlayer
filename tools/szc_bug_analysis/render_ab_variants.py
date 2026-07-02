"""A/B/C renders for landing the Spanish /s/ fix (issues #95/#100).

Variants (scoped to s_es + s_mx ONLY — base `s` untouched, English safe):
  A baseline   — current pack as-is
  B winner     — pf6 6500 / pb6 2000 / pa6 0.9 (2026-06-26 validated variant)
  C spread     — s_mx only: restore b3's pa5 band minus the pf4=3300 /S/
                 culprit (pa4 0, pa5 0.35, pa6 0.75). b3's spread survived
                 the affected devices; 19de232 removed it (b4 regression).

Dialects: es-mx (eSpeak es-419 IPA) and es-es (eSpeak es IPA).
Words: leak set + /S/-thickness regression guards + control.

Output: ab_variants/<dialect>_<word>_<A|B|C>.wav
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render_tgsb import Renderer, get_ipa, make_variant_pack, DEFAULT_PACK

WINNER = {"pf6": 6500, "pb6": 2000, "pa6": 0.9}
SPREAD = {"pa4": 0, "pa5": 0.35, "pa6": 0.75}

VARIANTS = {
    "A": None,
    "B": {"s_es": dict(WINNER), "s_mx": dict(WINNER)},
    "C": {"s_mx": dict(SPREAD)},
}

# (text, wav base, which dialects)
WORDS = [
    ("espanol",      "espanol",     ("es-mx", "es-es")),
    ("restablecer",  "restablecer", ("es-mx", "es-es")),
    ("suspendido",   "suspendido",  ("es-mx", "es-es")),
    ("casa",         "casa",        ("es-mx", "es-es")),
    ("pizza",        "pizza",       ("es-mx",)),
    ("percepcion",   "percepcion",  ("es-mx",)),
]

ESPEAK_VOICE = {"es-mx": "es-419", "es-es": "es"}

if __name__ == "__main__":
    outdir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "ab_variants")
    os.makedirs(outdir, exist_ok=True)
    for vname, edits in VARIANTS.items():
        pack = DEFAULT_PACK if edits is None else make_variant_pack(edits)
        for dialect in ("es-mx", "es-es"):
            if vname == "C" and dialect == "es-es":
                continue  # C only changes s_mx; es-es C == A
            r = Renderer(pack_dir=pack, lang=dialect)
            for text, base, dialects in WORDS:
                if dialect not in dialects:
                    continue
                ipa = get_ipa(text, voice=ESPEAK_VOICE[dialect])
                out = os.path.join(outdir, f"{dialect}_{base}_{vname}.wav")
                dur = r.render(ipa, out)
                print(f"{vname} {dialect:5} {base:12} {dur:.2f}s {ipa!r}")
