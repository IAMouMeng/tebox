#pragma once
#include <cstdint>
namespace aidl::android::hardware::wifi {
enum class Akm : int64_t {
    NONE = 0,
    PASN = 1LL << 0,
    SAE = 1LL << 1,
    FT_EAP_SHA256 = 1LL << 2,
    FT_PSK_SHA256 = 1LL << 3,
    FT_EAP_SHA384 = 1LL << 4,
    FT_PSK_SHA384 = 1LL << 5,
    FILS_EAP_SHA256 = 1LL << 6,
    FILS_EAP_SHA384 = 1LL << 7,
};
inline Akm operator|(Akm a, Akm b) {
    return static_cast<Akm>(static_cast<int64_t>(a) | static_cast<int64_t>(b));
}
inline int64_t operator|(int64_t a, Akm b) { return a | static_cast<int64_t>(b); }
}  // namespace aidl::android::hardware::wifi
