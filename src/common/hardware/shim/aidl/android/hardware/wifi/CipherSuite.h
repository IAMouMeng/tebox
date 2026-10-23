#pragma once
#include <cstdint>
namespace aidl::android::hardware::wifi {
enum class CipherSuite : int64_t {
    NONE = 0,
    CCMP_128 = 1LL << 0,
    GCMP_256 = 1LL << 1,
    GCMP_128 = 1LL << 2,
    CCMP_256 = 1LL << 3,
    SMS4 = 1LL << 4,
    GCMP_256_TK = 1LL << 5,
};
}  // namespace aidl::android::hardware::wifi
