#include "media/dma_buffer.hpp"

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace rksec::media {

std::unique_ptr<DmaBuffer> DmaBuffer::allocate(std::size_t size, std::string &err, const char *heap)
{
    const std::string path = std::string{"/dev/dma_heap/"} + heap;
    const int hfd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (hfd < 0) {
        err = "打不开 " + path + "：" + std::strerror(errno) + (errno == EACCES ? "（用户要在 video 组）" : "");
        return nullptr;
    }
    dma_heap_allocation_data a{};
    a.len = size;
    a.fd_flags = O_RDWR | O_CLOEXEC;
    const int r = ::ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &a);
    const int e = errno;
    ::close(hfd);  // 分配出来的 buffer 有自己的 fd，heap 的 fd 不用留着
    if (r != 0) {
        err = "dma-heap 分配 " + std::to_string(size) + " 字节失败：" + std::strerror(e);
        return nullptr;
    }
    const int fd = static_cast<int>(a.fd);
    void *p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        err = std::string{"mmap DMA-BUF 失败："} + std::strerror(errno);
        ::close(fd);
        return nullptr;
    }
    return std::unique_ptr<DmaBuffer>(new DmaBuffer(fd, static_cast<std::uint8_t *>(p), size));
}

DmaBuffer::~DmaBuffer()
{
    if (data_ != nullptr) {
        ::munmap(data_, size_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void DmaBuffer::sync(std::uint64_t flags) const
{
    dma_buf_sync s{};
    s.flags = flags;
    while (::ioctl(fd_, DMA_BUF_IOCTL_SYNC, &s) != 0 && (errno == EINTR || errno == EAGAIN)) {
    }
}

void DmaBuffer::begin_cpu_read() const
{
    sync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
}

void DmaBuffer::end_cpu_read() const
{
    sync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}

void DmaBuffer::begin_cpu_write() const
{
    sync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
}

void DmaBuffer::end_cpu_write() const
{
    sync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
}

}  // namespace rksec::media
