#pragma once
#include <android-base/thread_annotations.h>
#include <mutex>
namespace android::audio_utils {
using mutex = std::mutex;
using lock_guard = std::lock_guard<std::mutex>;
}  // namespace android::audio_utils
