#pragma once
#include <mutex>
#ifndef GUARDED_BY
#define GUARDED_BY(x)
#endif
#ifndef EXCLUSIVE_LOCKS_REQUIRED
#define EXCLUSIVE_LOCKS_REQUIRED(...)
#endif
namespace android {
using Mutex = std::mutex;
}
