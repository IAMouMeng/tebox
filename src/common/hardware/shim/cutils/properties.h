#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef PROPERTY_VALUE_MAX
#define PROPERTY_VALUE_MAX 92
#endif

#ifdef __cplusplus
extern "C" {
#endif

int property_get(const char* key, char* value, const char* default_value);
int property_set(const char* key, const char* value);
int32_t property_get_int32(const char* key, int32_t default_value);

#ifdef __cplusplus
}
#endif

#if defined(GKI_SHIM_PROPERTIES_IMPL)
#include <stdlib.h>
int property_get(const char* key, char* value, const char* default_value) {
    const char* v = getenv(key);
    if (!v || !v[0]) v = default_value;
    if (!v) v = "";
    size_t n = strlen(v);
    if (n >= PROPERTY_VALUE_MAX) n = PROPERTY_VALUE_MAX - 1;
    memcpy(value, v, n);
    value[n] = 0;
    return static_cast<int>(n);
}
int property_set(const char* key, const char* value) { return setenv(key, value ? value : "", 1); }
#endif
