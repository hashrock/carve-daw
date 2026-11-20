#!/usr/bin/env python3
"""Package Carve DAW's downloadable sound library and (optionally) upload it.

Reads library-server/packs.json. For each pack it writes, under --out:

  v1/packs/<id>/<version>/<id>-<version>.zip   the pack itself
  v1/packs/<id>/<version>/README.txt           its credits and terms, UTF-8

and then v1/library.json, the list the app reads: every pack's version,
download size, installed size, URL and SHA-256.

The zip holds README-Carve.txt at its root and the author's distribution,
unchanged, in one folder beside it. Timestamps and order are fixed, so the
same sources always make the same bytes and the same hash.

  scripts/package-library.py                      # build into build-library/
  scripts/package-library.py --upload             # build, then put into R2
  scripts/package-library.py --upload --only adachi-rei
"""

import argparse
import datetime
import hashlib
import json
import os
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PACKS = ROOT / "library-server" / "packs.json"
DEFAULT_BASE_URL = "https://carve-library.hashrock.workers.dev"
BUCKET = "carve-library"
SKIP = {".DS_Store", "__MACOSX", "Thumbs.db", "desktop.ini"}
FIXED_TIME = (2026, 1, 1, 0, 0, 0)


def decode(data: bytes) -> str:
    if data.startswith(b"\xff\xfe") or data.startswith(b"\xfe\xff"):
        return data.decode("utf-16")
    if data.startswith(b"\xef\xbb\xbf"):
        return data[3:].decode("utf-8")
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return data.decode("cp932", errors="replace")


def collect(source: Path, include):
    roots = [source / name for name in include] if include else [source]
    files = []
    for root in roots:
        if not root.exists():
            sys.exit(f"missing: {root}")
        if root.is_file():
            files.append(root)
            continue
        for path in root.rglob("*"):
            if path.is_file() and not any(part in SKIP for part in path.relative_to(source).parts):
                files.append(path)
    return sorted(files, key=lambda p: p.relative_to(source).as_posix())


def readme_for(pack, source: Path) -> str:
    lines = [
        f"{pack['nameEn']} / {pack['name']}  version {pack['version']}",
        "",
        f"Author:   {pack['author']}",
        f"Official: {pack['homepage']}",
        "",
        pack["description"],
        "",
        "This copy is redistributed by Carve DAW's sound library",
        "(https://github.com/hashrock/carve-daw) as the author's terms allow.",
        "The files in the folder beside this one are the author's distribution,",
        "unmodified. Only system files (.DS_Store, Thumbs.db, desktop.ini) were",
        "left out.",
        "",
        "Credit:",
        "  " + pack["credit"],
        "",
        "In short (the author's own terms below are what applies):",
        "  " + pack["licenseSummary"],
        "",
    ]
    for name in pack.get("terms", []):
        text = decode((source / name).read_bytes()).replace("\r\n", "\n").replace("\r", "\n")
        lines += ["=" * 40, name, "=" * 40, "", text.rstrip(), ""]
    return "\n".join(lines) + "\n"


def write_zip(path: Path, pack, source: Path, files, readme: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    folder = pack["folder"]
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        def add(name, data):
            info = zipfile.ZipInfo(name, FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, data)

        add("README-Carve.txt", readme.encode("utf-8"))
        for f in files:
            add(f"{folder}/{f.relative_to(source).as_posix()}", f.read_bytes())


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def put(key: str, file: Path, content_type: str, cache: str):
    print(f"  upload {key}")
    subprocess.run(["wrangler", "r2", "object", "put", f"{BUCKET}/{key}", "--remote",
                    "--file", str(file), "--content-type", content_type, "--cache-control", cache],
                   check=True, cwd=ROOT / "library-server")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=str(ROOT / "build-library"))
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL)
    parser.add_argument("--upload", action="store_true")
    parser.add_argument("--only", action="append", help="pack ids to upload (all are still listed)")
    args = parser.parse_args()

    out = Path(args.out)
    spec = json.loads(PACKS.read_text(encoding="utf-8"))
    listed = []
    uploads = []

    for pack in spec["packs"]:
        source = Path(os.path.expanduser(pack["source"]))
        files = collect(source, pack.get("include"))
        readme = readme_for(pack, source)
        base = f"v1/packs/{pack['id']}/{pack['version']}"
        zip_key = f"{base}/{pack['id']}-{pack['version']}.zip"
        readme_key = f"{base}/README.txt"
        zip_path, readme_path = out / zip_key, out / readme_key

        print(f"{pack['id']} {pack['version']}: {len(files)} files")
        write_zip(zip_path, pack, source, files, readme)
        readme_path.write_text(readme, encoding="utf-8")

        entry = pack["folder"] + ("/" + pack["entry"] if pack.get("entry") else "")
        listed.append({
            "id": pack["id"],
            "kind": pack["kind"],
            "name": pack["name"],
            "nameEn": pack["nameEn"],
            "version": pack["version"],
            "author": pack["author"],
            "homepage": pack["homepage"],
            "description": pack["description"],
            "licenseSummary": pack["licenseSummary"],
            "credit": pack["credit"],
            "entry": entry,
            "url": f"{args.base_url}/{zip_key}",
            "size": zip_path.stat().st_size,
            "sha256": sha256(zip_path),
            "installedSize": sum(f.stat().st_size for f in files) + len(readme.encode("utf-8")),
            "readmeUrl": f"{args.base_url}/{readme_key}",
        })

        if args.upload and (not args.only or pack["id"] in args.only):
            uploads.append((zip_key, zip_path, "application/zip"))
            uploads.append((readme_key, readme_path, "text/plain; charset=utf-8"))

    manifest = {
        "schema": 1,
        "updated": datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat(),
        "packs": listed,
    }
    manifest_path = out / "v1" / "library.json"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {manifest_path}")

    if args.upload:
        immutable = "public, max-age=31536000, immutable"
        for key, path, content_type in uploads:
            put(key, path, content_type, immutable)
        # Last, so the list never names a file that is not there yet.
        put("v1/library.json", manifest_path, "application/json; charset=utf-8", "public, max-age=300")


if __name__ == "__main__":
    main()
