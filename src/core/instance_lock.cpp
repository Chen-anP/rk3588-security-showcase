#include "core/instance_lock.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>

namespace rksec {

InstanceLock::~InstanceLock()
{
    if (fd_ >= 0) {
        ::close(fd_);  // 关闭即释放锁。不删文件：删了再有人打开就是另一个文件，锁就不互斥了
    }
}

bool InstanceLock::acquire(const std::string &path, std::string &err)
{
    if (fd_ >= 0) {
        return true;
    }
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    bool writable = true;
    if (fd < 0 && errno == EACCES) {
        // 锁文件是别的用户建的（比如有人用 sudo 运行过），只读打开也能加 flock，只是写不了 pid
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        writable = false;
    }
    if (fd < 0) {
        err = "打不开锁文件 " + path + "：" + std::strerror(errno);
        return false;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int e = errno;
        std::array<char, 32> pid{};
        const ssize_t n = ::pread(fd, pid.data(), pid.size() - 1, 0);
        std::string who = n > 0 ? std::string(pid.data(), static_cast<std::size_t>(n)) : std::string{};
        while (!who.empty() && (who.back() == '\n' || who.back() == ' ')) {
            who.pop_back();
        }
        ::close(fd);
        if (e == EWOULDBLOCK) {
            err = "已经有一个实例在运行" + (who.empty() ? std::string{} : "（pid " + who + "）") +
                  "，同一时间只能运行一个（摄像头和屏幕不能共用）";
        } else {
            err = "锁文件 " + path + " 加锁失败：" + std::strerror(e);
        }
        return false;
    }
    // 拿到锁：写自己的 pid（只给人看，锁本身靠 flock，不靠文件内容）
    const std::string me = std::to_string(::getpid()) + "\n";
    if (writable && (::ftruncate(fd, 0) != 0 || ::pwrite(fd, me.data(), me.size(), 0) < 0)) {
        // 写不进 pid 不影响互斥，忽略
    }
    fd_ = fd;
    return true;
}

}  // namespace rksec
