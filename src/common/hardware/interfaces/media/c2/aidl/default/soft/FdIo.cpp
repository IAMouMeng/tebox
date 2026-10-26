#include "FdIo.h"

#include <errno.h>
#include <poll.h>
#include <unistd.h>

#include <vector>

namespace gki {

bool writeFull(int fd, const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (size) {
        ssize_t w = write(fd, p, size);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        size -= static_cast<size_t>(w);
    }
    return true;
}

bool readFull(int fd, void* data, size_t size) {
    uint8_t* p = static_cast<uint8_t*>(data);
    while (size) {
        ssize_t r = read(fd, p, size);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r;
        size -= static_cast<size_t>(r);
    }
    return true;
}

std::vector<uint8_t> readFd(int fd, size_t maxBytes) {
    std::vector<uint8_t> out;
    if (fd < 0) return out;
    off_t len = lseek(fd, 0, SEEK_END);
    if (len <= 0) return out;
    if (static_cast<size_t>(len) > maxBytes) len = static_cast<off_t>(maxBytes);
    out.resize(static_cast<size_t>(len));
    lseek(fd, 0, SEEK_SET);
    size_t got = 0;
    while (got < out.size()) {
        ssize_t n = pread(fd, out.data() + got, out.size() - got, static_cast<off_t>(got));
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    out.resize(got);
    return out;
}

bool waitFd(int fd, int timeoutMs) {
    if (fd < 0) return true;
    pollfd p = {fd, POLLIN, 0};
    int r;
    do {
        r = poll(&p, 1, timeoutMs);
    } while (r < 0 && errno == EINTR);
    return r > 0;
}

}  // namespace gki

