#pragma once
#include <cstdarg>
#include <cstdio>
#include <string>

namespace android::base {

inline std::string StringPrintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) vsnprintf(out.data(), out.size() + 1, fmt, ap2);
    va_end(ap2);
    return out;
}

}  // namespace android::base
