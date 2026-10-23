#pragma once
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

namespace android::base {

inline bool ReadFileToString(const std::string& path, std::string* content, bool /*follow*/ = true) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[4096];
    content->clear();
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) content->append(buf, static_cast<size_t>(n));
    close(fd);
    return n >= 0;
}

inline bool WriteStringToFile(const std::string& content, const std::string& path) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    ssize_t n = write(fd, content.data(), content.size());
    close(fd);
    return n == static_cast<ssize_t>(content.size());
}

inline bool WriteStringToFd(const std::string& content, int fd) {
    if (fd < 0) return false;
    ssize_t n = write(fd, content.data(), content.size());
    return n == static_cast<ssize_t>(content.size());
}

}  // namespace android::base
