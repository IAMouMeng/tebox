#pragma once
#include <cstdlib>
#include <string>

namespace android::base {

template <typename T>
inline bool ParseInt(const std::string& s, T* out) {
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;
    *out = static_cast<T>(v);
    return true;
}

}  // namespace android::base
