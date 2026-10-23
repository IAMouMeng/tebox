#pragma once
#include <time.h>
#include <cstdint>
namespace android {
inline int64_t elapsedRealtimeNano() {
    timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
inline int64_t uptimeMillis() { return elapsedRealtimeNano() / 1000000LL; }
}  // namespace android
