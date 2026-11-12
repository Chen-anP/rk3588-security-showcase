#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// 从内核的 dma-heap（/dev/dma_heap/system）分配的一块内存：有 fd（给 RGA、NPU、显示等硬件用），
// 也映射到了进程里（CPU 能读写）。第 5c 步 RGA 的输出放在这里。
//
// 缓存一致性：system heap 的内存 CPU 访问走缓存。硬件写完、CPU 去读之前要 begin_cpu_read()（让 CPU
// 缓存里的旧数据失效）， 读完 end_cpu_read()；CPU 写完、交给硬件之前同样要 begin_cpu_write() /
// end_cpu_write()（把缓存里的数据写回内存）。 不做的话，CPU 可能读到硬件写之前的旧数据，硬件可能读到 CPU
// 还没写回的数据——这种错时有时无，很难查。
//
// 5a 摸底：板子上只有 system / system-uncached 两个 heap（video 组可用），没有 cma、dma32。
namespace rksec::media {

class DmaBuffer {
  public:
    // 分配 size 字节；heap 是 /dev/dma_heap/ 下的名字。失败返回 nullptr，err 是原因
    static std::unique_ptr<DmaBuffer> allocate(std::size_t size, std::string &err, const char *heap = "system");

    ~DmaBuffer();
    DmaBuffer(const DmaBuffer &) = delete;
    DmaBuffer &operator=(const DmaBuffer &) = delete;

    int fd() const { return fd_; }
    std::uint8_t *data() const { return data_; }
    std::size_t size() const { return size_; }

    void begin_cpu_read() const;
    void end_cpu_read() const;
    void begin_cpu_write() const;
    void end_cpu_write() const;

  private:
    DmaBuffer(int fd, std::uint8_t *data, std::size_t size) : fd_(fd), data_(data), size_(size) {}
    void sync(std::uint64_t flags) const;

    int fd_ = -1;
    std::uint8_t *data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace rksec::media
