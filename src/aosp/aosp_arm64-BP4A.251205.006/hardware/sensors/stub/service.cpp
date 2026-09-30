// Soft Sensors AIDL v2: empty sensor list for QEMU (no physical sensors).
#include <aidl/android/hardware/sensors/BnSensors.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>

using namespace aidl::android::hardware::sensors;
using aidl::android::hardware::common::fmq::MQDescriptor;
using aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using ndk::ScopedAStatus;

class QemuSensors : public BnSensors {
  public:
    ScopedAStatus activate(int32_t, bool) override {
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    ScopedAStatus batch(int32_t, int64_t, int64_t) override {
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    ScopedAStatus configDirectReport(int32_t, int32_t, ISensors::RateLevel, int32_t* out) override {
        *out = ERROR_BAD_VALUE;
        return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    ScopedAStatus flush(int32_t) override {
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    ScopedAStatus getSensorsList(std::vector<SensorInfo>* out) override {
        out->clear();
        return ScopedAStatus::ok();
    }
    ScopedAStatus initialize(const MQDescriptor<Event, SynchronizedReadWrite>&,
                             const MQDescriptor<int32_t, SynchronizedReadWrite>&,
                             const std::shared_ptr<ISensorsCallback>&) override {
        // No sensors to feed; accept the framework FMQ setup and return.
        return ScopedAStatus::ok();
    }
    ScopedAStatus injectSensorData(const Event&) override {
        return ScopedAStatus::fromServiceSpecificError(ERROR_BAD_VALUE);
    }
    ScopedAStatus registerDirectChannel(const ISensors::SharedMemInfo&, int32_t* out) override {
        *out = ERROR_BAD_VALUE;
        return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    ScopedAStatus setOperationMode(ISensors::OperationMode) override {
        return ScopedAStatus::ok();
    }
    ScopedAStatus unregisterDirectChannel(int32_t) override {
        return ScopedAStatus::ok();
    }
};

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(1);
    auto service = ndk::SharedRefBase::make<QemuSensors>();
    constexpr auto instance = "android.hardware.sensors.ISensors/default";
    auto status = AServiceManager_addService(service->asBinder().get(), instance);
    __android_log_print(status == STATUS_OK ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
                        "qemu-sensors", "register %s: %d", instance, status);
    if (status != STATUS_OK) return 1;
    ABinderProcess_joinThreadPool();
    return 1;
}
