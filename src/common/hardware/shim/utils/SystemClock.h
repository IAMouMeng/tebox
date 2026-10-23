#pragma once
#include <utils/Timers.h>

namespace android {
#ifndef GKI_UPTIME_NANOS_DEFINED
#define GKI_UPTIME_NANOS_DEFINED
inline int64_t uptimeNanos() { return systemTime(SYSTEM_TIME_MONOTONIC); }
#endif
inline int64_t uptimeMillis() { return uptimeNanos() / 1000000LL; }
}  // namespace android
