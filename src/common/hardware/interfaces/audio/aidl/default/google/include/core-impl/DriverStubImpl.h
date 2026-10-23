/*
 * Copyright (C) 2024 The Android Open Source Project
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
 */

#pragma once

#include <cstdint>
#include <vector>

#include "core-impl/Stream.h"

namespace aidl::android::hardware::audio::core {

class DriverStubImpl : virtual public DriverInterface {
  public:
    explicit DriverStubImpl(const StreamContext& context)
        : DriverStubImpl(context, 500 /*asyncSleepTimeUs*/, false) {}
    DriverStubImpl(const StreamContext& context, int asyncSleepTimeUs)
        : DriverStubImpl(context, asyncSleepTimeUs, false) {}
    DriverStubImpl(const StreamContext& context, int asyncSleepTimeUs, bool isSubmix);

    ::android::status_t init(DriverCallbackInterface* callback) override;
    ::android::status_t drain(StreamDescriptor::DrainMode) override;
    ::android::status_t flush() override;
    ::android::status_t pause() override;
    ::android::status_t standby() override;
    ::android::status_t start() override;
    ::android::status_t transfer(void* buffer, size_t frameCount, size_t* actualFrameCount,
                                 int32_t* latencyMs) override;
    void shutdown() override;

  protected:
    const size_t mBufferSizeFrames;
    const size_t mFrameSizeBytes;
    const size_t mChannelCount;
    const size_t mBytesPerSample;
    const bool mIsPcm;
    const int mSampleRate;
    const bool mIsAsynchronous;
    const bool mIsInput;
    const int32_t mMixPortHandle;
    const int mAsyncSleepTimeUs;
    const bool mIsSubmix;
    bool mIsInitialized = false;  // Used for validating the state machine logic.
    bool mIsStandby = true;       // Used for validating the state machine logic.
    bool mSubmixOutputCounted = false;
    bool mSubmixInputCounted = false;
    int64_t mStartTimeNs = 0;
    long mFramesSinceStart = 0;
    std::vector<int16_t> mCastPcm;
};

}  // namespace aidl::android::hardware::audio::core
