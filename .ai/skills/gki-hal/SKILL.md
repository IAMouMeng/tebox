---
name: gki-hal
description: Design, implement, install and pack soft Android vendor HALs for this QEMU GSI bring-up.
---

Locate the GKI workspace and read `AGENTS.md`. HAL code lives under
`src/aosp/<variant>/hardware/<name>/` and is installed into
`qemu/vendor` then packed by `scripts/build-vendor-img.sh`.

## Layout of an existing HAL

Typical soft HAL (see `health`, `graphics`, `audio`, `power`, `keymint/soft`):

| Piece | Path |
| --- | --- |
| Service sources | `hardware/<name>/stub/` or `hardware/<name>/soft/` (`service*.cpp`, `build-stub.sh`) |
| init rc | `hardware/<name>/init/*.rc` — `class early_hal`/`hal`, `seclabel`, `interface aidl …` |
| VINTF fragment | `hardware/<name>/vintf/*.xml` — name, version, `fqname` |
| Optional props | `hardware/<name>/props/*.prop` |
| Install helper | `hardware/<name>/stub/install.sh` (copies bin/rc/xml into `vendor`) |

Combined device manifest: `qemu/vendor/manifest.xml`.  
SELinux file contexts: `vendor/etc/selinux/vendor_file_contexts` (bring-up
often reuses `hal_keymint_system_exec` under permissive).

## New HAL checklist

1. **Confirm the required interface** from boot logs (`servicemanager` / audioserver
   / system_server) and FCM (`out/gsi-vintf` or GSI matrices). Pick a frozen AIDL
   version allowed by the matrix (often the lowest that still has the needed
   methods).

2. **Generate NDK AIDL** from frozen `aidl_api` trees under
   `thirdparty/hardware-interfaces` or `out/hw-ifaces-tmp`, with matching
   `--version` / `--hash`, same pattern as `hardware/health/stub/build-stub.sh`.

3. **Implement a minimal Bn*** service that registers with
   `AServiceManager_addService` / `AServiceManager_registerLazyService` as
   appropriate. Return safe defaults or `EX_UNSUPPORTED_OPERATION` for paths not
   needed to pass boot; expand only when a client requires it.

4. **Wire init + VINTF + manifest + file_contexts**. Instance names must match
   what the framework looks up (usually `/default`).

5. **Build & install**
   - Cross-compile with NDK via the HAL’s `build-stub.sh` / `build.sh`.
   - Install into `vendor` (bin under `bin/hw`, libs under `lib64`/`lib64/hw`).
   - Or run `scripts/build-hals.sh` for the standard set.
   - Repack: `scripts/build-vendor-img.sh` then `scripts/build-initramfs.sh`.
   - Ensure `scripts/build-vendor-img.sh` labels the new binary’s SELinux xattr
     when other HALs do.

6. **Boot verify** looking for `registered …` / `Found … in device VINTF
   manifest` and that the previous crash loop stopped. Use
   `scripts/qemu-console.py` when a debug console is enabled.

## Conventions

- Prefer soft/reference implementations that unblock boot over full hardware
  fidelity.
- KeyMint must provide real crypto ops for progressing past early boot; use
  `hardware/keymint/soft`, not an empty stub.
- Link against GSI-extracted `libbinder_ndk` / NDK stubs from `scripts/env.sh`
  (`GSI_LIBS`, `NDK`, `AIDL`). Do not introduce `libc++_shared` into mapper SPHAL
  paths unless the search path is fixed like the graphics stub.
- Stop QEMU before replacing `vendor.img`. Keep userdata under `out/test-<variant>/`.

Do not init/push a Git repo unless asked. Report what was registered and what is
still missing.
