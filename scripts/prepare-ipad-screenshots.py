#!/usr/bin/env python3
# Readies screenshots taken on the iPad (top button + volume up) for App
# Store Connect's 13-inch iPad slot: checks each is a full-screen capture at
# a size the slot takes, flattens it to RGB (no alpha -- App Store Connect
# rejects transparency), and writes them numbered, in name order, to
# dist/appstore/ipad. The iPad Pro 12.9" captures at 2048x2732 (2732x2048
# landscape), which the 13-inch slot accepts as is.
#
#   scripts/prepare-ipad-screenshots.py ~/Desktop/ipad-shots
import os, sys
from PIL import Image

SIZES = {(2048, 2732), (2732, 2048), (2064, 2752), (2752, 2064)}

if len(sys.argv) != 2:
    sys.exit("usage: prepare-ipad-screenshots.py <folder of iPad screenshots>")

src = os.path.expanduser(sys.argv[1])
out = "dist/appstore/ipad"
names = sorted(f for f in os.listdir(src) if f.lower().endswith((".png", ".jpg", ".jpeg")))
if not names:
    sys.exit(f"no PNG/JPEG screenshots in {src}")
if len(names) > 10:
    sys.exit(f"{len(names)} screenshots; App Store Connect takes at most 10")

bad = []
for f in names:
    with Image.open(os.path.join(src, f)) as im:
        if im.size not in SIZES:
            bad.append(f"{f}: {im.size[0]}x{im.size[1]}")
if bad:
    sys.exit("not a 13-inch iPad screen size (resized or cropped?):\n  " + "\n  ".join(bad))

os.makedirs(out, exist_ok=True)
for old in os.listdir(out):
    os.remove(os.path.join(out, old))

orient = set()
for i, f in enumerate(names, 1):
    with Image.open(os.path.join(src, f)) as im:
        orient.add("landscape" if im.width > im.height else "portrait")
        dest = f"{out}/{i:02d}-{os.path.splitext(f)[0]}.png"
        im.convert("RGB").save(dest)
        print(dest, f"{im.width}x{im.height}")

if len(orient) > 1:
    print("note: mixed portrait and landscape; the store shows them at different sizes")
