/*
 * Soft AIDL CachedScanData builder for QEMU. Used as a fail-path hook from
 * WifiStaIface::getCachedScanDataInternal when the legacy HAL path fails.
 */
#pragma once

#include <aidl/android/hardware/wifi/CachedScanData.h>

namespace aidl {
namespace android {
namespace hardware {
namespace wifi {

/** Fixed-for-boot fake AP catalog matching BP4A / OriginOS soft WiFi stubs. */
CachedScanData makeSoftCachedScanData();

}  // namespace wifi
}  // namespace hardware
}  // namespace android
}  // namespace aidl
