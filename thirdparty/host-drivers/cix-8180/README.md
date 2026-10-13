# cix-8180: CIX Sky1 GPU userspace (MYT R1P)

Unmodified Debian packages from Radxa's CIX repository, installed by
`.ci/cix-r1p/Dockerfile` (only their `/opt` trees). Files, checksums and
download URLs are listed in `SOURCES`.

| Package | Contents |
|---|---|
| `cix-gpu-umd_2.0.0_arm64.deb` | Mali (kbase r54p1) EGL/GLES/GBM in `/opt/cixgpu-pro` |
| `cix-libglvnd_1.7.0_arm64.deb` | GLVND dispatch in `/opt/cixgpu-compat` |

The kernel driver (`cix-gpu-driver`) belongs to the R1P host, not here.
Vendor binaries under the vendor's terms: keep them unmodified and update
both packages together.
