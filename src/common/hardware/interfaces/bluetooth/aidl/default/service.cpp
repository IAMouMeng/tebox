/*
 * Copyright 2024 The Android Open Source Project
 *
 * NDK binder registration for the CVD IBluetoothHci C++ port (hci.rs).
 */
#define LOG_TAG "bluetooth-cf"

#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>

#include "BluetoothHci.h"

using ::aidl::android::hardware::bluetooth::impl::BluetoothHci;

int main(int /* argc */, char** /* argv */) {
  ALOGI("Bluetooth HAL starting");
  ABinderProcess_setThreadPoolMaxThreadCount(0);
  ABinderProcess_startThreadPool();
  auto service = ndk::SharedRefBase::make<BluetoothHci>();
  const std::string instance =
      std::string() + BluetoothHci::descriptor + "/default";
  binder_status_t status =
      AServiceManager_addService(service->asBinder().get(), instance.c_str());
  if (status != STATUS_OK) {
    ALOGE("Failed to register %s: %d", instance.c_str(), status);
    return 1;
  }
  ABinderProcess_joinThreadPool();
  return 0;
}
