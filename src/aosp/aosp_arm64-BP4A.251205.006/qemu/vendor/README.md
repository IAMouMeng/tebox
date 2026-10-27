# vendor — `/vendor` image tree (AOSP `out/target/product/*/vendor`)

Edit **device** files here:

| Path | Role |
| --- | --- |
| `etc/fstab.ranchu` | vendor fstab |
| `etc/build.prop` `build.prop` | vendor props |
| `etc/ueventd.rc` | ueventd |
| `etc/selinux/` | vendor sepolicy |
| `etc/ld.config.txt` | linker config |
| `etc/init/hw/` | `init.${ro.hardware}.rc` placeholder |
| `etc/init/*.rc` | device rc (camera-launcher, tebox-logcat) |
| `usr/idc/` | input |

Do **not** hand-edit HAL install products. They come from `Android.bp`:

- `bin/hw/android.hardware.*`
- `etc/init/android.hardware.*.rc`
- `etc/vintf/manifest/android.hardware.*.xml`

Sources: `src/common/hardware/`. Product list: `../../device.mk`.
