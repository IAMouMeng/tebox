#pragma once
#include <time.h>
#include <cstdint>

typedef int64_t nsecs_t;

enum {
    SYSTEM_TIME_REALTIME = 0,
    SYSTEM_TIME_MONOTONIC = 1,
    SYSTEM_TIME_PROCESS = 2,
    SYSTEM_TIME_THREAD = 3,
    SYSTEM_TIME_BOOTTIME = 4,
};

inline nsecs_t systemTime(int clock = SYSTEM_TIME_MONOTONIC) {
    clockid_t clk = CLOCK_MONOTONIC;
    if (clock == SYSTEM_TIME_REALTIME) clk = CLOCK_REALTIME;
    else if (clock == SYSTEM_TIME_BOOTTIME) clk = CLOCK_BOOTTIME;
    timespec ts{};
    clock_gettime(clk, &ts);
    return static_cast<nsecs_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

inline double nanoseconds_to_seconds(nsecs_t n) { return static_cast<double>(n) / 1e9; }

namespace android {
inline int64_t elapsedRealtimeNano() {
    timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
inline int64_t elapsedRealtime() { return elapsedRealtimeNano() / 1000000LL; }
}  // namespace android
