#!/usr/bin/env python3
"""Fetch checksum-pinned Android inputs. Run in a clean CI checkout."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent


def download(spec, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        temp = path.with_suffix(path.suffix + '.part')
        subprocess.run(['curl', '-fL', '--retry', '3', '--connect-timeout', '30',
                        spec['url'], '-o', str(temp)], check=True)
        temp.rename(path)
    with path.open('rb') as stream:
        actual = hashlib.file_digest(stream, 'sha256').hexdigest()
    if actual != spec['sha256']:
        raise SystemExit(f'Checksum mismatch; remove only this bad cached download and retry: {path}')


def main():
    lock = json.loads((ROOT / '.ci/guest.lock.json').read_text())
    downloads = ROOT / 'downloads'
    gsi = downloads / 'aosp-arm64-BP4A.251205.006.zip'
    download(lock['gsi'], gsi)
    system = ROOT / f"src/aosp/{lock['variant']}/images/system.img"
    if not system.exists():
        system.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(gsi) as archive:
            names = [n for n in archive.namelist() if Path(n).name == 'system.img']
            if len(names) != 1:
                raise SystemExit('Expected exactly one system.img in official GSI archive')
            with archive.open(names[0]) as source, system.open('wb') as target:
                shutil.copyfileobj(source, target)
        with system.open('rb') as stream:
            sparse = stream.read(4) == bytes.fromhex('3aff26ed')
        if sparse:
            raw = system.with_suffix('.raw')
            subprocess.run(['simg2img', str(system), str(raw)], check=True)
            raw.replace(system)
    busybox = downloads / 'busybox-1.37.0.tar.bz2'
    download(lock['busybox'], busybox)
    source = ROOT / 'thirdparty/busybox'
    if not source.exists():
        stage = ROOT / 'out/busybox-source'
        stage.mkdir(parents=True, exist_ok=True)
        with tarfile.open(busybox) as archive:
            archive.extractall(stage, filter='data')
        (stage / 'busybox-1.37.0').rename(source)
    env = dict(os.environ, GKI_REV=lock['kernel_commit'], MODULES_REV=lock['modules_commit'])
    subprocess.run(['bash', str(ROOT / 'scripts/fetch-gki-virt.sh'), lock['kernel_id']], env=env, check=True)
    for name, expected in lock['kernel_files'].items():
        file = ROOT / 'src/kernel' / lock['kernel_id'] / name
        if hashlib.sha256(file.read_bytes()).hexdigest() != expected:
            raise SystemExit(f'Kernel/module differs from the tested version: {file}')


if __name__ == '__main__':
    main()
