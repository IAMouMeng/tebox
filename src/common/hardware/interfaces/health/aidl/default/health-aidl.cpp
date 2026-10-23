/*
 * Copyright (C) 2018 The Android Open Source Project
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
 * NDK port of cuttlefish/guest/hals/health/health-aidl.cpp (android-16.0.0_r4).
 * Fake-battery values and method overrides stay with CVD; binder is BnHealth v3.
 */
#define LOG_TAG "android.hardware.health-service.cuttlefish"

#include <aidl/android/hardware/health/BatteryCapacityLevel.h>
#include <aidl/android/hardware/health/BatteryChargingPolicy.h>
#include <aidl/android/hardware/health/BatteryHealth.h>
#include <aidl/android/hardware/health/BatteryHealthData.h>
#include <aidl/android/hardware/health/BatteryStatus.h>
#include <aidl/android/hardware/health/BnHealth.h>
#include <aidl/android/hardware/health/DiskStats.h>
#include <aidl/android/hardware/health/HealthInfo.h>
#include <aidl/android/hardware/health/IHealthInfoCallback.h>
#include <aidl/android/hardware/health/StorageInfo.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>

#ifndef ALOGI
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#endif
#ifndef ALOGE
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#endif

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace aidl::android::hardware::health;
using ndk::ScopedAStatus;
using ndk::SharedRefBase;

namespace {

HealthInfo makeCvdHealthInfo() {
    // HealthImpl::UpdateHealthInfo in CVD health-aidl.cpp.
    HealthInfo info;
    info.chargerAcOnline = true;
    info.chargerUsbOnline = true;
    info.chargerWirelessOnline = false;
    info.chargerDockOnline = false;
    info.maxChargingCurrentMicroamps = 500000;
    info.maxChargingVoltageMicrovolts = 5000000;
    info.batteryStatus = BatteryStatus::CHARGING;
    info.batteryHealth = BatteryHealth::GOOD;
    info.batteryPresent = true;
    info.batteryLevel = 85;
    info.batteryVoltageMillivolts = 3600;
    info.batteryTemperatureTenthsCelsius = 250;
    info.batteryCurrentMicroamps = 400000;
    info.batteryCycleCount = 32;
    info.batteryFullChargeUah = 4000000;
    info.batteryChargeCounterUah = 1900000;
    info.batteryTechnology = "Li-ion";
    info.batteryCapacityLevel = BatteryCapacityLevel::HIGH;
    return info;
}

class HealthImpl : public BnHealth {
  public:
    ScopedAStatus registerCallback(const std::shared_ptr<IHealthInfoCallback>& cb) override {
        if (!cb) return ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
        {
            std::lock_guard<std::mutex> lock(mu_);
            callbacks_.push_back(cb);
        }
        cb->healthInfoChanged(makeCvdHealthInfo());
        return ScopedAStatus::ok();
    }

    ScopedAStatus unregisterCallback(const std::shared_ptr<IHealthInfoCallback>& cb) override {
        if (!cb) return ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
        std::lock_guard<std::mutex> lock(mu_);
        auto it = std::remove(callbacks_.begin(), callbacks_.end(), cb);
        if (it == callbacks_.end()) return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        callbacks_.erase(it, callbacks_.end());
        return ScopedAStatus::ok();
    }

    ScopedAStatus update() override {
        HealthInfo info = makeCvdHealthInfo();
        std::vector<std::shared_ptr<IHealthInfoCallback>> copy;
        {
            std::lock_guard<std::mutex> lock(mu_);
            copy = callbacks_;
        }
        for (auto& cb : copy) {
            auto st = cb->healthInfoChanged(info);
            if (st.isOk()) continue;
            std::lock_guard<std::mutex> lock(mu_);
            callbacks_.erase(std::remove(callbacks_.begin(), callbacks_.end(), cb),
                             callbacks_.end());
        }
        return ScopedAStatus::ok();
    }

    ScopedAStatus getChargeCounterUah(int32_t* out) override {
        *out = 1900000;
        return ScopedAStatus::ok();
    }
    ScopedAStatus getCurrentNowMicroamps(int32_t* out) override {
        *out = 400000;
        return ScopedAStatus::ok();
    }
    ScopedAStatus getCurrentAverageMicroamps(int32_t*) override {
        return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    ScopedAStatus getCapacity(int32_t* out) override {
        *out = 85;
        return ScopedAStatus::ok();
    }
    ScopedAStatus getEnergyCounterNwh(int64_t* out) override {
        *out = 6840000;
        return ScopedAStatus::ok();
    }
    ScopedAStatus getChargeStatus(BatteryStatus* out) override {
        *out = BatteryStatus::CHARGING;
        return ScopedAStatus::ok();
    }
    ScopedAStatus getStorageInfo(std::vector<StorageInfo>* out) override {
        out->clear();
        return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    ScopedAStatus getDiskStats(std::vector<DiskStats>* out) override {
        out->clear();
        return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    ScopedAStatus getHealthInfo(HealthInfo* out) override {
        *out = makeCvdHealthInfo();
        return ScopedAStatus::ok();
    }
    ScopedAStatus setChargingPolicy(BatteryChargingPolicy value) override {
        policy_ = value;
        return ScopedAStatus::ok();
    }
    ScopedAStatus getChargingPolicy(BatteryChargingPolicy* out) override {
        *out = policy_;
        return ScopedAStatus::ok();
    }
    ScopedAStatus getBatteryHealthData(BatteryHealthData* out) override {
        // CVD HealthImpl::getBatteryHealthData.
        out->batteryManufacturingDateSeconds = 1689787603;
        out->batteryFirstUsageSeconds = 1691256403;
        out->batteryStateOfHealth = 99;
        out->batterySerialNumber = "d1f92fe7591ff096ca3a29c450a5a3d1";
        return ScopedAStatus::ok();
    }

  private:
    std::mutex mu_;
    std::vector<std::shared_ptr<IHealthInfoCallback>> callbacks_;
    BatteryChargingPolicy policy_ = BatteryChargingPolicy::DEFAULT;
};

}  // namespace

int main() {
    ALOGI("Starting health HAL.");
    ABinderProcess_setThreadPoolMaxThreadCount(1);
    ABinderProcess_startThreadPool();
    auto binder = SharedRefBase::make<HealthImpl>();
    constexpr auto instance = "android.hardware.health.IHealth/default";
    auto status = AServiceManager_addService(binder->asBinder().get(), instance);
    if (status != STATUS_OK) {
        ALOGE("addService %s failed: %d", instance, status);
        return 1;
    }
    ABinderProcess_joinThreadPool();
    return 1;
}
