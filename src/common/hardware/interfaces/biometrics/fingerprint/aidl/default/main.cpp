/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * NDK port of biometrics/fingerprint/aidl/default/main.cpp.
 * Registers IFingerprint/default (GSI VINTF v4). VirtualHal instance omitted.
 */
#include "Fingerprint.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

using aidl::android::hardware::biometrics::fingerprint::Fingerprint;

int main(int argc, char** argv) {
    const char* mode = (argc >= 2) ? argv[1] : "default";
    LOG(INFO) << "Fingerprint HAL started: " << mode;
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    std::shared_ptr<Fingerprint> hal = ndk::SharedRefBase::make<Fingerprint>();
    if (!hal->connected()) {
        LOG(ERROR) << "Fingerprint HAL is not connected";
        return EXIT_FAILURE;
    }
    const std::string instance = std::string(Fingerprint::descriptor) + "/default";
    binder_status_t status = AServiceManager_addService(hal->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK);
    LOG(INFO) << "started IFingerprint/default";
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
