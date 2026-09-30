#!/usr/bin/env python3
"""Package installed QEMU and project graphics libraries, then smoke-test relocation."""
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    host_os = 'darwin' if platform.system() == 'Darwin' else 'linux'
    name = f'qemu-gki-{host_os}-arm64'
    prefix = ROOT / f'prebuilts/host/{host_os}-arm64'
    dist = ROOT / 'dist'
    dist.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='package-', dir=ROOT / 'out') as temp:
        package = Path(temp) / name
        shutil.copytree(prefix / 'qemu', package, symlinks=True)
        libdir = package / 'lib'
        libdir.mkdir(exist_ok=True)
        for dependency in ['libepoxy', 'virglrenderer']:
            for lib in (prefix / dependency / 'lib').iterdir():
                if '.so' in lib.name or lib.name.endswith('.dylib'):
                    shutil.copy2(lib, libdir / lib.name, follow_symlinks=False)
        binary = package / 'bin/qemu-system-aarch64'
        if host_os == 'darwin':
            for file in [binary, *libdir.iterdir()]:
                if file.is_symlink() or not file.is_file():
                    continue
                deps = subprocess.check_output(['otool', '-L', str(file)], text=True)
                for line in deps.splitlines()[1:]:
                    old = line.strip().split(' (')[0]
                    if str(prefix) in old:
                        relative = os.path.relpath(libdir / Path(old).name, file.parent)
                        subprocess.run(['install_name_tool', '-change', old,
                                        '@loader_path/' + relative, str(file)], check=True)
                if file.suffix == '.dylib':
                    subprocess.run(['install_name_tool', '-id', '@rpath/' + file.name, str(file)], check=True)
                subprocess.run(['codesign', '--force', '--sign', '-',
                                '--preserve-metadata=entitlements', str(file)], check=True)
        launcher = package / 'qemu'
        variable = 'DYLD_LIBRARY_PATH' if host_os == 'darwin' else 'LD_LIBRARY_PATH'
        launcher.write_text('#!/usr/bin/env bash\nset -euo pipefail\n'
                            'HERE="$(cd "$(dirname "$0")" && pwd)"\n'
                            f'export {variable}="$HERE/lib${{{variable}:+:${variable}}}"\n'
                            'exec "$HERE/bin/qemu-system-aarch64" "$@"\n')
        launcher.chmod(0o755)
        (package / 'licenses').mkdir()
        for source, label in [('qemu/COPYING', 'QEMU'), ('thirdparty/virglrenderer/COPYING', 'VirGL'),
                              ('thirdparty/libepoxy/COPYING', 'libepoxy')]:
            shutil.copy2(ROOT / source, package / 'licenses' / (label + '.txt'))
        (package / 'README.txt').write_text(
            'Run ./qemu [QEMU options]. Target guest: aarch64.\n'
            'Includes QEMU and project libepoxy/VirGL; system dependencies remain external.\n'
            'macOS ARM64: Homebrew libraries from .ci/install-deps.sh.\n'
            'Linux ARM64: Ubuntu 24.04-compatible glibc, SDL2, GL/EGL, libgbm, libdrm, glib, pixman, libslirp.\n'
            'A compatible host GPU/display and HVF or /dev/kvm access are required for accelerated boot.\n'
            'CI checks compilation and command-line capabilities, not real GPU rendering.\n')
        # Run from a different directory to catch accidental current-directory dependencies.
        subprocess.run([str(launcher), '--version'], cwd=temp, check=True)
        devices = subprocess.check_output([str(launcher), '-device', 'help'], cwd=temp, text=True)
        if 'virtio-gpu-gl-pci' not in devices:
            raise SystemExit('Packaged QEMU has no virtio-gpu-gl-pci')
        fileinfo = subprocess.check_output(['file', str(binary)], text=True)
        if not any(arch in fileinfo for arch in ['arm64', 'aarch64']):
            raise SystemExit(f'Wrong architecture: {fileinfo}')
        archive = dist / (name + '.tar.gz')
        with tarfile.open(archive, 'w:gz') as tar:
            tar.add(package, arcname=name)
        print(archive)


if __name__ == '__main__':
    main()
