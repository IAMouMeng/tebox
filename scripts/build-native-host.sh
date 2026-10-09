#!/usr/bin/env bash
# Authorized LOCAL experiment, not an upstream QEMU contribution.
# Native Windows x64/UCRT64 build of project QEMU + patched VirGL; never runs WSL.
# Run in an MSYS2 UCRT64 shell (or use build-native-host.ps1).
# Requires preinstalled UCRT64 gcc, meson, ninja, pkgconf, python,
# python-setuptools, python-wheel, python-yaml, glib2, pixman, SDL2, libslirp,
# libepoxy (EGL enabled), dtc and zlib; MSYS git/make and standard shell tools.
# No package installation, driver changes, Git init/commit, or guest image work.
# Stop programs using install/bin before rebuilding. Original sources and WSL
# artifacts are read-only. Symlinks are materialized in the exported copy.
#
# Environment overrides:
#   SOURCE_ROOT          Checkout (default: this script's parent)
#   SOURCE_REF           Committed revision to export (default: HEAD)
#   WIN32_PRESENT_HELPER Optional explicit legacy helper input (SHA256 recorded)
#   NATIVE_HOST_ROOT     ASCII, no-space NTFS build root
#                        (default: %LOCALAPPDATA%/TeboxNative/host)
#   JOBS                 Parallel jobs (default: min(nproc,20))
#   HOST_CFLAGS          Default: -march=x86-64 -mtune=generic
#   HOST_CXXFLAGS        Default: HOST_CFLAGS
#   HOST_LDFLAGS         Default: empty
#   HOST_X86_VERSION     QEMU x86 baseline 1/2/3/4 (default: 1)
#   KEYCODEMAPDB_ARCHIVE  Optional pre-exported pinned tar.gz; otherwise use
#                        downloads/keycodemapdb-<revision>.tar.gz, then HTTPS
#   KEYCODEMAPDB_SHA256   Optional required checksum of that archive
#   STAGE_ONLY=1         Export sources only
#   BUILD_ONLY=1         Stop after compilation, before private installation
#
# A different source revision requires a different root. Existing owned staged
# edits are preserved. Minimum staged-only compatibility guards live below.
# Release -O3, x86-64-v3 needs AVX2; use HOST_X86_VERSION=1 and
# HOST_CFLAGS='-march=x86-64' for older CPUs. No KVM/WHPX can accelerate ARM64
# guests on an x64 host. Runtime EXEs and DLLs are PE Windows binaries, not MSYS.
set -euo pipefail
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
[[ "${MSYSTEM:-}" == UCRT64 && "${MINGW_PREFIX:-}" == /ucrt64 ]] || fail 'Run using MSYS2 UCRT64 (build-native-host.ps1), not Git Bash/WSL.'
SOURCE_ROOT="$(cygpath -au "${SOURCE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}")"
SOURCE_REF="${SOURCE_REF:-HEAD}"
NATIVE_HOST_ROOT="$(cygpath -au "${NATIVE_HOST_ROOT:-${LOCALAPPDATA:?}/TeboxNative/host}")"
[[ "$NATIVE_HOST_ROOT" != *[[:space:]]* ]] || fail 'NATIVE_HOST_ROOT must not contain whitespace.'
[[ "$NATIVE_HOST_ROOT" != "$SOURCE_ROOT" && "$NATIVE_HOST_ROOT" != "$SOURCE_ROOT/"* ]] || fail 'Build outside the checkout.'
for tool in python3 git gcc g++ ninja pkg-config make objdump; do
  command -v "$tool" >/dev/null || fail "Missing preinstalled build tool: $tool"
done
[[ "$(command -v gcc)" == /ucrt64/bin/* && "$(command -v python3)" == /ucrt64/bin/* ]] || fail 'UCRT64 compiler and Python must precede other tools on PATH.'
JOBS="${JOBS:-$(nproc)}"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail 'JOBS must be a positive integer.'
[[ -n "${HOST_JOBS_UNCAPPED:-}" ]] || { (( JOBS <= 20 )) || JOBS=20; }
HOST_CFLAGS="${HOST_CFLAGS:--march=x86-64 -mtune=generic}"
HOST_CXXFLAGS="${HOST_CXXFLAGS:-$HOST_CFLAGS}"
HOST_LDFLAGS="${HOST_LDFLAGS:-}"
HOST_X86_VERSION="${HOST_X86_VERSION:-1}"
[[ "$HOST_X86_VERSION" =~ ^[1-4]$ ]] || fail 'HOST_X86_VERSION must be 1,2,3,4.'
TOOLS="$NATIVE_HOST_ROOT/tools"
SOURCES="$NATIVE_HOST_ROOT/source"
QEMU_BUILD="$NATIVE_HOST_ROOT/build/qemu"
VIRGL_BUILD="$NATIVE_HOST_ROOT/build/virglrenderer"
VIRGL_PREFIX="$NATIVE_HOST_ROOT/virglrenderer"
PREFIX="$NATIVE_HOST_ROOT/install"
export SOURCE_REF HOST_CFLAGS HOST_CXXFLAGS HOST_LDFLAGS HOST_X86_VERSION JOBS
export SOURCE_ROOT_WIN="$(cygpath -am "$SOURCE_ROOT")"
export NATIVE_HOST_ROOT_WIN="$(cygpath -am "$NATIVE_HOST_ROOT")"
export UCRT_BIN_WIN="$(cygpath -am /ucrt64/bin)"
export GIT_LFS_SKIP_SMUDGE=1 GIT_TERMINAL_PROMPT=0 PYTHONUTF8=1
export PYTHONDONTWRITEBYTECODE=1 PIP_DISABLE_PIP_VERSION_CHECK=1
export PIP_CACHE_DIR="$(cygpath -am "$TOOLS/pip-cache")"
# Use MSYS's ordinary copy fallback for configure's ln -s; no symlink privilege.
unset MSYS
python3 - <<'PY'
import ctypes, os, pathlib, sys
root = pathlib.Path(os.environ['NATIVE_HOST_ROOT_WIN'])
if os.name != 'nt' or sys.version_info < (3, 12):
    raise SystemExit('Need native UCRT Python >= 3.12.')
if not str(root).isascii():
    raise SystemExit('NATIVE_HOST_ROOT must be ASCII.')
fs = ctypes.create_unicode_buffer(32)
if not ctypes.windll.kernel32.GetVolumeInformationW(root.anchor, None, 0, None, None, None, fs, len(fs)) or fs.value != 'NTFS':
    raise SystemExit('NATIVE_HOST_ROOT must be on local NTFS.')
marker = root / '.tebox-native-host-owned'
if root.is_symlink() or (root.exists() and any(root.iterdir()) and not marker.is_file()):
    raise SystemExit(f'Refusing nonempty unowned root: {root}')
root.mkdir(parents=True, exist_ok=True)
marker.write_text('scripts/build-native-host.sh\n')
for name in ('tools', 'downloads', 'build/qemu', 'build/virglrenderer', 'install', 'virglrenderer'):
    (root / name).mkdir(parents=True, exist_ok=True)
PY
exec > >(tee -a "$TOOLS/build.log") 2>&1
printf '\n=== Native host build %s ===\nSource: %s (%s)\nRoot: %s\nJobs: %s\n' "$(date -Is)" "$SOURCE_ROOT" "$SOURCE_REF" "$NATIVE_HOST_ROOT" "$JOBS"

# git archive respects git's symlink metadata even with core.symlinks=false.
# Extract regular members first, then COPY link targets (files/directories).
# No Windows Developer Mode/admin privilege, no mutation of the source checkout.
export SOURCE_REVISION="$(git -C "$SOURCE_ROOT" rev-parse "${SOURCE_REF}^{commit}")"
python3 - <<'PY'
import json, os, pathlib, shutil, subprocess, tarfile, tempfile
root = pathlib.Path(os.environ['NATIVE_HOST_ROOT_WIN'])
base = ['git', '-C', os.environ['SOURCE_ROOT_WIN'], '-c', 'filter.lfs.process=',
        '-c', 'filter.lfs.clean=', '-c', 'filter.lfs.smudge=', '-c', 'filter.lfs.required=false']
revision = os.environ['SOURCE_REVISION']
dest = root / 'source'
marker = dest / '.tebox-native-source.json'
if dest.exists():
    if not marker.is_file() or json.loads(marker.read_text())['revision'] != revision:
        raise SystemExit('Refusing to replace existing source; use a new NATIVE_HOST_ROOT.')
else:
    archive = root / 'downloads' / ('host-source-' + revision + '.tar')
    if not archive.is_file():
        with archive.with_suffix('.part').open('wb') as out:
            subprocess.run(base + ['archive', '--format=tar', revision, '--', 'qemu', 'thirdparty/virglrenderer'], stdout=out, check=True)
        archive.with_suffix('.part').replace(archive)
    with tempfile.TemporaryDirectory(prefix='source-', dir=root / 'tools') as tmp:
        stage = pathlib.Path(tmp)
        with tarfile.open(archive) as tar:
            members = tar.getmembers()
            links = [m for m in members if m.issym() or m.islnk()]
            tar.extractall(stage, members=[m for m in members if not m.issym() and not m.islnk()], filter='data')
        pending = list(links)
        while pending:
            progress = False
            for m in pending[:]:
                dst = stage / m.name
                target = ((dst.parent if m.issym() else stage) / m.linkname).resolve()
                if not target.is_relative_to(stage.resolve()):
                    raise SystemExit(f'Archive link escapes source: {m.name}')
                if not target.exists():
                    continue
                dst.parent.mkdir(parents=True, exist_ok=True)
                if target.is_dir():
                    shutil.copytree(target, dst)
                else:
                    shutil.copy2(target, dst)
                pending.remove(m)
                progress = True
            if not progress:
                raise SystemExit('Unresolved archive links: ' + ', '.join(m.name for m in pending))
        (stage / marker.name).write_text(json.dumps({'revision': revision, 'materialized_links': len(links)}, indent=2) + '\n')
        stage.rename(dest)
print('Source revision:', revision, flush=True)
print('QEMU:', (dest / 'qemu/VERSION').read_text().strip(), flush=True)
PY
[[ "${STAGE_ONLY:-0}" != 1 ]] || exit 0

for dep in glib-2.0 pixman-1 sdl2 slirp epoxy zlib; do
  pkg-config --exists "$dep" || fail "Missing UCRT64 dependency: $dep"
done
[[ -f /ucrt64/include/libfdt.h ]] || fail 'Missing UCRT64 dtc/libfdt headers.'
# System-site-packages exposes ONLY native UCRT Python deps; build tools remain
# isolated. Vendored wheels need no network. Never invoke system pip install.
python3 -m venv --copies --system-site-packages "$TOOLS/venv"
PYTHON="$TOOLS/venv/bin/python.exe"
"$PYTHON" -m pip install --no-index --find-links "$SOURCES/qemu/python/wheels" \
  meson==1.12.0 pycotap==1.3.1 qemu.qmp==0.0.6
"$PYTHON" - <<'PY'
import importlib.metadata
for name in ('setuptools', 'wheel', 'PyYAML'):
    try:
        print(name, importlib.metadata.version(name), flush=True)
    except importlib.metadata.PackageNotFoundError:
        raise SystemExit('Preinstall UCRT64 python-setuptools, python-wheel and python-yaml; no global install is performed by this helper.')
import yaml
PY
export PATH="$TOOLS/venv/bin:/ucrt64/bin:/usr/bin:$PATH"
if [[ -n "${KEYCODEMAPDB_ARCHIVE:-}" ]]; then
  export KEYCODEMAPDB_ARCHIVE="$(cygpath -am "$KEYCODEMAPDB_ARCHIVE")"
fi
# A one-time externally exported WSL archive is accepted, but not requested or
# created here. All later source setup, compilation and launch are Windows-only.
"$PYTHON" - <<'PY'
import configparser, hashlib, os, pathlib, re, shutil, tarfile, tempfile, urllib.request
root = pathlib.Path(os.environ['NATIVE_HOST_ROOT_WIN'])
qemu = root / 'source/qemu'
wrap = configparser.ConfigParser()
wrap.read(qemu / 'subprojects/keycodemapdb.wrap')
revision = wrap['wrap-git']['revision']
if not re.fullmatch(r'[0-9a-f]{40}', revision):
    raise SystemExit('keycodemapdb must be pinned.')
dest = qemu / 'subprojects/keycodemapdb'
if not dest.exists():
    archive = pathlib.Path(os.environ.get('KEYCODEMAPDB_ARCHIVE', str(root / 'downloads' / f'keycodemapdb-{revision}.tar.gz')))
    if not archive.is_file():
        url = wrap['wrap-git']['url'].removesuffix('.git') + f'/-/archive/{revision}/keycodemapdb-{revision}.tar.gz'
        with urllib.request.urlopen(url, timeout=30) as src, archive.with_suffix('.part').open('wb') as out:
            shutil.copyfileobj(src, out)
        archive.with_suffix('.part').replace(archive)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if os.environ.get('KEYCODEMAPDB_SHA256', digest).lower() != digest:
        raise SystemExit('keycodemapdb archive checksum mismatch.')
    with tempfile.TemporaryDirectory(prefix='keycode-', dir=root / 'tools') as tmp:
        with tarfile.open(archive) as tar:
            tar.extractall(tmp, filter='data')
        candidates = [p for p in pathlib.Path(tmp).iterdir() if p.is_dir() and (p / 'meson.build').is_file()]
        if len(candidates) != 1:
            raise SystemExit('Expected one keycodemapdb source root in archive.')
        src = candidates[0]
        old_marker = src / '.tebox-wsl-source-revision'
        if old_marker.exists() and old_marker.read_text().strip() != revision:
            raise SystemExit('Wrong pinned keycodemapdb revision in exported archive.')
        (src / '.tebox-native-keycode.json').write_text(__import__('json').dumps({'revision': revision, 'sha256': digest}, indent=2) + '\n')
        src.rename(dest)
if not (dest / 'README').is_file() or not (dest / 'meson.build').is_file():
    raise SystemExit('Incomplete keycodemapdb source.')
print('keycodemapdb:', revision, flush=True)
PY

# Minimum compatibility changes, ONLY in owned staged trees, exact/idempotent.
# 1. Project glyph workaround references an Apple-only helper in common code.
#    Keep the workaround on Apple, normal integer attributes on Windows/Linux.
# 2. On Windows without symlink privilege QEMU's optional qemu-bundle postconf
#    fails WinError1314. Skip only that optional convenience bundle on Windows;
#    install/bin and share/qemu are populated explicitly after the build.
# 3. VirGL 1.3 includes virtgpu_drm.h for capset IDs even without DRM renderers.
#    Keep its portable types, but exclude BSD-only sys/ioccom.h on Windows;
#    no DRM ioctl macro is used by this configuration.
"$PYTHON" - <<'PY'
import os, pathlib
root = pathlib.Path(os.environ['NATIVE_HOST_ROOT_WIN'])
def replace(relative, old, new):
    path = root / 'source' / relative
    text = path.read_text(encoding='utf-8')
    if new in text:
        return
    if old in text:
        if text.count(old) != 1:
            raise SystemExit(f'Ambiguous staged guard: {relative}')
        path.write_text(text.replace(old, new), encoding='utf-8', newline='\n')
        print('Applied staged guard:', relative, flush=True)
    else:
        raise SystemExit(f'Staged guard needs source review: {relative}')
renderer = root / 'source/thirdparty/virglrenderer/src/vrend/vrend_renderer.c'
# Newer revisions already contain the platform split. Older pinned native
# sources still need the equivalent staged guard below.
integer_split = '''#else
         if (util_format_is_pure_integer(ve->base.src_format)) {
#endif'''
if integer_split not in renderer.read_text(encoding='utf-8'):
    replace('thirdparty/virglrenderer/src/vrend/vrend_renderer.c',
'''         if (util_format_is_pure_integer(ve->base.src_format) &&
             !apple_vs_input_is_float(ctx, i)) {''',
'''         if (util_format_is_pure_integer(ve->base.src_format)
#ifdef __APPLE__
             && !apple_vs_input_is_float(ctx, i)
#endif
         ) {''')
replace('thirdparty/virglrenderer/src/drm/drm-uapi/drm.h',
'#include <stdint.h>\n#include <sys/ioccom.h>\n#include <sys/types.h>',
'#include <stdint.h>\n#ifndef _WIN32\n#include <sys/ioccom.h>\n#endif\n#include <sys/types.h>')
# Keep the deployed server's protocol bound in native builds as well. Host GL
# limits may exceed the guest protocol's fixed constant-buffer arrays.
replace('thirdparty/virglrenderer/src/vrend/vrend_renderer.c',
        '      caps->v1.max_uniform_blocks = max + 1 - 1;',
        '      caps->v1.max_uniform_blocks = MIN2(max + 1 - 1, PIPE_MAX_CONSTANT_BUFFERS);')
# Native presentation helper is maintained in the checkout. Apply only its
# integration hooks to the exported revision; retain other staged changes.
helper = root / 'source/qemu/ui/sdl2-win32-present.h'
if not helper.is_file() and os.environ.get('WIN32_PRESENT_HELPER'):
    helper = pathlib.Path(os.environ['WIN32_PRESENT_HELPER']).resolve()
if not helper.is_file():
    raise SystemExit('SOURCE_REF lacks the Windows helper; set WIN32_PRESENT_HELPER explicitly for legacy sources.')
(root / 'source/qemu/ui/sdl2-win32-present.h').write_text(
    helper.read_text(encoding='utf-8'), encoding='utf-8', newline='\n')
replace('qemu/ui/sdl2-gl.c', '#include "ui/sdl2.h"\n',
        '#include "ui/sdl2.h"\n#ifdef CONFIG_WIN32\n#include "sdl2-win32-present.h"\n#endif\n')
replace('qemu/ui/sdl2-gl.c',
        '    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);\n',
        '    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);\n'
        '#ifdef CONFIG_WIN32\n'
        '    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,\n'
        '                        epoxy_is_desktop_gl() ? SDL_GL_CONTEXT_PROFILE_CORE\n'
        '                                              : SDL_GL_CONTEXT_PROFILE_ES);\n#else\n')
replace('qemu/ui/sdl2-gl.c',
        '    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, params->major_ver);\n',
        '#endif\n    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, params->major_ver);\n')
replace('qemu/ui/sdl2-gl.c',
        '    SDL_GetWindowSize(scon->real_window, &ww, &wh);\n'
        '    egl_fb_setup_default(&scon->win_fb, ww, wh, 0, 0);\n',
        '    SDL_GetWindowSize(scon->real_window, &ww, &wh);\n'
        '#ifdef CONFIG_WIN32\n    if (sdl2_win32_present(scon)) {\n        return;\n    }\n#endif\n'
        '    egl_fb_setup_default(&scon->win_fb, ww, wh, 0, 0);\n')
replace('qemu/include/ui/sdl2.h', '    bool scanout_mode;\n',
        '    bool scanout_mode;\n#ifdef CONFIG_WIN32\n    void *win32_present;\n#endif\n')
replace('qemu/include/ui/sdl2.h', 'void sdl2_gl_console_init(struct sdl2_console *scon);\n',
        'void sdl2_gl_console_init(struct sdl2_console *scon);\n'
        '#if defined(CONFIG_WIN32) && defined(CONFIG_OPENGL)\n'
        'void sdl2_gl_win32_present_destroy(struct sdl2_console *scon);\n#endif\n')
replace('qemu/ui/sdl2.c', '    if (scon->winctx) {\n',
        '#if defined(CONFIG_WIN32) && defined(CONFIG_OPENGL)\n'
        '    sdl2_gl_win32_present_destroy(scon);\n#endif\n    if (scon->winctx) {\n')
replace('qemu/ui/sdl2-gl.c', '    scon->scanout_mode = scanout;\n    if (!scon->scanout_mode) {\n',
        '    scon->scanout_mode = scanout;\n    if (!scon->scanout_mode) {\n#ifdef CONFIG_WIN32\n'
        '        sdl2_gl_win32_present_destroy(scon);\n#endif\n')
probe = root / 'tools/symlink-probe'
target = root / 'tools/symlink-probe.target'
target.write_text('test\n')
try:
    os.symlink(target, probe)
except OSError as error:
    if error.winerror != 1314:
        raise
    replace('qemu/meson.build',
"meson.add_postconf_script(find_program('scripts/symlink-install-tree.py'))\n",
"if host_machine.system() != 'windows'\n  meson.add_postconf_script(find_program('scripts/symlink-install-tree.py'))\nendif\n")
finally:
    probe.unlink(missing_ok=True)
    target.unlink()
PY

VIRGL_SETUP=(setup)
[[ ! -f "$VIRGL_BUILD/build.ninja" ]] || VIRGL_SETUP+=(--reconfigure --clearcache)
meson "${VIRGL_SETUP[@]}" "$VIRGL_BUILD" "$SOURCES/thirdparty/virglrenderer" \
  --prefix="$(cygpath -am "$VIRGL_PREFIX")" --libdir=lib --buildtype=release \
  -Doptimization=3 -Ddebug=false -Ddefault_library=shared \
  -Dc_args="$HOST_CFLAGS" -Dc_link_args="$HOST_LDFLAGS" \
  -Dplatforms=egl -Dtests=false -Dvenus=false -Dvideo=false '-Ddrm-renderers=[]'
ninja -C "$VIRGL_BUILD" -j "$JOBS"
meson install -C "$VIRGL_BUILD" --no-rebuild
export PKG_CONFIG_PATH="$VIRGL_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PATH="$VIRGL_PREFIX/bin:$PATH"
[[ "$(pkg-config --modversion virglrenderer)" == 1.3.0 ]] || fail 'pkg-config did not select project VirGL 1.3.0.'
(
  cd "$QEMU_BUILD"
  "$SOURCES/qemu/configure" \
    --prefix="$(cygpath -am "$PREFIX")" --libdir="$(cygpath -am "$PREFIX/lib")" \
    --python="$PYTHON" --cc=gcc --cxx=g++ --ninja=ninja \
    --target-list=aarch64-softmmu --enable-tcg --disable-user \
    --disable-kvm --disable-hvf --disable-whpx --disable-xen --disable-rust \
    --without-default-features --enable-sdl --enable-opengl --enable-virglrenderer \
    --enable-slirp --enable-pixman --enable-hmp --disable-plugins --enable-tools \
    --audio-drv-list=sdl --enable-fdt=system \
    --disable-docs --disable-tests --disable-guest-agent --disable-modules \
    --disable-install-blobs --disable-download --disable-werror --disable-containers \
    --extra-cflags="$HOST_CFLAGS" --extra-cxxflags="$HOST_CXXFLAGS" \
    --extra-ldflags="$HOST_LDFLAGS" \
    -Dbuildtype=release -Doptimization=3 -Ddebug=false -Dqom_cast_debug=false \
    -Dx86_version="$HOST_X86_VERSION"
)
# No firmware rebuilds or unneeded tools/tests. Explicit install avoids a full
# meson install demanding tools that the selected fast targets did not build.
ninja -C "$QEMU_BUILD" -j "$JOBS" qemu-system-aarch64.exe qemu-img.exe
printf '\nCompiled native QEMU: %s/qemu-system-aarch64.exe\n' "$QEMU_BUILD"
[[ "${BUILD_ONLY:-0}" != 1 ]] || exit 0

# Recursively package PE imports, with project VirGL first. EGL/GLES are also
# copied because libepoxy loads them dynamically. SDL desktop core stays WGL:
# NEVER copy Mesa's opengl32.dll over Windows' hardware ICD dispatcher.
"$PYTHON" - <<'PY'
import collections, hashlib, json, os, pathlib, re, shutil, subprocess
root = pathlib.Path(os.environ['NATIVE_HOST_ROOT_WIN'])
ucrt = pathlib.Path(os.environ['UCRT_BIN_WIN'])
bin_dir = root / 'install/bin'
bin_dir.mkdir(parents=True, exist_ok=True)
data = root / 'install/share/qemu'
data.mkdir(parents=True, exist_ok=True)
for name in ('qemu-system-aarch64.exe', 'qemu-img.exe'):
    shutil.copy2(root / 'build/qemu' / name, bin_dir / name)
shutil.copy2(root / 'source/qemu/pc-bios/efi-virtio.rom', data / 'efi-virtio.rom')
search = [{p.name.lower(): p for p in d.glob('*.dll')} for d in (root / 'virglrenderer/bin', ucrt)]
system = pathlib.Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32'
system_names = {p.name.lower() for p in system.glob('*.dll')}
queue = collections.deque(bin_dir.glob('*.exe'))
for name in ('libvirglrenderer-1.dll', 'libEGL.dll', 'libGLESv2.dll'):
    src = next((index[name.lower()] for index in search if name.lower() in index), None)
    if src is None:
        raise SystemExit(f'Missing native runtime dependency: {name}')
    dst = bin_dir / src.name
    shutil.copy2(src, dst)
    queue.append(dst)
seen, imports = set(), {}
while queue:
    pe = queue.popleft()
    if pe.name.lower() in seen:
        continue
    seen.add(pe.name.lower())
    output = subprocess.check_output([str(ucrt / 'objdump.exe'), '-p', str(pe)], text=True, errors='replace')
    if 'pei-x86-64' not in output:
        raise SystemExit(f'Not a native PE x64 binary: {pe}')
    names = re.findall(r'DLL Name:\s*(\S+)', output)
    imports[pe.name] = names
    for name in names:
        key = name.lower()
        if key.startswith(('msys-', 'cygwin')):
            raise SystemExit(f'Unexpected MSYS/Cygwin runtime import: {pe.name}: {name}')
        if key in system_names or key.startswith(('api-ms-win-', 'ext-ms-win-')):
            continue
        src = next((index[key] for index in search if key in index), None)
        if src is None:
            raise SystemExit(f'Unresolved PE import: {pe.name}: {name}')
        if key == 'opengl32.dll':
            raise SystemExit('Refusing non-system OpenGL dispatcher.')
        dst = bin_dir / src.name
        if not dst.exists() or hashlib.sha256(dst.read_bytes()).digest() != hashlib.sha256(src.read_bytes()).digest():
            shutil.copy2(src, dst)
        queue.append(dst)
(root / 'install/pe-imports.json').write_text(json.dumps(imports, indent=2) + '\n')
info = {'source': json.loads((root / 'source/.tebox-native-source.json').read_text()),
        'staged_fix_sha256': {name: hashlib.sha256((root / 'source' / name).read_bytes()).hexdigest()
                             for name in ('thirdparty/virglrenderer/src/vrend/vrend_renderer.c',
                                          'qemu/ui/sdl2-gl.c', 'qemu/ui/sdl2-win32-present.h')},
        'qemu_version': (root / 'source/qemu/VERSION').read_text().strip(), 'virglrenderer_version': '1.3.0',
        'cflags': os.environ['HOST_CFLAGS'], 'optimization': 3,
        'x86_version': os.environ['HOST_X86_VERSION'], 'targets': ['aarch64-softmmu'],
        'compiler': subprocess.check_output(['gcc', '--version'], text=True).splitlines()[0],
        'python': subprocess.check_output(['python3', '--version'], text=True).strip(),
        'runtime_files': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(bin_dir.iterdir()) if p.is_file()}}
(root / 'install/build-info.json').write_text(json.dumps(info, indent=2) + '\n')
# Launch directly with PATH containing NO MSYS/UCRT directories. Windows system
# DLLs and the exe-adjacent private closure are sufficient; WSL is never invoked.
env = os.environ.copy()
env['PATH'] = str(bin_dir) + os.pathsep + str(system) + os.pathsep + str(system.parent)
for name in ('PYTHONPATH', 'LD_LIBRARY_PATH', 'SDL_VIDEODRIVER', 'SDL_OPENGL_LIBRARY'):
    env.pop(name, None)
qemu = str(bin_dir / 'qemu-system-aarch64.exe')
def check(*args):
    result = subprocess.run([qemu, *args], env=env, cwd=root, capture_output=True, text=True, timeout=30, check=True)
    return result.stdout + result.stderr
version = check('--version')
accels = check('-accel', 'help')
devices = check('-device', 'help')
touch = check('-device', 'virtio-tablet-pci,help')
display = check('-display', 'help')
assert '11.1.50' in version and 'tcg' in accels
assert 'virtio-gpu-gl-pci' in devices and 'touchscreen' in touch and 'sdl' in display
print(version, accels, touch, display, sep='\n', flush=True)
subprocess.run([str(bin_dir / 'qemu-img.exe'), '--version'], env=env, check=True, timeout=30)
# A paused, imageless headless machine: no Android guest, network or bound ports.
smoke = subprocess.run([qemu, '-machine', 'virt', '-accel', 'tcg', '-cpu', 'max', '-m', '128',
    '-S', '-display', 'none', '-serial', 'none', '-monitor', 'stdio', '-nodefaults', '-nic', 'none',
    '-device', 'virtio-tablet-pci,touchscreen=on'], input='\ninfo status\nquit\n',
    env=env, cwd=root, capture_output=True, text=True, timeout=30, check=True)
if 'paused' not in smoke.stdout:
    raise SystemExit('Paused headless smoke did not report paused status: ' + smoke.stdout + smoke.stderr)
(root / 'install/smoke-test.log').write_text(version + accels + touch + display + smoke.stdout + smoke.stderr, encoding='utf-8')
print('Private PE dependency closure:', len(imports), 'binaries; no MSYS runtime imports.', flush=True)
print('Paused headless touchscreen smoke passed. No guest boot/GPU/glyph claim.', flush=True)
print('QEMU:', qemu, flush=True)
PY
printf '\nNative build complete. Android boot, AMD renderer and glyphs require separate GUI validation.\n'
