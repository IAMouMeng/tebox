# Guest images

`system.img` is the OneUI 16 pa3q (Samsung Galaxy S25 Ultra SM-S938B, MysticGSI) GSI. `./run` downloads it
from the URL in `system.url` and checks it against `sha256sum.txt`
(`scripts/ensure-guest-downloads.sh`). It is never modified: changes to the
system partition are bind mounts from the initramfs (`qemu/init`).

`vendor.img` and `initramfs.img` are rebuilt by `./run` from `qemu/vendor`,
busybox and the kernel. All three images stay gitignored.
