#pragma once

#include <aidl/android/hardware/common/NativeHandle.h>
#include <aidl/android/hardware/media/c2/BaseBlock.h>
#include <aidl/android/hardware/media/c2/Buffer.h>
#include <aidl/android/hardware/media/c2/FrameData.h>
#include <aidl/android/hardware/media/c2/Work.h>

namespace gki {

/* Deep-copy AIDL Codec2 handles/blocks so workers outlive the binder call. */
aidl::android::hardware::common::NativeHandle duplicateHandle(
        const aidl::android::hardware::common::NativeHandle& src);
aidl::android::hardware::media::c2::BaseBlock duplicateBaseBlock(
        const aidl::android::hardware::media::c2::BaseBlock& src);
aidl::android::hardware::media::c2::Buffer duplicateBuffer(
        const aidl::android::hardware::media::c2::Buffer& src);
aidl::android::hardware::media::c2::FrameData duplicateFrameData(
        const aidl::android::hardware::media::c2::FrameData& src);
aidl::android::hardware::media::c2::Work duplicateWork(
        const aidl::android::hardware::media::c2::Work& src);

}  // namespace gki
