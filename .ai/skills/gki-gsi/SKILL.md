---
name: gki-gsi
description: Adapt this project to a new Android GSI system.img (new build ID / fingerprint / FCM level).
---

Locate the GKI workspace; read root `README.md`, then `.ai/AGENTS.md`, the
active variant’s `FETCHED_FROM.txt`, `KERNEL`, and `.ci/guest.lock.json`.

## When this skill applies

The user is swapping or upgrading `system.img` (new BP* / AP* ID, new SDK, new
GSI zip), **or** bringing up a manufacturer / MysticGSI image (OneUI, ColorOS,
OriginOS, Flyme, …). Goal: boot on the existing QEMU + vendor stack with the
smallest necessary changes.

## OEM family matrix (prompts + goldens)

| Family | Prompt | Living reference tree |
| --- | --- | --- |
| AOSP official | `.ai/prompts/gsi.md` | `src/aosp/aosp_arm64-BP4A.251205.006/` |
| Any OEM (router) | `.ai/prompts/oem-adapt.md` | pick family below |
| Samsung OneUI | `.ai/prompts/oem-oneui.md` | `src/aosp/Oneui-a25x-14-…/` |
| ColorOS / OriginOS | `.ai/prompts/oem-coloros-originos.md` | `src/aosp/Originos-PD2266-15-…/` (ColorOS dirs may be empty donors) |
| Flyme | `.ai/prompts/oem-flyme.md` | often images-only; do not invent HALs |

OEM images are usually hand-placed (no `guest.lock.json` URL). Copy the closest
populated donor, record it in `FETCHED_FROM.txt`, then diff against boot logs.

## Procedure

1. **Identify the new image**
   - Record `BUILD_ID`, incremental, fingerprint, Android version/SDK from
     `build.prop` or the download page.
   - Prefer official `aosp_arm64` user/userdebug GSI. Note sparse vs raw; convert
     with `simg2img` when needed.
   - Update `src/aosp/<variant>/FETCHED_FROM.txt` and
     `.ci/guest.lock.json` (`system.url`, `system.sha256`, `variant` if renamed).

2. **Place the image**
   - Install as `src/aosp/<variant>/images/system.img` for local testing; the
     runtime downloader fills this path when it is absent.
   - Extract link libs with `bash scripts/extract-gsi-libs.sh` into
     `prebuilts/gsi-lib64/`.
   - Stop any running QEMU before replacing images. Use `SNAPSHOT=1` for trials.

3. **Re-check kernel pairing**
   - Read `KERNEL` → `src/kernel/<id>/`. Confirm the GKI `Image` / virtio `.ko`
     still load under the new userspace. If FCM/kernel requirements jump (e.g.
     6.1 → 6.12), refresh prebuilts and digests in `.ci/guest.lock.json` /
     `FETCHED_FROM.txt` before debugging framework crashes.

4. **VINTF / missing HALs**
   - Boot once and collect `servicemanager` “Could not find … in the VINTF
     manifest” and `init:` restart loops.
   - Map each missing interface to an existing soft HAL under
     `src/aosp/<variant>/hardware/` or add a new one (use the `gki-hal` skill).
   - Framework compatibility matrices under extracted GSI VINTF / `out/gsi-vintf`
     show allowed AIDL versions (e.g. health 1–3, audio.core 1–3).

5. **Vendor / initramfs**
   - After HAL or Mesa changes: stop QEMU → `scripts/build-hals.sh` (as needed) →
     `scripts/build-vendor-img.sh` → `scripts/build-initramfs.sh`.
   - Keep SELinux permissive for bring-up unless the user asks otherwise
     (`androidboot.selinux=permissive` in `scripts/boot-qemu.sh`).

6. **Props and product identity**
   - Vendor `build.prop` / `*.prop` under `hardware/*/props` and `vendor` must not
     fight the new system fingerprint in ways that break zygote or SurfaceFlinger.
   - Preserve project display defaults (portrait 1080×2400 / 420 dpi) unless the
     new GSI task explicitly changes them.

7. **Validate**
   - `FORCE_VIRGL=1 SNAPSHOT=1 ./run` (or host package `QEMU=… ./run`).
   - Report boot progress, which HALs registered, and remaining missing interfaces.
   - Do not claim success from compile-only checks.

Do not initialize or publish a Git repository unless the user explicitly asks.
