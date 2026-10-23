/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * hardware/interfaces/sensors/aidl/default/main.cpp
 */
#include "sensors-impl/Sensors.h"

#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>

using aidl::android::hardware::sensors::Sensors;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    auto sensor = ndk::SharedRefBase::make<Sensors>();
    const std::string sensorName = std::string() + Sensors::descriptor + "/default";
    binder_status_t status =
            AServiceManager_addService(sensor->asBinder().get(), sensorName.c_str());
    if (status != STATUS_OK) {
        ALOGE("addService %s failed: %d", sensorName.c_str(), status);
        return 1;
    }
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
