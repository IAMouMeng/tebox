# src/common/hardware — AOSP `hardware/` project

Shared vendor HAL defaults. Other common AOSP projects (`frameworks/`,
`packages/`) sit next to this directory under `src/common/` when they are
added. Device trees stay under `src/aosp/<variant>/`.

```
src/common/hardware/interfaces/<hal>/aidl/default/   # cc_binary / gki_custom + rc + vintf xml
src/common/hardware/interfaces/common.mk             # generic PRODUCT_PACKAGES
src/common/hardware/shim/                            # NDK compatibility headers
build/soong/soong.py                                 # Soong-lite (`source build/envsetup.sh && lunch && m`)
src/aosp/<variant>/device.mk                         # inherit common.mk, extra packages
src/aosp/<variant>/hardware/                         # this-GSI HALs when common is not enough
```

GSI trees live on `GSI/...` branches. `main` has no `src/aosp/`.

## Product

```makefile
# src/aosp/<variant>/device.mk
$(call inherit-product, src/common/hardware/interfaces/common.mk)

# Only if common has no such HAL (or this GSI cannot use the common binary):
PRODUCT_PACKAGES += android.hardware.foo-service
```

A new variant needs that `inherit-product`. `m` runs soong-lite, then
`build-vendor-img.sh` packs `qemu/vendor`.

## Own HAL (common is not enough)

- **All GSIs / shared bug** → edit `src/common/hardware/interfaces/<hal>/aidl/default/` and list it in `common.mk`. Do not put OEM-only packages there.
- **This GSI only, new module** → `src/aosp/<variant>/hardware/<hal>/aidl/default/` (`Android.bp` + rc + xml) and `PRODUCT_PACKAGES +=` in that `device.mk`.
- **This GSI cannot use the common binary** → same relative path under the variant `hardware/` (soong-lite overrides common). Keep the module name.

Copy `src/common/hardware/interfaces/light/aidl/default/` as the layout. Do not copy the whole common tree into the variant.

Delete unused functions and files in the same change: every leftover `.cpp/.h/.sh` must be in `Android.bp` `srcs` or the `gki_custom` `build.sh`; every leftover function needs a caller. Required AIDL `Bn*` overrides stay. Drop orphan stub scripts, duplicate `google/` vs `stub/` trees, and copied AOSP files this binary does not compile.

Names: classes `PascalCase`, functions `camelCase`, `#define` `SCREAMING_SNAKE`. Do not extract a helper that has one caller (`util.cpp` / `tools.cpp` for a single user). Constants are `#define` at the top of the file, or a dedicated `.h` if shared.

## Android.bp

```
cc_binary {
    name: "android.hardware.lights-service",
    relative_install_path: "hw",
    vendor: true,
    srcs: ["lights.cpp"],
    shared_libs: [
        "libbinder_ndk",
        "liblog",
        "android.hardware.light-V2-ndk",
    ],
    init_rc: ["android.hardware.lights-service.rc"],
    vintf_fragments: ["android.hardware.lights-service.xml"],
}
```

`android.hardware.*-V<n>-ndk` in `shared_libs` generates frozen NDK AIDL (no
full Soong). QEMU-only HALs that still use a script are `gki_custom`.

## Build

```bash
source build/envsetup.sh
lunch aosp_arm64-BP4A.251205.006
m
m android.hardware.lights-service
```

Kernel drivers stay in `src/kernel/`.
