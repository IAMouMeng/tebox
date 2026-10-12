#pragma once

#include <android/log.h>

#ifdef __cplusplus
extern "C" {
#endif
int android_errorWriteLog(int tag, const char* subTag);
#ifdef __cplusplus
}
#endif

#ifndef ALOGE
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, "FMQ", __VA_ARGS__)
#endif
#ifndef ALOGW
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, "FMQ", __VA_ARGS__)
#endif
#ifndef ALOGI
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, "FMQ", __VA_ARGS__)
#endif
