#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "media/video_encoder.hpp"

// 预录缓冲（第 8b 步）：一直保存最近几秒编好的包，事件开始时把它们写在录像最前面，
// 这样录像里能看到“陌生人走进来”的过程，而不是从被认出来的那一刻才开始。
//
// 要点：缓冲区的第一个包永远是关键帧。从 P 帧开始的录像，开头一段解不出来（花屏）。
// 所以裁剪时不是“正好留 keep_us”，而是留“最近一个比 keep_us 更早的关键帧”之后的全部包：
// GOP 2 秒、keep 5 秒时，实际预录 5～7 秒。
//
// 包用 shared_ptr 共享：录像线程拿走一份快照时不拷贝码流。
// 不是线程安全的，调用方加锁（EventRecorder 里在编码线程 push、在主线程 snapshot）。
namespace rksec::record {

using PacketPtr = std::shared_ptr<const media::EncodedPacket>;

class PreRecordBuffer {
  public:
    // keep_us：至少保留多久；max_packets：保护上限（一直没有关键帧时不会无限增长）
    explicit PreRecordBuffer(std::int64_t keep_us, std::size_t max_packets = 3000);

    void push(PacketPtr p);
    // 当前缓冲的全部包（第一个是关键帧；还没收到过关键帧时为空）
    std::vector<PacketPtr> snapshot() const;
    void clear();

    std::size_t size() const { return packets_.size(); }
    std::size_t bytes() const { return bytes_; }
    // 第一个包到最后一个包的时间跨度
    std::int64_t span_us() const;

  private:
    void pop_front();

    std::int64_t keep_us_;
    std::size_t max_packets_;
    std::deque<PacketPtr> packets_;
    std::size_t bytes_ = 0;
};

}  // namespace rksec::record
