# QEMU boot notes (aosp_arm64-BP4A.251205.006)

```bash
echo gfxstream > ../GPU   # or virgl
SNAPSHOT=1 ./run aosp_arm64-BP4A.251205.006
```

- Portrait **1080 × 2400**, **420 dpi**
- GPU file only switch: `gfxstream` / `virgl`
- gfxstream: `gralloc=minigbm` prop + stub virtgpu alloc
- virgl: `gralloc=stub` + Mesa GBM
- Rebuild HALs: `source build/envsetup.sh && lunch aosp_arm64-BP4A.251205.006 && m`
