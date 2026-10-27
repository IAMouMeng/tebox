# aosp_arm64-BP4A.251205.006

Official aosp_arm64 GSI (BP4A.251205.006). Kernel / virtio / guest modules stay
under `src/kernel/<id>/`. Vendor HALs come from Google Cuttlefish (CVD) +
`src/common/hardware`.

**只改本目录 `GPU` 文件，再 `./run`，即可切换后端**（不要设
`FORCE_RUTABAGA` / `FORCE_VIRGL`，否则会盖过该文件）：

```bash
echo gfxstream > GPU   # rutabaga + ANGLE；stub virtgpu + minigbm 句柄布局
echo virgl     > GPU   # Mesa + stub gralloc（主机有 VIRGL_VIDEO 时带 c2.mesa）
SNAPSHOT=1 ./run aosp_arm64-BP4A.251205.006
```

链路：`GPU` → `boot-qemu.sh`（`-device` + `androidboot.*`）→
`init.ranchu.rc`（写 `ro.hardware.egl` / `gralloc` / `hwui`）。
`qemu/vendor/build.prop` 里不要写死 `ro.hardware.gralloc` / `egl`。

| Piece | Source |
| --- | --- |
| GSI `system.img` | BP4A.251205.006 |
| Kernel | `KERNEL` → `gki-6.1-android14-gsi` |
| GPU | `GPU` file → gfxstream / virgl |
| Graphics | composer+allocator stub |
| Other HALs | `src/common/hardware/README.md` |

```bash
source build/envsetup.sh
lunch aosp_arm64-BP4A.251205.006
m
```
