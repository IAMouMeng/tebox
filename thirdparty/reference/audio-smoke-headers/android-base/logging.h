#pragma once

#include <android/log.h>
#include <cstdlib>
#include <sstream>
#include <string>

namespace android::base::detail {
struct LogMessage {
    int priority;
    const char* file;
    int line;
    std::ostringstream stream;
    template <typename T> LogMessage& operator<<(const T& value) { stream << value; return *this; }
    ~LogMessage() { __android_log_print(priority, "FMQ", "%s", stream.str().c_str()); }
};
}

#define LOG(level) ::android::base::detail::LogMessage{ANDROID_LOG_ERROR, __FILE__, __LINE__}
#define CHECK(condition) if (!(condition)) ::android::base::detail::LogMessage{ANDROID_LOG_FATAL, __FILE__, __LINE__} << "CHECK failed: " #condition
