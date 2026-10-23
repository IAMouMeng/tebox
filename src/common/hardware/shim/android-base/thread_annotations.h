#pragma once
#include <android-base/macros.h>

#ifndef GUARDED_BY
#define GUARDED_BY(x)
#endif
#ifndef PT_GUARDED_BY
#define PT_GUARDED_BY(x)
#endif
#ifndef EXCLUSIVE_LOCKS_REQUIRED
#define EXCLUSIVE_LOCKS_REQUIRED(...)
#endif
#ifndef SHARED_LOCKS_REQUIRED
#define SHARED_LOCKS_REQUIRED(...)
#endif
#ifndef ACQUIRED_AFTER
#define ACQUIRED_AFTER(...)
#endif
#ifndef ACQUIRED_BEFORE
#define ACQUIRED_BEFORE(...)
#endif
#ifndef EXCLUSIVE_LOCK_FUNCTION
#define EXCLUSIVE_LOCK_FUNCTION(...)
#endif
#ifndef SHARED_LOCK_FUNCTION
#define SHARED_LOCK_FUNCTION(...)
#endif
#ifndef UNLOCK_FUNCTION
#define UNLOCK_FUNCTION(...)
#endif
#ifndef ASSERT_EXCLUSIVE_LOCK
#define ASSERT_EXCLUSIVE_LOCK(...)
#endif
#ifndef ASSERT_SHARED_LOCK
#define ASSERT_SHARED_LOCK(...)
#endif
#ifndef NO_THREAD_SAFETY_ANALYSIS
#define NO_THREAD_SAFETY_ANALYSIS
#endif
#ifndef RETURN_CAPABILITY
#define RETURN_CAPABILITY(x)
#endif
#ifndef SCOPED_CAPABILITY
#define SCOPED_CAPABILITY
#endif
#ifndef CAPABILITY
#define CAPABILITY(x)
#endif
#ifndef REQUIRES
#define REQUIRES(...)
#endif
#ifndef ACQUIRE
#define ACQUIRE(...)
#endif
#ifndef RELEASE
#define RELEASE(...)
#endif

namespace android::base {
class ScopedLockAssertion {
  public:
    template <typename T>
    explicit ScopedLockAssertion(T&) {}
};
}  // namespace android::base
