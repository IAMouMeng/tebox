#pragma once
#include <cstring>

template <int (*cmp)(const char*, const char*, size_t)>
constexpr int fasticmp(const char* a, const char* b, size_t n) {
    return cmp(a, b, n);
}
