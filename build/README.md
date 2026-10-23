# build — AOSP envsetup / lunch / m

Guest HAL compile uses the AOSP command sequence. It does not call `scripts/`.

```bash
source build/envsetup.sh
lunch aosp_arm64-BP4A.251205.006
m
m android.hardware.lights-service
```

| Path | Role |
| --- | --- |
| `build/envsetup.sh` | `lunch`, `m`, `printconfig`, `croot` |
| `build/make/core/config.sh` | NDK / product paths after lunch |
| `build/soong/` | Soong-lite (`Android.bp` + `device.mk`) |

`lunch` lists `src/aosp/<product>/` folder names that have `KERNEL` plus `images/` or `qemu/`.

QEMU launch stays `./run`. Host QEMU/Mesa builds stay under `.ci/` and `scripts/`.
