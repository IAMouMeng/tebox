#!/usr/bin/env python3
"""Soong-lite: read Android.bp + device.mk, compile NDK vendor HALs."""

from __future__ import annotations

import argparse
import glob as globmod
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from bp import parse_file, BpError

ROOT = Path(__file__).resolve().parents[2]
HW = ROOT / "src" / "common" / "hardware"
IFACES = HW / "interfaces"
SOONG = ROOT / "build" / "soong"
AIDL_NDK = re.compile(r"^(?P<name>.+)-V(?P<ver>\d+)-ndk$")


def load_env(variant: str) -> dict[str, str]:
    script = r"""
source "$1"
printf "NDK=%s\nNDK_HOST_TAG=%s\nAIDL=%s\nANDROID_HEADERS=%s\nGSI_LIBS=%s\n" \
  "$NDK" "$NDK_HOST_TAG" "$AIDL" "$ANDROID_HEADERS" "$GSI_LIBS"
"""
    out = subprocess.check_output(
        ["bash", "-c", script, "soong-env", str(ROOT / "build" / "make" / "core" / "config.sh")],
        env={**os.environ, "VARIANT": variant, "TARGET_PRODUCT": variant, "ROOT": str(ROOT)},
        cwd=str(ROOT),
        text=True,
    )
    env = {}
    for line in out.splitlines():
        k, _, v = line.partition("=")
        env[k] = v
    missing = [k for k in ("NDK", "NDK_HOST_TAG", "AIDL") if not env.get(k)]
    if missing:
        sys.exit(f"soong: config.sh missing {missing}")
    env["VARIANT"] = variant
    env["GKI_ROOT"] = str(ROOT)
    env["GKI_HARDWARE"] = str(IFACES)
    env["GKI_HAL_SHIM"] = str(HW / "shim")
    env["GKI_HAL_VARIANT"] = str(ROOT / "src" / "aosp" / variant / "hardware")
    env["TARGET_OUT_VENDOR"] = str(ROOT / "src" / "aosp" / variant / "qemu" / "vendor")
    env["GKI_OUT"] = str(ROOT / "out")
    env["HI"] = str(ROOT / "thirdparty" / "hardware-interfaces")
    env["API"] = os.environ.get("API", "34")
    ndk = Path(env["NDK"])
    env["CXX"] = str(
        ndk
        / "toolchains"
        / "llvm"
        / "prebuilt"
        / env["NDK_HOST_TAG"]
        / "bin"
        / f"aarch64-linux-android{env['API']}-clang++"
    )
    env.setdefault("ANDROID_HEADERS", str(ROOT / "thirdparty" / "android-headers" / "include"))
    env.setdefault("GSI_LIBS", str(ROOT / "src" / "aosp" / variant / "prebuilts" / "gsi-lib64"))
    return env


def parse_product_mk(path: Path, pkgs: list[str], seen: set[Path], env: dict[str, str]) -> None:
    path = path.resolve()
    if path in seen:
        return
    if not path.is_file():
        sys.exit(f"soong: missing product makefile {path}")
    seen.add(path)
    text = path.read_text(encoding="utf-8")
    for m in re.finditer(r"\$\(call inherit-product,\s*([^)]+)\)", text):
        inc = m.group(1).strip().strip("\"'")
        inc = inc.replace("$(GKI_HARDWARE)", env["GKI_HARDWARE"])
        inc = inc.replace("$(GKI_ROOT)", env["GKI_ROOT"])
        p = Path(inc)
        if not p.is_absolute():
            cand = ROOT / p
            p = cand if cand.is_file() else path.parent / p
        parse_product_mk(p, pkgs, seen, env)
    cont = False
    acc: list[str] = []
    for raw in text.splitlines():
        line = re.sub(r"#.*", "", raw).strip()
        if not line:
            continue
        if cont:
            piece = line.rstrip("\\").strip()
            acc.extend(piece.split())
            cont = line.endswith("\\")
            if not cont:
                for tok in acc:
                    if tok not in pkgs:
                        pkgs.append(tok)
                acc = []
            continue
        m = re.match(r"PRODUCT_PACKAGES\s*\+=\s*(.*)$", line)
        if not m:
            continue
        rest = m.group(1).strip()
        if rest.endswith("\\"):
            acc = rest[:-1].split()
            cont = True
        else:
            for tok in rest.split():
                if tok not in pkgs:
                    pkgs.append(tok)


def collect_modules(env: dict[str, str]) -> dict[str, dict]:
    hw = Path(env["GKI_HARDWARE"])
    var = Path(env["GKI_HAL_VARIANT"])
    files = sorted(hw.rglob("Android.bp"))
    var_files = sorted(var.rglob("Android.bp")) if var.is_dir() else []
    # Variant Android.bp of the same relative path overrides common.
    by_rel: dict[str, Path] = {}
    for f in files:
        by_rel[str(f.relative_to(hw))] = f
    for f in var_files:
        by_rel[str(f.relative_to(var))] = f
    modules: dict[str, dict] = {}
    for f in by_rel.values():
        try:
            parsed = parse_file(str(f))
        except BpError as e:
            print(f"soong: skip {f}: {e}", file=sys.stderr)
            continue
        for mod in parsed:
            if mod["_type"] not in ("cc_binary", "gki_custom", "prebuilt_etc"):
                continue
            name = mod.get("name")
            if not name:
                continue
            mod["_dir"] = str(f.parent)
            modules[name] = mod
    return modules


def as_list(v) -> list:
    if v is None:
        return []
    if isinstance(v, list):
        return v
    return [v]


def expand_srcs(mod: dict, env: dict[str, str]) -> list[str]:
    d = Path(mod["_dir"])
    out = []
    for s in as_list(mod.get("srcs")):
        if any(ch in s for ch in "*?["):
            out.extend(sorted(globmod.glob(str(d / s), recursive=True)))
        else:
            out.append(str(d / s))
    shim = Path(env["GKI_HAL_SHIM"])
    for s in as_list(mod.get("shim_srcs")):
        out.append(str(shim / s))
    return [p for p in out if os.path.isfile(p)]


def aidl_and_libs(mod: dict) -> tuple[list[str], list[str]]:
    aidl = []
    libs = []
    for lib in as_list(mod.get("shared_libs")):
        m = AIDL_NDK.match(lib)
        if m:
            aidl.append(f"{m.group('name')}:{m.group('ver')}")
        else:
            libs.append(lib)
    for spec in as_list(mod.get("aidl_interfaces")):
        aidl.append(spec)
    if not libs:
        libs = ["libbinder_ndk", "liblog"]
    return aidl, libs


def newer(dst: str, srcs: list[str]) -> bool:
    if not os.path.isfile(dst):
        return True
    dt = os.path.getmtime(dst)
    for s in srcs:
        if os.path.isfile(s) and os.path.getmtime(s) > dt:
            return True
    return False


def run(cmd, env, cwd=None):
    r = subprocess.run(cmd, env=env, cwd=cwd)
    if r.returncode != 0:
        sys.exit(r.returncode)


def extract_gsi(env: dict[str, str]) -> None:
    stamp = Path(env["GKI_OUT"]) / f"gsi-libs-{env['VARIANT']}.extracted"
    img = ROOT / "src" / "aosp" / env["VARIANT"] / "images" / "system.img"
    vendor = Path(env["TARGET_OUT_VENDOR"])
    if not (vendor / "etc").is_dir():
        sys.exit(f"soong: missing vendor template {vendor}")
    srcs = [str(img)] if img.is_file() else []
    srcs.append(str(SOONG / "extract-gsi-libs.sh"))
    if not newer(str(stamp), srcs):
        return
    run(["bash", str(SOONG / "extract-gsi-libs.sh"), env["VARIANT"]], os.environ)
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.touch()


def install_file(src: str, dst_dir: str, mode: int) -> None:
    os.makedirs(dst_dir, exist_ok=True)
    dst = os.path.join(dst_dir, os.path.basename(src))
    shutil.copy2(src, dst)
    os.chmod(dst, mode)


def append_file_contexts(env: dict[str, str], names: list[str]) -> None:
    fc = Path(env["TARGET_OUT_VENDOR"]) / "etc" / "selinux" / "vendor_file_contexts"
    if not fc.is_file() or not names:
        return
    text = fc.read_text(encoding="utf-8")
    extra = []
    for name in names:
        if name in text:
            continue
        esc = name.replace(".", r"\\.")
        extra.append(f"/vendor/bin/hw/{esc}    u:object_r:hal_keymint_system_exec:s0\n")
    if extra:
        with fc.open("a", encoding="utf-8") as f:
            f.writelines(extra)


def build_cc_binary(mod: dict, env: dict[str, str]) -> None:
    name = mod["name"]
    srcs = expand_srcs(mod, env)
    if not srcs:
        sys.exit(f"soong: {name} has no srcs")
    aidl, libs = aidl_and_libs(mod)
    rel = mod.get("relative_install_path") or "hw"
    vendor = Path(env["TARGET_OUT_VENDOR"])
    installed = vendor / "bin" / rel / name
    rc = [str(Path(mod["_dir"]) / x) for x in as_list(mod.get("init_rc"))]
    vintf = [str(Path(mod["_dir"]) / x) for x in as_list(mod.get("vintf_fragments"))]
    perm = [str(Path(mod["_dir"]) / x) for x in as_list(mod.get("permissions"))]
    incs = [str(Path(mod["_dir"]) / x) for x in as_list(mod.get("local_include_dirs"))]
    deps = srcs + rc + vintf + perm + [mod["_file"], str(SOONG / "compile-executable.sh")]
    if not newer(str(installed), deps):
        return
    out = Path(env["GKI_OUT"]) / name
    cflags = as_list(mod.get("cflags"))
    ldflags = as_list(mod.get("ldflags")) or ["-static-libstdc++", "-Wl,--allow-shlib-undefined"]
    cmd_env = {
        **os.environ,
        **env,
        "MODULE": name,
        "PATH_DIR": mod["_dir"],
        "OUT": str(out),
        "AIDL_SPECS": " ".join(aidl),
        "SRCS": " ".join(srcs),
        "INCLUDES": " ".join(incs),
        "LIBS": " ".join(libs),
        "LDFLAGS": " ".join(ldflags),
        "FORBIDDEN": str(mod.get("gki_aidl_forbidden") or ""),
        "ROOT": env["GKI_ROOT"],
    }
    run(
        ["bash", str(SOONG / "compile-executable.sh"), "--", *cflags],
        cmd_env,
    )
    built = out / "bin" / name
    install_file(str(built), str(installed.parent), 0o755)
    for f in rc:
        if os.path.isfile(f):
            install_file(f, str(vendor / "etc" / "init"), 0o644)
    for f in vintf:
        if os.path.isfile(f):
            install_file(f, str(vendor / "etc" / "vintf" / "manifest"), 0o644)
    for f in perm:
        if os.path.isfile(f):
            install_file(f, str(vendor / "etc" / "permissions"), 0o644)
    append_file_contexts(env, as_list(mod.get("gki_file_contexts")))
    print(f"Installed {name} -> {installed}")


def build_custom(mod: dict, env: dict[str, str]) -> None:
    name = mod["name"]
    d = Path(mod["_dir"])
    stamp = Path(env["GKI_OUT"]) / name / ".stamp"
    srcs = [str(p) for p in d.rglob("*") if p.is_file() and p.name != ".stamp"]
    srcs.append(mod["_file"])
    if not newer(str(stamp), srcs):
        return
    cmd_env = {
        **os.environ,
        **env,
        "VARIANT": env["VARIANT"],
        "GKI_HAL_SHIM": env["GKI_HAL_SHIM"],
        "ROOT": env["GKI_ROOT"],
    }
    for s in as_list(mod.get("build")):
        print(f"== {s} ==")
        run(["bash", str(d / s)], cmd_env)
    for s in as_list(mod.get("install")):
        run(["bash", str(d / s)], cmd_env)
    post = mod.get("post_install")
    if post:
        run(["bash", "-c", post], cmd_env)
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.touch()


def build_prebuilt_etc(mod: dict, env: dict[str, str]) -> None:
    src = Path(mod["_dir"]) / (mod.get("src") or "")
    if not src.is_file():
        sys.exit(f"soong: prebuilt_etc {mod.get('name')} missing src {src}")
    sub = mod.get("sub_dir") or ""
    filename = mod.get("filename") or src.name
    dst_dir = Path(env["TARGET_OUT_VENDOR"]) / "etc" / sub
    dst = dst_dir / filename
    if not newer(str(dst), [str(src), mod["_file"]]):
        return
    dst_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    os.chmod(dst, 0o644)
    print(f"Installed {filename} -> {dst}")


def build_one(mod: dict, env: dict[str, str], modules: dict) -> None:
    for req in as_list(mod.get("required")):
        if req in modules:
            build_one(modules[req], env, modules)
    t = mod["_type"]
    if t == "cc_binary":
        build_cc_binary(mod, env)
    elif t == "gki_custom":
        build_custom(mod, env)
    elif t == "prebuilt_etc":
        build_prebuilt_etc(mod, env)
    else:
        sys.exit(f"soong: unsupported module type {t} ({mod.get('name')})")


def main() -> int:
    ap = argparse.ArgumentParser(description="Soong-lite HAL build")
    ap.add_argument("--variant", required=True)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("modules", nargs="*")
    args = ap.parse_args()
    env = load_env(args.variant)
    device_mk = ROOT / "src" / "aosp" / args.variant / "device.mk"
    if not device_mk.is_file():
        print(f"skip HAL make: no {device_mk}")
        return 0
    pkgs: list[str] = []
    parse_product_mk(device_mk, pkgs, set(), env)
    modules = collect_modules(env)
    if args.list:
        print("PRODUCT_PACKAGES=", " ".join(pkgs))
        print("GKI_HAL_MODULES=", " ".join(sorted(modules)))
        return 0
    if not pkgs:
        sys.exit(f"soong: PRODUCT_PACKAGES is empty in {device_mk}")
    want = args.modules or pkgs
    extract_gsi(env)
    for name in want:
        if name not in modules:
            sys.exit(f"soong: unknown module {name}")
        build_one(modules[name], env, modules)
    if not args.modules:
        print(f"HALs installed into {env['TARGET_OUT_VENDOR']}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BpError as e:
        print(f"soong: {e}", file=sys.stderr)
        raise SystemExit(1)
