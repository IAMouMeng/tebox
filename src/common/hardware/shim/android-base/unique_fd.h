#pragma once
#include <unistd.h>

namespace android::base {
class unique_fd {
  public:
    explicit unique_fd(int fd = -1) : fd_(fd) {}
    unique_fd(unique_fd&& other) noexcept : fd_(other.release()) {}
    unique_fd& operator=(unique_fd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;
    ~unique_fd() { reset(); }
    int get() const { return fd_; }
    bool operator!=(int fd) const { return fd_ != fd; }
    bool operator==(int fd) const { return fd_ == fd; }
    operator int() const { return fd_; }
    int release() {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset(int fd = -1) {
        if (fd_ >= 0) close(fd_);
        fd_ = fd;
    }
    explicit operator bool() const { return fd_ >= 0; }

  private:
    int fd_;
};
}  // namespace android::base
