#pragma once
#include <string>
#include <cutils/properties.h>

namespace android::base {

inline std::string GetProperty(const std::string& key, const std::string& def) {
    char value[PROPERTY_VALUE_MAX] = {};
    int n = property_get(key.c_str(), value, def.c_str());
    if (n <= 0) return def;
    return std::string(value, static_cast<size_t>(n));
}

inline bool SetProperty(const std::string& key, const std::string& value) {
    return property_set(key.c_str(), value.c_str()) == 0;
}

inline bool GetBoolProperty(const std::string& key, bool def) {
    std::string v = GetProperty(key, def ? "1" : "0");
    return v == "1" || v == "true";
}

}  // namespace android::base
