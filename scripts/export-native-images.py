#!/usr/bin/env python3
"""One-time, offline export of the verified WSL images; native launch does not use WSL."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path.home() / ".local/share/tebox-windows/tebox")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "out/native-windows/image-export")
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    for cmdline in Path("/proc").glob("[0-9]*/cmdline"):
        try:
            command = cmdline.read_bytes().split(b"\0")
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
        if command and Path(command[0].decode(errors="replace")).name == "qemu-system-aarch64" \
                and any(str(source).encode() in item for item in command[1:]):
            raise SystemExit("Stop this WSL VM before exporting its userdata.")
    if output.exists() and any(output.iterdir()):
        raise SystemExit(f"Refusing to replace an existing export: {output}")
    output.mkdir(parents=True, exist_ok=True)
    variant = "aosp_arm64-BP4A.251205.006"
    images = source / "src/aosp" / variant / "images"
    jobs = {
        "Image": source / "src/kernel/gki-6.1-android14-gsi/gki/Image",
        "system.img": images / "system.img",
        "vendor.img": images / "vendor.img",
        "initramfs.img": images / "initramfs.img",
        "userdata.qcow2": source / "out" / ("test-" + variant) / "userdata.img",
    }
    records = {}
    for name, path in jobs.items():
        if not path.is_file() or path.stat().st_size < 4096:
            raise SystemExit(f"Missing prepared runtime input: {path}")
        target = output / name
        if name == "userdata.qcow2":
            subprocess.run(["qemu-img", "convert", "-f", "raw", "-O", "qcow2", str(path), str(target)], check=True)
            subprocess.run(["qemu-img", "check", "-f", "qcow2", str(target)], check=True)
            subprocess.run(["qemu-img", "compare", "-f", "raw", "-F", "qcow2", str(path), str(target)], check=True)
        else:
            shutil.copyfile(path, target)
        records[name] = {"size": target.stat().st_size, "sha256": digest(target), "source": str(path)}
        if name != "userdata.qcow2" and records[name]["sha256"] != digest(path):
            raise RuntimeError(f"Export hash mismatch: {name}")
        print(f"Verified export: {name}, {records[name]['size']:,} bytes", flush=True)
    manifest = {
        "variant": variant, "source_runtime": str(source),
        "copy_semantics": "Independent offline copy; original WSL userdata is untouched",
        "files": records,
    }
    (output / "image-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Export complete: {output}", flush=True)


if __name__ == "__main__":
    main()
