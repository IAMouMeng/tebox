#pragma once

#include <aidl/android/hardware/media/c2/Status.h>
#include <android/binder_auto_utils.h>

namespace gki {

inline ::ndk::ScopedAStatus c2Omitted() {
    return ::ndk::ScopedAStatus::fromServiceSpecificError(
            aidl::android::hardware::media::c2::Status::OMITTED);
}

inline ::ndk::ScopedAStatus c2BadState() {
    return ::ndk::ScopedAStatus::fromServiceSpecificError(
            aidl::android::hardware::media::c2::Status::BAD_STATE);
}

inline ::ndk::ScopedAStatus c2NotSupported() {
    return c2Omitted();
}

}  // namespace gki
