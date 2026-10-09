#!/usr/bin/env python3
"""Import an offline image export into the independent native Windows runtime."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil


FILES = ("Image", "system.img", "vendor.img", "initramfs.img", "userdata.qcow2")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1] / "out/native-windows/image-export")
    parser.add_argument("--runtime", type=Path, default=Path(os.environ.get("LOCALAPPDATA", ".")) / "TeboxNative")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("Run using native Windows Python.")
    source, runtime = args.source.resolve(), args.runtime.resolve()
    manifest = json.loads((source / "image-manifest.json").read_text())
    images = runtime / "images"
    if images.exists() and any(images.iterdir()):
        raise SystemExit(f"Runtime images already exist; refusing to overwrite userdata: {images}")
    for name in FILES:
        expected = manifest["files"][name]
        path = source / name
        if path.stat().st_size != expected["size"] or sha256(path) != expected["sha256"]:
            raise SystemExit(f"Source failed verification: {path}")
    images.mkdir(parents=True, exist_ok=True)
    for name in FILES:
        target = images / name
        partial = target.with_suffix(target.suffix + ".copy")
        shutil.copyfile(source / name, partial)
        if sha256(partial) != manifest["files"][name]["sha256"]:
            raise SystemExit(f"Destination failed verification: {partial}")
        partial.replace(target)
        print(f"Imported and verified {name}", flush=True)
    (images / "image-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Native Windows images ready: {images}")
    print("The WSL source images were not modified. Native startup does not access WSL.")


if __name__ == "__main__":
    main()
