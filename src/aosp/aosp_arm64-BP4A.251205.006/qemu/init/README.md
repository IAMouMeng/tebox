# init — system `.rc` overlays only

Each file here is an Android init rc bind-mounted over the GSI:

| File | Guest path |
| --- | --- |
| `init.rc` | `/system/etc/init/hw/init.rc` |
| `init.ranchu.rc` | `/init.ranchu.rc` (or vendor placeholder) |
| `keystore2.rc` | `/system/etc/init/keystore2.rc` |
| `surfaceflinger.rc` | `/system/etc/init/surfaceflinger.rc` |
| `virtual_camera.hal.rc` | `/system/etc/init/virtual_camera.hal.rc` |
| `android.hardware.security.keymint-service.rc` | GSI system KeyMint rc (masked) |

Vendor HAL rc files live next to the HAL
(`src/common/hardware/interfaces/<hal>/aidl/default/*.rc`) and
install into `/vendor/etc/init`.

Non-rc system overlays (wificond, …) live in `../overlay/`.
