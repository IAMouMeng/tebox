/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * NDK adaptation of hardware/interfaces/sensors/aidl/default/Sensors.cpp.
 * Sensor models stay in AOSP Sensor.cpp; the event FMQ uses EventQueue.
 */
#include "sensors-impl/Sensors.h"

#include "event-queue.h"

#include <aidl/android/hardware/common/fmq/SynchronizedReadWrite.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>

using ::aidl::android::hardware::common::fmq::MQDescriptor;
using ::aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using ::ndk::ScopedAStatus;

namespace aidl {
namespace android {
namespace hardware {
namespace sensors {

constexpr uint32_t kReadAndProcess =
        static_cast<uint32_t>(ISensors::EVENT_QUEUE_FLAG_BITS_READ_AND_PROCESS);

Sensors::Sensors() {
    AddSensor<AccelSensor>();
    AddSensor<GyroSensor>();
    AddSensor<AmbientTempSensor>();
    AddSensor<PressureSensor>();
    AddSensor<MagnetometerSensor>();
    AddSensor<LightSensor>();
    AddSensor<ProximitySensor>();
    AddSensor<RelativeHumiditySensor>();
    AddSensor<HingeAngleSensor>();
}

ScopedAStatus Sensors::activate(int32_t in_sensorHandle, bool in_enabled) {
    auto sensor = mSensors.find(in_sensorHandle);
    if (sensor != mSensors.end()) {
        sensor->second->activate(in_enabled);
        return ScopedAStatus::ok();
    }
    return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}

ScopedAStatus Sensors::batch(int32_t in_sensorHandle, int64_t in_samplingPeriodNs,
                             int64_t /* in_maxReportLatencyNs */) {
    auto sensor = mSensors.find(in_sensorHandle);
    if (sensor != mSensors.end()) {
        sensor->second->batch(in_samplingPeriodNs);
        return ScopedAStatus::ok();
    }
    return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}

ScopedAStatus Sensors::configDirectReport(int32_t, int32_t, ISensors::RateLevel, int32_t* out) {
    *out = EX_UNSUPPORTED_OPERATION;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ScopedAStatus Sensors::flush(int32_t in_sensorHandle) {
    auto sensor = mSensors.find(in_sensorHandle);
    if (sensor != mSensors.end()) return sensor->second->flush();
    return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}

ScopedAStatus Sensors::getSensorsList(std::vector<SensorInfo>* _aidl_return) {
    for (const auto& sensor : mSensors) {
        _aidl_return->push_back(sensor.second->getSensorInfo());
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Sensors::initialize(
        const MQDescriptor<Event, SynchronizedReadWrite>& in_eventQueueDescriptor,
        const MQDescriptor<int32_t, SynchronizedReadWrite>& /* wakeLock */,
        const std::shared_ptr<ISensorsCallback>& in_sensorsCallback) {
    ALOGI("Sensors initializing");
    for (auto& sensor : mSensors) sensor.second->activate(false);
    mCallback = in_sensorsCallback;
    mEventQueue = std::make_unique<EventQueue<Event>>();
    if (!mEventQueue->initialize(in_eventQueueDescriptor)) {
        mEventQueue.reset();
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Sensors::injectSensorData(const Event& in_event) {
    auto sensor = mSensors.find(in_event.sensorHandle);
    if (sensor != mSensors.end()) return sensor->second->injectEvent(in_event);
    return ScopedAStatus::fromServiceSpecificError(
            static_cast<int32_t>(BnSensors::ERROR_BAD_VALUE));
}

ScopedAStatus Sensors::registerDirectChannel(const ISensors::SharedMemInfo&, int32_t* out) {
    *out = EX_UNSUPPORTED_OPERATION;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ScopedAStatus Sensors::setOperationMode(ISensors::OperationMode in_mode) {
    for (auto& sensor : mSensors) sensor.second->setOperationMode(in_mode);
    return ScopedAStatus::ok();
}

ScopedAStatus Sensors::unregisterDirectChannel(int32_t) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

void Sensors::postEvents(const std::vector<Event>& events, bool /*wakeup*/) {
    std::lock_guard<std::mutex> lock(mWriteLock);
    if (!mEventQueue) return;
    for (const auto& event : events) {
        if (!mEventQueue->write(event, kReadAndProcess)) break;
    }
}

}  // namespace sensors
}  // namespace hardware
}  // namespace android
}  // namespace aidl
