"""Render the full 6-word set with the pf6_6500 /s/ fix into windows_probe_v1/."""
import os
from render_tgsb import Renderer, get_ipa, WORDS, apply3, make_variant_pack

EDITS = apply3({"pf6": 6500, "pb6": 2000, "pa6": 0.9})
pack = make_variant_pack(EDITS)
r = Renderer(pack_dir=pack, lang="es-mx")
os.makedirs("windows_probe_v1", exist_ok=True)
for text, base in WORDS:
    r.render(get_ipa(text, "es-419"), os.path.join("windows_probe_v1", base + ".wav"))
    print(base, "done")
