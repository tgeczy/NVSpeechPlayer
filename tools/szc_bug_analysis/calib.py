from render_tgsb import Renderer, get_ipa

r = Renderer(lang="es-mx")
ipa = get_ipa("espanol", voice="es-419")
print("ipa:", ipa)
for s in [0.5, 1, 2, 5, 10, 20, 50]:
    d = r.render(ipa, f"cal_{s}.wav", speed=s)
    print(f"speed={s:<5} dur={d:.3f}s")
