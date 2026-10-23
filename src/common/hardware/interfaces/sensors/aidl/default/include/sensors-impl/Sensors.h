/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * NDK header matching hardware/interfaces/sensors/aidl/default/include/sensors-impl/Sensors.h
 * with EventQueue instead of AidlMessageQueue.
 */
#pragma once

#include <aidl/android/hardware/common/fmq/SynchronizedReadWrite.h>
#include <aidl/android/hardware/sensors/BnSensors.h>
#include <map>
#include <memory>
#include <mutex>

#include "Sensor.h"
#include "event-queue.h"

namespace aidl {
namespace android {
namespace hardware {
namespace sensors {

class Sensors : public BnSensors, public ISensorsEventCallback {
  public:
    Sensors();
    ~Sensors() override = default;

    ndk::ScopedAStatus activate(int32_t in_sensorHandle, bool in_enabled) override;
    ndk::ScopedAStatus batch(int32_t in_sensorHandle, int64_t in_samplingPeriodNs,
                             int64_t in_maxReportLatencyNs) override;
    ndk::ScopedAStatus configDirectReport(
            int32_t in_sensorHandle, int32_t in_channelHandle,
            ISensors::RateLevel in_rate, int32_t* _aidl_return) override;
    ndk::ScopedAStatus flush(int32_t in_sensorHandle) override;
    ndk::ScopedAStatus getSensorsList(std::vector<SensorInfo>* _aidl_return) override;
    ndk::ScopedAStatus initialize(
            const aidl::android::hardware::common::fmq::MQDescriptor<
                    Event, aidl::android::hardware::common::fmq::SynchronizedReadWrite>&
                    in_eventQueueDescriptor,
            const aidl::android::hardware::common::fmq::MQDescriptor<
                    int32_t, aidl::android::hardware::common::fmq::SynchronizedReadWrite>&
                    in_wakeLockDescriptor,
            const std::shared_ptr<ISensorsCallback>& in_sensorsCallback) override;
    ndk::ScopedAStatus injectSensorData(const Event& in_event) override;
    ndk::ScopedAStatus registerDirectChannel(const ISensors::SharedMemInfo& in_mem,
                                             int32_t* _aidl_return) override;
    ndk::ScopedAStatus setOperationMode(ISensors::OperationMode in_mode) override;
    ndk::ScopedAStatus unregisterDirectChannel(int32_t in_channelHandle) override;

    void postEvents(const std::vector<Event>& events, bool wakeup) override;

  protected:
    template <class SensorType>
    void AddSensor() {
        auto sensor = std::make_shared<SensorType>(mNextHandle++, this);
        mSensors[sensor->getSensorInfo().sensorHandle] = sensor;
    }

  private:
    std::unique_ptr<EventQueue<Event>> mEventQueue;
    std::shared_ptr<ISensorsCallback> mCallback;
    std::map<int32_t, std::shared_ptr<Sensor>> mSensors;
    int32_t mNextHandle = 1;
    std::mutex mWriteLock;
};

}  // namespace sensors
}  // namespace hardware
}  // namespace android
}  // namespace aidl
