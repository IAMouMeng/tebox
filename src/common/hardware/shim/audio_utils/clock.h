#pragma once
#include <cstdint>
#include <time.h>

#ifndef NANOS_PER_SECOND
#define NANOS_PER_SECOND 1000000000LL
#endif
#ifndef NANOS_PER_MICROSECOND
#define NANOS_PER_MICROSECOND 1000LL
#endif
#ifndef MICROS_PER_SECOND
#define MICROS_PER_SECOND 1000000LL
#endif
#ifndef MILLIS_PER_SECOND
#define MILLIS_PER_SECOND 1000LL
#endif

namespace android {
#ifndef GKI_UPTIME_NANOS_DEFINED
#define GKI_UPTIME_NANOS_DEFINED
inline int64_t uptimeNanos() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * NANOS_PER_SECOND + ts.tv_nsec;
}
#endif
}  // namespace android
