#pragma once

#include <cstddef>
#include <cstdint>

// 采集模块用到的系统调用，集中在一个接口后面。
//
// 为什么：V4L2 代码离不开 open / ioctl / mmap / poll，直接调用的话只能在接了摄像头的板子上测试，
// 而且“驱动改了分辨率”“拔线”“某一帧出错”这些情况很难在真机上按需制造。
// 把它们放在接口后面：真机用 real_sys()，单元测试用一个假设备（tests/fake_v4l2.hpp），想制造什么情况都行。
//
// 约定：出错时返回 -errno（负数），不用全局 errno——测试里好构造，多线程下也不会被别的调用覆盖。
namespace rksec::capture {

class Sys {
  public:
    virtual ~Sys() = default;

    virtual int open(const char *path, int flags) = 0;  // 成功返回 fd
    virtual int close(int fd) = 0;
    virtual int ioctl(int fd, unsigned long request, void *arg) = 0;  // 成功返回 0（个别 ioctl 返回正数）
    // 成功返回映射地址；失败返回 nullptr，err 里是 -errno
    virtual void *mmap(std::size_t length, int fd, std::int64_t offset, int &err) = 0;
    virtual int munmap(void *addr, std::size_t length) = 0;
    // 等 fd 可读：1 可读，0 超时，-errno 出错；设备出错（POLLERR，例如拔线）返回 -ENODEV
    virtual int poll_readable(int fd, int timeout_ms) = 0;
};

// 真正的系统调用
Sys &real_sys();

}  // namespace rksec::capture
