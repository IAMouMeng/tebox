#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gki {

/* Blocking fd IO used by tebox.venc / tebox.vdec / tebox.aenc paths. */
bool writeFull(int fd, const void* data, size_t size);
bool readFull(int fd, void* data, size_t size);
std::vector<uint8_t> readFd(int fd, size_t maxBytes);
/* Poll |fd| for POLLIN up to |timeoutMs|. Negative fd is treated as ready. */
bool waitFd(int fd, int timeoutMs);

}  // namespace gki

