#include "capture/sys.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>

namespace rksec::capture {

namespace {

class RealSys final : public Sys {
  public:
    int open(const char *path, int flags) override
    {
        const int fd = ::open(path, flags | O_CLOEXEC);
        return fd >= 0 ? fd : -errno;
    }

    int close(int fd) override { return ::close(fd) == 0 ? 0 : -errno; }

    int ioctl(int fd, unsigned long request, void *arg) override
    {
        int r = 0;
        do {
            r = ::ioctl(fd, request, arg);
        } while (r < 0 && errno == EINTR);  // 被信号打断就重试
        return r >= 0 ? r : -errno;
    }

    void *mmap(std::size_t length, int fd, std::int64_t offset, int &err) override
    {
        void *p = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, static_cast<off_t>(offset));
        if (p == MAP_FAILED) {
            err = -errno;
            return nullptr;
        }
        err = 0;
        return p;
    }

    int munmap(void *addr, std::size_t length) override { return ::munmap(addr, length) == 0 ? 0 : -errno; }

    int poll_readable(int fd, int timeout_ms) override
    {
        pollfd p{fd, POLLIN, 0};
        int r = 0;
        do {
            r = ::poll(&p, 1, timeout_ms);
        } while (r < 0 && errno == EINTR);
        if (r < 0) {
            return -errno;
        }
        if (r == 0) {
            return 0;
        }
        if ((p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return -ENODEV;
        }
        return 1;
    }
};

}  // namespace

Sys &real_sys()
{
    static RealSys sys;
    return sys;
}

}  // namespace rksec::capture
