#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

// 帧句柄：采集模块的每个缓冲区，交给后面的模块用完以后，必须自动还回去。
//
// 第 3 步 V4L2 采集的流程是：DQBUF 拿到第 i 个缓冲区 → 交给检测、显示、编码 → 都用完后 QBUF 还给驱动。
// 忘了还，驱动的缓冲区越来越少，最后采集卡住；还早了，驱动往正在被检测读的内存里写下一帧，画面撕裂。
//
// 这里用引用计数解决：FramePool::acquire(i) 返回 FrameRef（std::shared_ptr），
// 复制一份就多一个使用者，最后一份销毁时自动调用 on_release(i)。谁都不需要手动“还”。
//
// - on_release 在锁外调用，可能在任意线程（谁最后放手就在谁的线程）
// - 同一个缓冲区还没还就又 acquire，说明调用方有 bug，返回空句柄
// - FramePool 先销毁、帧还在别处被引用也安全：之后的释放不再调用 on_release（驱动那边已经停流）；
//   析构函数会等正在执行的 on_release 返回，所以析构之后 on_release 一定不会再被调用
// - 不要在 on_release 里销毁 FramePool（会自己等自己）
namespace rksec {

struct Frame {
    int index = -1;                                  // 第几个缓冲区，0 ~ count-1
    std::uint64_t sequence = 0;                      // 采集序号，用来发现丢帧
    std::chrono::steady_clock::time_point captured;  // 采集时间（驱动时间戳），用来算端到端延迟
    const std::uint8_t *data = nullptr;  // 帧数据（第 3 步：MJPG），属于采集模块的 mmap 区域
    std::size_t bytes = 0;               // 有效字节数
    int dmabuf_fd = -1;  // 同一块内存导出的 DMA-BUF（第 3c 步）；交给硬件（MPP 解码、RGA）时不用拷贝。-1 表示没有
};

using FrameRef = std::shared_ptr<const Frame>;

class FramePool {
  public:
    using ReleaseFn = std::function<void(int index)>;

    struct Stats {
        std::uint64_t acquired = 0;
        std::uint64_t released = 0;
        std::uint64_t rejected = 0;  // acquire 失败的次数（下标越界或重复）
    };

    FramePool(int count, ReleaseFn on_release);
    ~FramePool();

    FramePool(const FramePool &) = delete;
    FramePool &operator=(const FramePool &) = delete;

    // 第 index 个缓冲区装好了一帧，交出句柄。失败返回空句柄。
    FrameRef acquire(int index, std::uint64_t sequence, std::chrono::steady_clock::time_point captured,
                     const std::uint8_t *data = nullptr, std::size_t bytes = 0, int dmabuf_fd = -1);

    int count() const;
    int in_use() const;  // 正在被使用（还没还）的缓冲区个数
    Stats stats() const;

  private:
    struct State;
    struct Releaser;
    std::shared_ptr<State> state_;  // 每个 FrameRef 也持有一份，所以池子先销毁也安全
};

}  // namespace rksec
