#pragma once
#include <android-base/macros.h>
#include <android/log.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

#ifndef LOG_TAG
#define LOG_TAG "gki-hal"
#endif

namespace android::base {

enum LogSeverity { VERBOSE, DEBUG, INFO, WARNING, ERROR, FATAL_WITHOUT_ABORT, FATAL };

inline int androidPriority(LogSeverity s) {
    switch (s) {
        case VERBOSE:
            return ANDROID_LOG_VERBOSE;
        case DEBUG:
            return ANDROID_LOG_DEBUG;
        case INFO:
            return ANDROID_LOG_INFO;
        case WARNING:
            return ANDROID_LOG_WARN;
        case ERROR:
            return ANDROID_LOG_ERROR;
        default:
            return ANDROID_LOG_FATAL;
    }
}

struct LogMessage {
    LogSeverity severity;
    const char* file;
    int line;
    std::ostringstream stream;
    template <typename T>
    LogMessage& operator<<(const T& value) {
        if constexpr (requires { value.toString(); }) {
            stream << value.toString();
        } else if constexpr (requires { toString(value); }) {
            stream << toString(value);
        } else if constexpr (requires(std::ostream& o) { o << value; }) {
            stream << value;
        } else if constexpr (requires { std::begin(value); std::end(value); }) {
            stream << '[';
            bool first = true;
            for (const auto& el : value) {
                if (!first) stream << ", ";
                first = false;
                *this << el;
            }
            stream << ']';
        }
        return *this;
    }
    ~LogMessage() {
        __android_log_print(androidPriority(severity), LOG_TAG, "%s:%d %s", file, line,
                            stream.str().c_str());
        if (severity >= FATAL) abort();
    }
};

enum LogId {
    DEFAULT,
    MAIN,
    RADIO,
    SYSTEM,
    CRASH,
    KERNEL,
    SECURITY,
};

struct LogdLogger {
    explicit LogdLogger(LogId = MAIN) {}
};

inline void InitLogging(char** /*argv*/, LogdLogger = LogdLogger()) {}
inline void SetDefaultTag(const std::string&) {}
inline void SetMinimumLogSeverity(LogSeverity) {}

}  // namespace android::base

// Accepts both LOG(ERROR) and LOG(severity) where severity is a LogSeverity variable.
#define LOG(level)                                                                             \
    ::android::base::LogMessage {                                                              \
        [&]() -> ::android::base::LogSeverity {                                                \
            using ::android::base::VERBOSE;                                                    \
            using ::android::base::DEBUG;                                                      \
            using ::android::base::INFO;                                                       \
            using ::android::base::WARNING;                                                    \
            using ::android::base::ERROR;                                                      \
            using ::android::base::FATAL_WITHOUT_ABORT;                                        \
            using ::android::base::FATAL;                                                      \
            return static_cast<::android::base::LogSeverity>(level);                           \
        }(),                                                                                   \
                __FILE__, __LINE__                                                             \
    }
#define PLOG(level) LOG(level) << ::strerror(errno) << ": "
#define LOG_IF(level, cond) \
    if (cond) LOG(level)

#define CHECK(condition)                                                                      \
    if (!(condition))                                                                         \
    ::android::base::LogMessage{::android::base::FATAL, __FILE__, __LINE__}                   \
            << "CHECK failed: " #condition " "

#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_NE(a, b) CHECK((a) != (b))
#define CHECK_GE(a, b) CHECK((a) >= (b))
#define CHECK_GT(a, b) CHECK((a) > (b))
#define CHECK_LE(a, b) CHECK((a) <= (b))
#define CHECK_LT(a, b) CHECK((a) < (b))

#define UNIMPLEMENTED(level) LOG(level) << "unimplemented "
