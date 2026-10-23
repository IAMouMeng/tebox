#define GKI_SHIM_PROPERTIES_IMPL 1
#include "properties.h"

int32_t property_get_int32(const char* key, int32_t default_value) {
    char buf[PROPERTY_VALUE_MAX];
    if (property_get(key, buf, "") <= 0) return default_value;
    char* end = nullptr;
    long v = strtol(buf, &end, 0);
    if (end == buf) return default_value;
    return static_cast<int32_t>(v);
}
