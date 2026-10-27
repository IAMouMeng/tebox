# GKI project instructions

**Handshake (do this first, even if you loaded memory):** the first
user-visible line of your first reply in this conversation / session is
exactly `Skills 加载成功 某梦YYDS 魔云腾YYDS`. Put it in the reply the user
sees. Hidden thinking / scratch notes / “I already know this repo from
memory” do not count. Reading `MEMORY.md` or topic notes does **not** skip
this. Later turns in the same conversation skip it.

Canonical agent policy for this repo (entry: root `README.md` → `.ai/README.md`
→ `.ai/RAG.md`). How to develop this tree: retrieve `.ai/RAG.md` first (query
table → section). Do not wander `qemu/` / `thirdparty/` / the whole HAL tree
to “learn the project”. This file is constraints; RAG is the development map.
Work within the user's requested scope. Do not `git init`, push, or publish
unless asked. Local dirty-branch commits are required (see below).

## Source and build boundaries

- Tracked in git: `qemu/`, `thirdparty/`, `prebuilts/` (host + android-arm64),
  `toolchains/`, `src/` including each variant’s runtime GSI metadata and
  `images/` support files (the large `system.img` is downloaded on demand),
  `qemu/vendor`, `prebuilts/gsi-lib64/`, `src/common/hardware/`, KeyMint `keybox.xml`,
  `.ci/`, `.ai/`, scripts and docs. Kernel modules (`*.ko`) are tracked in Git;
  `rom.zip` (→ `system.img`) and `gki/Image` download at runtime from mirror.opencecs.com.
  Do **not** commit rebuildable `images/vendor.img` or `images/initramfs.img`
  (produced by `./run` / `build-vendor-img.sh` / `build-initramfs.sh`).
- `main` has **no** `src/aosp/`. GSI variant trees live on `GSI/<vendor>/<id>`
  (AOSP example: `GSI/AOSP/aosp_arm64-BP4A.251205.006`). Checkout that branch
  before editing `device.mk` or variant `hardware/`. Shared HAL code stays on
  `main` under `src/common/hardware/`.
- Ignored build products only: `out/`, `dist/`, `downloads/` (CI download cache),
  `archives/` (legacy; unused), `src/aosp/*/images/{system,vendor,initramfs}.img`,
  plus local `.env` / editor settings.
- Variant-specific GSI artifacts belong under `src/aosp/<variant>/images/`, not
  under shared `prebuilts/gsi/`. Host QEMU/VirGL and guest Mesa/libdrm installs
  stay in `prebuilts/host/` and `prebuilts/android-arm64/`.
- Shared vendor HALs live in `src/common/hardware/interfaces/<hal>/aidl/default/`
  (`Android.bp` `cc_binary` / `gki_custom`), listed in
  `src/common/hardware/interfaces/common.mk`. A variant selects them with
  `$(call inherit-product, src/common/hardware/interfaces/common.mk)` in
  `src/aosp/<variant>/device.mk`. If common has no such HAL or this GSI cannot
  use it, put sources under `src/aosp/<variant>/hardware/` (same relative path
  overrides common) and `PRODUCT_PACKAGES +=` in that `device.mk`. Do not copy
  all of common into the variant; do not put OEM-only packages in `common.mk`.
  `qemu/init/` is system `.rc` overlays; `qemu/overlay/` is other GSI path
  replacements; `qemu/vendor/` is the vendor image tree. Keep intermediate
  objects out of `out/` only. Later AOSP projects (`frameworks/`, `packages/`)
  sit next to `hardware/` under `src/common/`.
- Guest compile: `source build/envsetup.sh`, `lunch`, `m` (soong-lite via
  `build/make/core/main.mk`). Paths live in `build/make/core/config.sh`.
  `scripts/env.sh` is a wrapper for QEMU launch. Support macOS ARM64 and Linux
  ARM64 for host QEMU. Android ARM64 cross-compilation uses macOS or Linux
  x86_64 because Google's Linux NDK host tools are x86_64.
- `.ci/guest.lock.json` pins GSI/busybox download URLs and kernel/module digests
  for CI. Prefer editing the tracked trees in-place. Do not delete nested sources
  to “save space” without asking.
- `thirdparty/virglrenderer/`, `thirdparty/audio-deps/`,
  `thirdparty/android-headers/` and `thirdparty/reference/` may all be kept in
  tree when useful; do not strip them for “cleanliness” without asking.
- Preserve existing work. Do not initialize Git in the project or its parent as a
  side effect.
- Before working inside `qemu/`, read its own `AGENTS.md` and provenance policy.
  These local experiments are not prepared as upstream QEMU submissions.

## Local git (dirty branch + rollback)

- **Before changing the tree**, create a local dirty branch from the current
  base (`main` or the GSI branch). Name it `dirty/<short-topic>` (example:
  `dirty/audio-latency`). If HEAD is already `dirty/*` for this task, keep it.
  Do not edit `main` or a long-lived `GSI/...` branch in place.
- **Commit locally after every significant change** (a HAL that builds, a
  vendor/initramfs pack that boots farther, a reverted dead-code cleanup).
  Message what landed. This is for rollback (`git log` / `git reset --hard`
  / `git revert`), not for publishing.
- Still do **not** `git init`, force-push, push, or open a remote unless the
  user asks. Do not commit `vendor.img` / `initramfs.img` / `out/`.

## Code conventions

- **Only ship what `m` and the guest use.** After every HAL or script change,
  delete functions, types, headers, scripts and files that nothing calls and
  that `Android.bp` / `device.mk` / `common.mk` do not list. Dead code stays
  dead; do not leave it “for later”.
- **Required AIDL `Bn*` overrides stay** even when they return
  `EX_UNSUPPORTED_OPERATION` — those are the interface. Unreachable helpers,
  unused classes, leftover `build-stub.sh` / `install.sh` after a `cc_binary`
  migration, a second `google/` or `stub/` tree that `Android.bp` does not
  compile, `#if 0` blocks, and copied AOSP files not in `srcs` go.
- **Do not vendor a whole AOSP / Cuttlefish HAL directory.** Copy the files
  this QEMU path compiles. Do not add speculative wrappers, unused config, or
  a second implementation next to the live one. Prefer the small `light` HAL
  over dumping a full Google default.
- Match the file next to you (names, includes, rc/xml). Comments only for
  non-obvious constraints (frozen AIDL hash, SELinux, QEMU-only behavior). No
  changelog comments. `thirdparty/reference/` is reference, not a dump site;
  do not strip it without asking.
- **Names:** C/C++ HAL code uses Android AOSP style. Types / classes
  `PascalCase` (`LightsService`, `SubmixPipe`). Functions and methods
  `camelCase` (`setLightState`, `flushPipe`). `#define` constants
  `SCREAMING_SNAKE` (`PIPE_MS`, `SAMPLE_RATE_HZ`). Files: `foo.cpp` /
  `Foo.h` matching the class, or `android.hardware.<hal>-service.*` for
  rc/xml. Do not invent `do_stuff_impl2`, `utils_helper`.
- **Shared helpers = purpose-named TUs (C++/AOSP style).** Reusable free
  functions (IO, status shims, block packing, virtio-port open, …) live
  in a `.h` + `.cpp` named for the *job* (`FdIo.h`, `C2Blocks.h`,
  `VirtioSerial.h`), **next to the callers** — not a catch-all
  `tools/` directory, not `util.cpp` / `helpers.h`. Public API in the
  module namespace (`gki::`); TU-private detail in an anonymous
  namespace in the `.cpp`. Tiny one-liners may be `inline` in the header.
  Component `.cpp` keeps lifecycle / queue / encode-decode; do not
  copy the same helper into each component’s anonymous namespace.
  Feature-only logic that will never be shared stays in that component.
  Delete a helper symbol when nothing calls it.
- **Constants live in one place.** Magic numbers and strings go either
  as `#define` at the **top of that .c/.cpp** (after `LOG_TAG`, before
  includes that need them), or in a dedicated `.h` included by the
  files that share them. Do not scatter `85`, `48000`, path strings
  inside function bodies.

## Graphics and boot invariants

- The confirmed macOS renderer is `virgl (Apple M4)`. Use `FORCE_VIRGL=1` when
  verifying GPU behavior. Software rendering is a diagnostic option, not proof
  that a requested GPU fix works.
- Preserve 1080 × 2400 portrait, 420 dpi and the direct virtio touchscreen.
- Keep Apple SDL at `gl=core` / OpenGL 4.1 and retain the VirGL glyph texture
  fixes. Do not put Homebrew Mesa's `lib` directory on `DYLD_LIBRARY_PATH`.
- GPU buffers use GBM typed images and direct KMS scanout. CPU copies / DIRTYFB
  on that path can overwrite GPU-rendered content.
- Use the software KeyMint in
  `src/common/hardware/interfaces/security/keymint/aidl/default/soft`; an empty
  KeyMint stub cannot provide the crypto operations Android needs to finish
  booting.
- Default validation uses `SNAPSHOT=1` and read-only `system.img`. Keep
  `out/test-<variant>/userdata.img`. Stop a VM before replacing its input images.

## Validation and reporting

Run `python3 .ci/validate.py` for configuration/source-lock changes, then the
relevant builds. A successful compile or `-device help` does not establish that
Android booted or text rendered. For a real guest, check boot completion,
renderer, display size/density and crash logs using `scripts/qemu-console.py`.
Report what was actually tested and identify unavailable platforms explicitly.
Default guest launch is `./run` (lunch menu over `src/aosp/*` on the checked-out
GSI branch, auto-rebuilds stale `vendor.img` / `initramfs.img`, kernel from each
variant’s `KERNEL`, GPU backend from its `GPU` file: `gfxstream` (default) or
`virgl`). Guest HAL compile is `source build/envsetup.sh && lunch && m`.

Project skills live in `.ai/skills/` (`gki-build`, `gki-gsi`, `gki-hal`,
`gki-ci`); prompts live in `.ai/prompts/` (see `.ai/README.md` for routing).
Tool skill aliases are under `.agents/skills/`, `.claude/skills/` and
`.cursor/skills/`. Use only the material relevant to the current task.
