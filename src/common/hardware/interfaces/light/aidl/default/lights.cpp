// C++ NDK port of hardware/interfaces/light/aidl/default/lights.rs
// (android-16.0.0_r4). In-memory backlight entries; QEMU has no LED sysfs.
#define LOG_TAG "lights_service_example"

#include <aidl/android/hardware/light/BnLights.h>
#include <aidl/android/hardware/light/HwLight.h>
#include <aidl/android/hardware/light/HwLightState.h>
#include <aidl/android/hardware/light/LightType.h>
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

using aidl::android::hardware::light::BnLights;
using aidl::android::hardware::light::HwLight;
using aidl::android::hardware::light::HwLightState;
using aidl::android::hardware::light::LightType;
using ndk::ScopedAStatus;

namespace {

constexpr int32_t kNumDefaultLights = 3;

class LightsService : public BnLights {
  public:
    LightsService() {
        for (int32_t id = 1; id <= kNumDefaultLights; ++id) {
            lights_[id] = Light{
                    .hw = {.id = id, .ordinal = id, .type = LightType::BACKLIGHT},
                    .state = {},
            };
        }
    }

    ScopedAStatus setLightState(int32_t id, const HwLightState& state) override {
        LOG(INFO) << "Lights setting state for id=" << id << " to color " << std::hex
                  << state.color;
        std::lock_guard<std::mutex> lock(mu_);
        auto it = lights_.find(id);
        if (it == lights_.end()) {
            return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }
        it->second.state = state;
        return ScopedAStatus::ok();
    }

    ScopedAStatus getLights(std::vector<HwLight>* out) override {
        LOG(INFO) << "Lights reporting supported lights";
        std::lock_guard<std::mutex> lock(mu_);
        out->clear();
        out->reserve(lights_.size());
        for (const auto& [id, light] : lights_) {
            out->push_back(light.hw);
        }
        return ScopedAStatus::ok();
    }

  private:
    struct Light {
        HwLight hw;
        HwLightState state;
    };
    std::mutex mu_;
    std::map<int32_t, Light> lights_;
};

}  // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    std::shared_ptr<LightsService> svc = ndk::SharedRefBase::make<LightsService>();
    const std::string instance = std::string(LightsService::descriptor) + "/default";
    binder_status_t status = AServiceManager_addService(svc->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK);
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
