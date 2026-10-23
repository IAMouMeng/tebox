#pragma once
// NDK stand-in for soong-generated fingerprint.sysprop.h (VirtualProps).
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace android::fingerprint::virt {

using OptBool = std::optional<bool>;
using OptInt32 = std::optional<std::int32_t>;
using OptInt64 = std::optional<std::int64_t>;
using OptString = std::optional<std::string>;
using OptIntVec = std::vector<std::optional<std::int32_t>>;

class FingerprintHalProperties {
    using Value = std::variant<OptBool, OptInt32, OptInt64, OptString, OptIntVec>;
    static std::mutex& mu() {
        static std::mutex m;
        return m;
    }
    static std::map<std::string, Value>& store() {
        static std::map<std::string, Value> s;
        return s;
    }
    template <typename T>
    static T get(const char* name) {
        std::lock_guard lock(mu());
        auto it = store().find(name);
        if (it == store().end()) return T{};
        if (auto* p = std::get_if<T>(&it->second)) return *p;
        return T{};
    }
    template <typename T>
    static bool set(const char* name, const T& v) {
        std::lock_guard lock(mu());
        store()[name] = v;
        return true;
    }

  public:
#define PROP_OPT(n, T)                    \
    static T n() { return get<T>(#n); }   \
    static bool n(const T& v) { return set<T>(#n, v); }
    PROP_OPT(type, OptString)
    PROP_OPT(enrollments, OptIntVec)
    PROP_OPT(enrollment_hit, OptInt32)
    PROP_OPT(next_enrollment, OptString)
    PROP_OPT(authenticator_id, OptInt64)
    PROP_OPT(challenge, OptInt64)
    PROP_OPT(sensor_id, OptInt32)
    PROP_OPT(sensor_location, OptString)
    PROP_OPT(sensor_strength, OptInt32)
    PROP_OPT(operation_authenticate_fails, OptBool)
    PROP_OPT(operation_authenticate_latency, OptIntVec)
    PROP_OPT(operation_authenticate_duration, OptInt32)
    PROP_OPT(operation_authenticate_error, OptInt32)
    PROP_OPT(operation_authenticate_acquired, OptString)
    PROP_OPT(operation_enroll_error, OptInt32)
    PROP_OPT(operation_enroll_latency, OptIntVec)
    PROP_OPT(operation_detect_interaction_error, OptInt32)
    PROP_OPT(operation_detect_interaction_latency, OptIntVec)
    PROP_OPT(operation_detect_interaction_duration, OptInt32)
    PROP_OPT(operation_detect_interaction_acquired, OptString)
    PROP_OPT(max_enrollments, OptInt32)
    PROP_OPT(navigation_gesture, OptBool)
    PROP_OPT(detect_interaction, OptBool)
    PROP_OPT(display_touch, OptBool)
    PROP_OPT(control_illumination, OptBool)
    PROP_OPT(lockout, OptBool)
    PROP_OPT(lockout_enable, OptBool)
    PROP_OPT(lockout_timed_threshold, OptInt32)
    PROP_OPT(lockout_timed_duration, OptInt32)
    PROP_OPT(lockout_permanent_threshold, OptInt32)
#undef PROP_OPT
};

}  // namespace android::fingerprint::virt
