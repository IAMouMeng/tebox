/*
 * Soft QEMU legacy WiFi HAL hooks. New logic lives here; call
 * installSoftLegacyHalHooks() after initHalFuncTableWithStubs fills NOT_SUPPORTED
 * stubs so createStaIface / cached scan work without a vendor .so.
 */
#pragma once

#include "wifi_legacy_hal.h"

namespace aidl {
namespace android {
namespace hardware {
namespace wifi {
namespace legacy_hal {

/** Override critical wifi_hal_fn entries with soft QEMU implementations. */
void installSoftLegacyHalHooks(wifi_hal_fn* hal_fn);

}  // namespace legacy_hal
}  // namespace wifi
}  // namespace hardware
}  // namespace android
}  // namespace aidl
