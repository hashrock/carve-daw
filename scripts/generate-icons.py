#!/usr/bin/env python3
"""Regenerate branding from carve.svg. Requires rsvg-convert (librsvg)."""

from pathlib import Path
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parent.parent


def main():
    renderer = shutil.which("rsvg-convert")
    if renderer is None:
        raise SystemExit("Install librsvg (macOS: brew install librsvg) first.")

    source = (ROOT / "carve.svg").read_text()
    svg = ET.fromstring(source)
    ET.register_namespace("", "http://www.w3.org/2000/svg")
    for element in svg.iter():
        if element.get("fill") == "black":
            element.set("fill", "url(#mark)")
    mark = "\n    ".join(ET.tostring(child, encoding="unicode") for child in svg)
    template = (ROOT / "scripts/icon-template.svg").read_text()
    (ROOT / "assets/icon.svg").write_text(template.replace("__CARVE_MARK__", mark))
    (ROOT / "website/img/logo.svg").write_text(source)

    header = ROOT / "src/app/CarveLogo.h"
    updated, count = re.subn(r'R"svg\(.*?\)svg"', lambda _: 'R"svg(\n' + source.rstrip() + '\n)svg"',
                             header.read_text(), flags=re.DOTALL)
    if count != 1:
        raise SystemExit("Expected exactly one embedded SVG in CarveLogo.h")
    header.write_text(updated)

    for filename, size in [("assets/icon-1024.png", 1024),
                           ("website/img/icon-256.png", 256),
                           ("website/img/favicon.png", 64)]:
        subprocess.run([renderer, "-w", str(size), "-h", str(size),
                        "-o", str(ROOT / filename), str(ROOT / "assets/icon.svg")], check=True)
        print(f"Generated {filename} ({size} × {size})")


if __name__ == "__main__":
    main()
