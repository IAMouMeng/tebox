#pragma once

#include <utility>

// NDK stand-in for hardware/interfaces error/expected_utils.h.
// errorIsOk is provided for ScopedAStatus in Utils.h (ndk::) and for status_t below.

inline bool errorIsOk(int s) { return s == 0; }

#ifndef RETURN_IF_ERROR
#define RETURN_IF_ERROR(expr)              \
    do {                                   \
        auto _st = (expr);                 \
        if (!errorIsOk(_st)) return _st;   \
    } while (0)
#endif

#ifndef RETURN_STATUS_IF_ERROR
#define RETURN_STATUS_IF_ERROR(expr) RETURN_IF_ERROR(expr)
#endif

#ifndef VALUE_OR_RETURN
#define VALUE_OR_RETURN(expr)                  \
    ({                                         \
        auto _tmp = (expr);                    \
        if (!_tmp.ok()) return _tmp.error();   \
        std::move(_tmp.value());               \
    })
#endif

#ifndef VALUE_OR_RETURN_STATUS
#define VALUE_OR_RETURN_STATUS(expr) VALUE_OR_RETURN(expr)
#endif
