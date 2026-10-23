---
name: gki-hal
description: Design, implement, install and pack soft Android vendor HALs for this QEMU GSI bring-up.
---

Locate the GKI workspace; read root `README.md`, then `.ai/AGENTS.md`. Shared
Google HALs live in `src/common/hardware/interfaces/<hal>/aidl/default/`
(`Android.bp`). A variant selects them with
`$(call inherit-product, src/common/hardware/interfaces/common.mk)` and
`PRODUCT_PACKAGES +=` in `src/aosp/<variant>/device.mk`. Only
OEM-specific HALs stay under the variant. `scripts/build-vendor-img.sh`
is the QEMU pack path. Guest compile is `source build/envsetup.sh && lunch && m`.

## Layout of an existing HAL

Typical shared HAL (see `src/common/hardware/interfaces/{health,light,power}`):

| Piece | Path |
| --- | --- |
| Common sources + `Android.bp` | `src/common/hardware/interfaces/<name>/aidl/default/` |
| Variant product list | `src/aosp/<variant>/device.mk` (`PRODUCT_PACKAGES +=`) |
| OEM-only special | `src/aosp/<variant>/hardware/<name>/Android.bp` (`gki_custom` or `cc_binary`) |
| init rc | `*.rc` next to sources — `class early_hal`/`hal`, `seclabel`, `interface aidl …` |
| VINTF fragment | `*.xml` next to sources — name, version, `fqname` |
| Optional props | `props/*.prop` |
| NDK shims | `src/common/hardware/shim` (`GKI_HAL_SHIM`) |

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
   - Write `src/common/hardware/interfaces/<name>/aidl/default/Android.bp`
     (`cc_binary` with `srcs`, `shared_libs` including
     `android.hardware.<hal>-V<n>-ndk`, `init_rc`, `vintf_fragments`). Add the
     module to `src/common/hardware/interfaces/common.mk` or the variant
     `device.mk` `PRODUCT_PACKAGES +=`. Local specials use `gki_custom`.
   - Install into `vendor` (bin under `bin/hw`, libs under `lib64`/`lib64/hw`).
   - `source build/envsetup.sh && lunch <variant> && m` (or `m <module>`).
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
- Link against GSI-extracted `libbinder_ndk` / NDK stubs from lunch
  (`GSI_LIBS`, `NDK`, `AIDL`). Do not introduce `libc++_shared` into mapper SPHAL
  paths unless the search path is fixed like the graphics stub.
- Stop QEMU before replacing `vendor.img`. Keep userdata under `out/test-<variant>/`.

Do not init/push a Git repo unless asked. Report what was registered and what is
still missing.
