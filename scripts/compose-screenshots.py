#!/usr/bin/env python3
# Composes the landing-page hero and the App Store screenshot set (1440x900)
# from the window captures in website/img: the main window as the backdrop,
# the floating editors laid over it with a shadow. Run after retaking any
# capture. Window sizes are whatever the app opens them at (1x points):
#   shot-playlist / shot-pianoroll 1440x900, shot-pianoroll-small 860x520,
#   shot-808 686x349, shot-4osc 965x511, shot-mixer 648x424.
import os
from PIL import Image, ImageFilter

src = "website/img"; out = "dist/appstore"; os.makedirs(out, exist_ok=True)
W, H = 1440, 900

def shadowed(img, blur=28, alpha=150, offset=(0, 14)):
    pad = blur * 3
    layer = Image.new("RGBA", (img.width + pad * 2, img.height + pad * 2), (0, 0, 0, 0))
    mask = Image.new("L", img.size, alpha)
    layer.paste((0, 0, 0, alpha), (pad + offset[0], pad + offset[1]), mask)
    layer = layer.filter(ImageFilter.GaussianBlur(blur))
    layer.paste(img, (pad, pad), img if img.mode == "RGBA" else None)
    return layer, pad

def over(base, name, pos):
    win = Image.open(f"{src}/{name}").convert("RGBA")
    layer, pad = shadowed(win)
    base.alpha_composite(layer, (pos[0] - pad, pos[1] - pad))

main = Image.open(f"{src}/shot-playlist.png").convert("RGBA")

# Landing-page hero: the piano roll over the playlist's empty lower half,
# clear of the clips (which end around y=360) and the help bar.
hero = main.copy(); over(hero, "shot-pianoroll-small.png", (W - 860 - 90, 345))
hero.convert("RGB").save(f"{src}/hero.png")

main.save(f"{out}/01-playlist.png")
Image.open(f"{src}/shot-pianoroll.png").convert("RGB").save(f"{out}/02-pianoroll.png")

drums = main.copy(); over(drums, "shot-808.png", (W - 686 - 80, 150)); drums.convert("RGB").save(f"{out}/03-808-drums.png")
synth = main.copy(); over(synth, "shot-4osc.png", (W - 965 - 80, 150)); synth.convert("RGB").save(f"{out}/04-4osc.png")
mixer = main.copy(); over(mixer, "shot-mixer.png", (W - 648 - 120, 330)); mixer.convert("RGB").save(f"{out}/05-mixer.png")

for f in sorted(os.listdir(out)):
    im = Image.open(f"{out}/{f}"); assert im.size == (W, H), (f, im.size); print(f, im.size)
