/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * NDK port of hardware/interfaces/power/aidl/default (IPower v1).
 */
#define LOG_TAG "android.hardware.power-service.example"

#include <aidl/android/hardware/power/BnPower.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>

#ifndef ALOGD
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#endif
#ifndef ALOGI
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#endif

using aidl::android::hardware::power::Boost;
using aidl::android::hardware::power::Mode;
using ndk::ScopedAStatus;

namespace aidl {
namespace android {
namespace hardware {
namespace power {
namespace impl {

class Power : public BnPower {
  public:
    ndk::ScopedAStatus setMode(Mode type, bool enabled) override {
        ALOGD("Power setMode: %d to: %s", static_cast<int>(type), enabled ? "on" : "off");
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus isModeSupported(Mode type, bool* _aidl_return) override {
        ALOGD("Power isModeSupported: %d", static_cast<int>(type));
        *_aidl_return = false;
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus setBoost(Boost type, int32_t durationMs) override {
        ALOGD("Power setBoost: %d, duration: %d", static_cast<int>(type), durationMs);
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus isBoostSupported(Boost type, bool* _aidl_return) override {
        ALOGD("Power isBoostSupported: %d", static_cast<int>(type));
        *_aidl_return = false;
        return ndk::ScopedAStatus::ok();
    }
};

}  // namespace impl
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(1);
    auto power = ndk::SharedRefBase::make<
            aidl::android::hardware::power::impl::Power>();
    const std::string instance = std::string(power->descriptor) + "/default";
    binder_status_t status =
            AServiceManager_addService(power->asBinder().get(), instance.c_str());
    ALOGI("register %s: %d", instance.c_str(), status);
    if (status != STATUS_OK) return 1;
    ABinderProcess_joinThreadPool();
    return 1;
}
