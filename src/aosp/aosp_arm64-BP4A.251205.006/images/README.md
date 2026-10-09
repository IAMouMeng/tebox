# Guest images

`system.img` is the pinned Android GSI runtime input. It is not tracked in Git;
`./run` downloads it from the URL in `.ci/guest.lock.json` when it is missing.
Its digest is recorded in `sha256sum.txt`. A valid custom GSI may be placed at
this path and is kept instead of downloading the pinned image.

The GKI boot `Image` for this variant’s kernel id lives at
`src/kernel/<id>/gki/Image` and is also downloaded at runtime (see
`.ci/guest.lock.json` → `kernel`).

`vendor.img` and `initramfs.img` are rebuilt by `./run` from `qemu/vendor`,
busybox and the kernel. They stay gitignored.
