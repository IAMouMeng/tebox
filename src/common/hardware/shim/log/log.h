#pragma once
#include <android/log.h>
#include <cstdlib>
#ifndef LOG_TAG
#define LOG_TAG "gki-hal"
#endif
#ifndef ALOGV
#define ALOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)
#endif
#ifndef ALOGD
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#endif
#ifndef ALOGI
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#endif
#ifndef ALOGW
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#endif
#ifndef ALOGE
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#endif
#ifndef LOG_ALWAYS_FATAL
#define LOG_ALWAYS_FATAL(...)                    \
    do {                                         \
        __android_log_print(ANDROID_LOG_FATAL, LOG_TAG, __VA_ARGS__); \
        abort();                                 \
    } while (0)
#endif
#ifndef LOG_ALWAYS_FATAL_IF
#define LOG_ALWAYS_FATAL_IF(cond, ...) \
    do {                               \
        if (cond) LOG_ALWAYS_FATAL(__VA_ARGS__); \
    } while (0)
#endif
#ifndef LOG_PRI_VA
#define LOG_PRI_VA(priority, tag, fmt, args) \
    __android_log_vprint(priority, tag, fmt, args)
#endif
