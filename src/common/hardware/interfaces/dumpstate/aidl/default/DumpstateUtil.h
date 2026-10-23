// NDK stand-in for dumpstateutil DumpFileToFd used by AOSP dumpstate/aidl/default.
#pragma once
#include <cstdio>

namespace android::os::dumpstate {

inline void DumpFileToFd(int fd, const char* title, const char* path) {
    dprintf(fd, "------ %s (%s) ------\n", title, path);
    FILE* f = fopen(path, "r");
    if (!f) {
        dprintf(fd, "Unable to open %s\n", path);
        return;
    }
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) dprintf(fd, "%s", buf);
    fclose(f);
    dprintf(fd, "\n");
}

}  // namespace android::os::dumpstate
