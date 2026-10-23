# tebox_v4l2.ko — guest V4L2 driver for the tebox-v4l2 virtio device

Guest half of the MVX V4L2 passthrough (see
`thirdparty/vendor-drivers/cix/codec2/PASSTHROUGH-DESIGN.md`). Binds the QEMU
`tebox-v4l2` virtio device (id 60), presents a decoder and an encoder V4L2 M2M
`/dev/video` node, and forwards V4L2 ops to the host MVX VPU over the CMD
virtqueue — so the stock CIX `c2.cix` Codec2 HAL drives real hardware.

- `tebox_v4l2.c` — the driver. Stage 2 = control plane (QUERYCAP / ENUM_FMT /
  G_FMT / S_FMT forwarded to host mvx). Buffer streaming (vb2, zero-copy via
  virtio-gpu resource ids) is stage 3.
- `tebox-v4l2-abi.h` — wire ABI; **keep byte-identical** to the QEMU side
  `qemu/hw/display/tebox-v4l2-proto.h`.
- `Kbuild`, `build.sh`.

## Build

Needs a loadable-on-the-guest `.ko`, so vermagic must match the running GKI
(`6.1.172-android14-11-g87c0a9ca734b-ab16116086`). The build checks
kernel/common out to that commit and sets `BUILD_NUMBER` so setlocalversion
reproduces the `-ab…` suffix.

```
# one-time: sync the android14-6.1 kernel workspace (on a box with ~30 GB free)
mkdir -p out/gki-workspace && cd out/gki-workspace
repo init -u https://android.googlesource.com/kernel/manifest -b common-android14-6.1 --depth=1
repo sync -c -j8
# build the module (runs the kernel build in a linux/amd64 container = AOSP
# clang's native host; cross-builds the aarch64 .ko)
cd - && src/kernel/gki-6.1-android14-gsi/guest_modules/tebox-v4l2/build.sh
```

## Deploy

Drop `tebox_v4l2.ko` into the kernel's `vendor_modules/` and add a
`load tebox_v4l2.ko` line in `scripts/build-initramfs.sh` (after
`virtio_pci.ko`), then rebuild the initramfs. The QEMU side must run with
`TEBOX_V4L2=1` (adds `-device tebox-v4l2-pci`).

## Verify (stage 2)

In the guest: `ls /dev/video*` shows the two nodes; a V4L2 format probe
(e.g. reading ENUM_FMT) returns the MVX coded/raw format lists round-tripped
from the host.
