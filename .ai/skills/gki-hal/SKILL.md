---
name: gki-hal
description: Design, implement, install and pack soft Android vendor HALs for this QEMU GSI bring-up.
---

Locate the GKI workspace; read root `README.md`, then `.ai/RAG.md` (HAL
section) and `.ai/AGENTS.md`. Guest
compile is `source build/envsetup.sh && lunch && m` (product makefile +
soong-lite). Shared HALs live in
`src/common/hardware/interfaces/<hal>/aidl/default/`. A variant selects them
with `$(call inherit-product, src/common/hardware/interfaces/common.mk)` in
`src/aosp/<variant>/device.mk`. Variant trees live on `GSI/...` branches;
`main` has no `src/aosp/`. Before edits, create `dirty/<topic>` from that base
and commit locally after each significant change (`.ai/AGENTS.md` Local git).
`scripts/build-vendor-img.sh` packs `qemu/vendor`.

## Common vs own

| Situation | Where to write |
| --- | --- |
| Every GSI needs it, or the shared default is buggy | `src/common/hardware/interfaces/<hal>/aidl/default/` + `PRODUCT_PACKAGES` in `common.mk` |
| This GSI needs an interface common does not ship | `src/aosp/<variant>/hardware/<hal>/aidl/default/` + `PRODUCT_PACKAGES +=` in **that** `device.mk` |
| Common ships it but this GSI cannot use that binary (AIDL version, extra OEM methods) | Same **relative** `Android.bp` path under the variant `hardware/` (soong-lite overrides common). Keep the module name. |

Do not copy the whole common tree into a variant. Do not put OEM-only packages
in `common.mk`. Prefer `cc_binary`; QEMU-only script HALs use `gki_custom`
(graphics, keymint, audio, camera, radio, wifi, codec2).

Template: `src/common/hardware/interfaces/light/aidl/default/`.

## Layout

| Piece | Common | Own (this variant) |
| --- | --- | --- |
| Sources + `Android.bp` | `src/common/hardware/interfaces/<name>/aidl/default/` | `src/aosp/<variant>/hardware/<name>/aidl/default/` |
| Product list | `interfaces/common.mk` | `src/aosp/<variant>/device.mk` after inherit |
| init rc / VINTF xml | next to sources | next to sources |
| NDK shims | `src/common/hardware/shim` | reuse the same shims |

Override example: common `interfaces/light/aidl/default/Android.bp` is replaced
by `src/aosp/<variant>/hardware/light/aidl/default/Android.bp`.

Combined device manifest: `qemu/vendor/manifest.xml`.  
SELinux file contexts: `vendor/etc/selinux/vendor_file_contexts` (bring-up
often reuses `hal_keymint_system_exec` under permissive).

## New HAL checklist

1. **Confirm the interface** from boot logs (`servicemanager` / audioserver /
   system_server) and FCM (`out/gsi-vintf` or GSI matrices). Pick a frozen AIDL
   version the matrix allows.

2. **Decide common vs own** using the table above. Checkout the GSI branch
   before editing `device.mk` or variant `hardware/`.

3. **Generate NDK AIDL** from frozen `aidl_api` trees under
   `thirdparty/hardware-interfaces`. `android.hardware.*-V<n>-ndk` in
   `shared_libs` is enough for `cc_binary` (soong-lite emits the stubs).

4. **Implement a minimal Bn*** service (`AServiceManager_addService` or
   `AServiceManager_registerLazyService`). Return safe defaults or
   `EX_UNSUPPORTED_OPERATION` unless a client needs the path.

5. **Wire init + VINTF**. Instance names must match the framework (usually
   `/default`).

6. **Build**
   ```bash
   source build/envsetup.sh
   lunch <variant>
   m                         # or: m <module>
   ```
   Installs into `src/aosp/<variant>/qemu/vendor`. Stop QEMU, then
   `scripts/build-vendor-img.sh` (and initramfs as needed). Ensure the new
   binary gets the same SELinux xattr as other HALs.

7. **Boot verify** for `registered` / `Found … in device VINTF manifest` and
   that the crash loop stopped. Use `scripts/qemu-console.py` when enabled.

## Conventions

- Soft/reference implementations that unblock boot over full hardware fidelity.
- **Delete unused functions and files** in the same change. `srcs` / `build.sh`
  must list every leftover file; every leftover function needs a call site.
  Drop orphan `build-stub.sh`, duplicate `google/` vs `stub/` trees, `#if 0`,
  and copied AOSP files this binary does not compile. Required `Bn*` overrides
  stay. See `.ai/AGENTS.md` Code conventions.
- **Names:** classes `PascalCase`, functions `camelCase`, `#define` constants
  `SCREAMING_SNAKE`. Do not mint `tools_` / `utils_` helpers.
- **Shared helpers:** purpose-named `.h`/`.cpp` next to callers (`FdIo`,
  `C2Blocks`, …); public API in the module namespace. No catch-all
  `tools/` / `util.cpp`. Feature-only logic stays in the component.
- **Constants** are `#define` at the top of the `.c/.cpp`, or a dedicated
  `.h` if several files share them. No magic numbers in function bodies.
- KeyMint must do real crypto: common
  `src/common/hardware/interfaces/security/keymint/aidl/default/soft`, not an
  empty stub.
- Link GSI-extracted `libbinder_ndk` / NDK stubs from lunch (`GSI_LIBS`,
  `NDK`, `AIDL`). Do not put `libc++_shared` on mapper SPHAL paths unless the
  search path is fixed like the graphics stub.
- Stop QEMU before replacing `vendor.img`. Keep userdata under
  `out/test-<variant>/`.

Do not init/push a Git repo unless asked. Report what was registered and what is
still missing.
