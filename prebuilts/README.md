# Shared prebuilts (not GSI-variant specific)

| Path | Contents |
| --- | --- |
| `host/<os>-arm64/` | Installed QEMU, libepoxy, VirGL for that host |
| `android-arm64/` | Cross-built Mesa (`mesa/`, `mesa-install/`) and libdrm |

Variant-specific GSI pieces (`system.img`, link `.so`s) live under
`src/aosp/<variant>/images/`, not here.

Rebuild with `.ci/build-host.sh` / Android Mesa scripts; outputs refresh these trees.
