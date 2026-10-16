# Guest images

`system.img` is the MagicOS BKQ-AN00 16 (Honor, MysticGSI) GSI. `./run` downloads it
from the URL in `system.url` and checks it against `sha256sum.txt`
(`scripts/ensure-guest-downloads.sh`). It is never modified: changes to the
system partition are bind mounts from the initramfs (`qemu/init`).

`vendor.img` and `initramfs.img` are rebuilt by `./run` from `qemu/vendor`,
busybox and the kernel. All three images stay gitignored.
