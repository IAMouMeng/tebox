#include "VirtioSerial.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace gki {

int openVirtioPort(const char* portName) {
    if (!portName || !*portName) return -1;
    DIR* dir = opendir("/sys/class/virtio-ports");
    if (!dir) return -1;
    int fd = -1;
    while (dirent* e = readdir(dir)) {
        if (e->d_name[0] == '.') continue;
        char path[256], name[64] = "";
        snprintf(path, sizeof(path), "/sys/class/virtio-ports/%s/name", e->d_name);
        FILE* f = fopen(path, "re");
        if (!f) continue;
        if (fgets(name, sizeof(name), f)) name[strcspn(name, "\n")] = 0;
        fclose(f);
        if (strcmp(name, portName) != 0) continue;
        snprintf(path, sizeof(path), "/dev/%s", e->d_name);
        fd = open(path, O_RDWR | O_CLOEXEC);
        break;
    }
    closedir(dir);
    return fd;
}

}  // namespace gki
