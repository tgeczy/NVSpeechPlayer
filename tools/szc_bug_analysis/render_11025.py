"""11025 Hz /s/ survival prototypes (issue #100, post-b5 reports).

Nyquist at 11025 = 5512 Hz. b5 pf6=6500 is above it (dead); b4 pf6=5250
was only 262 Hz under it (half-dead). DECtalk-style answer: park the
peak at ~4600-4900 with wide bandwidth and steep low-end.

Variants (s_es + s_mx):
  A_b4      - b4 values (pa6 0.75 @ 5250, pb6 1815): what 11025 users had
  B_b5      - current shipped (pa6 0.9 @ 6500, pb6 2000): reported dead
  C_clamp   - simulated DSP clamp: 6500 -> 4650, pb6 2200, pa6 0.9
  D_insured - C + pa5 0.35 @ 3750 body band

Renders casa/restablecer/suspendido es-mx at sr=11025.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render_tgsb import Renderer, get_ipa, make_variant_pack, DEFAULT_PACK

def both(d):
    return {"s_es": dict(d), "s_mx": dict(d)}

VARIANTS = {
    "A_b4":      both({"pf6": 5250, "pb6": 1815, "pa6": 0.75, "pa5": 0, "pa4": 0}),
    "B_b5":      None,  # live pack
    "C_clamp":   both({"pf6": 4650, "pb6": 2200, "pa6": 0.9}),
    "D_insured": both({"pf6": 4650, "pb6": 2200, "pa6": 0.9, "pa5": 0.35}),
}

WORDS = [("casa", "casa"), ("restablecer", "restablecer"),
         ("suspendido", "suspendido")]

if __name__ == "__main__":
    outdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sr11025")
    os.makedirs(outdir, exist_ok=True)
    for vname, edits in VARIANTS.items():
        pack = DEFAULT_PACK if edits is None else make_variant_pack(edits)
        r = Renderer(pack_dir=pack, lang="es-mx", sr=11025)
        for text, base in WORDS:
            ipa = get_ipa(text, voice="es-419")
            dur = r.render(ipa, os.path.join(outdir, f"{base}_{vname}.wav"))
            print(f"{vname:10} {base:12} {dur:.2f}s")
