#pragma once

#include <cstdint>

// 事件状态机（第 8b 步）：什么时候开始录、什么时候停、什么时候切段。纯逻辑，CI 上测。
//
//   空闲 ──看到陌生人──▶ 录像中 ──post_us 内没再看到陌生人──▶ 空闲（停）
//                         │ 这一段录满 max_us
//                         └──▶ 切段（结束这个文件，从下一个关键帧开始新文件，事件继续）
//
// - “看到陌生人”：检测结果里有身份是陌生人的跟踪编号（IdentityBook 已经要求两次一致才算）
// - 收尾时间从“最后一次看到”算起：陌生人走了 post_us 才停，中间短暂被挡住不会切成两个事件
// - 没有新的检测结果时（检测卡住）也要定时调用 update(false, now)，否则永远不会停
namespace rksec::record {

struct EventOptions {
    std::int64_t post_us = 10'000'000;  // 陌生人消失多久后结束
    std::int64_t max_us = 300'000'000;  // 一个文件最长多久，超过切段
};

enum class EventAction : std::uint8_t {
    none,
    start,  // 开始一个事件（录像从预录缓冲开始）
    stop,   // 结束事件
    split,  // 当前文件录满了：结束这个文件，从下一个关键帧开始新文件
};

class EventTracker {
  public:
    explicit EventTracker(EventOptions opts) : opts_(opts) {}

    // stranger：这一次的检测结果里有没有陌生人；now_us：现在（和包的 pts 同一个时钟）
    EventAction update(bool stranger, std::int64_t now_us);

    bool active() const { return active_; }
    std::int64_t event_start_us() const { return event_start_; }

  private:
    EventOptions opts_;
    bool active_ = false;
    std::int64_t event_start_ = 0;
    std::int64_t segment_start_ = 0;
    std::int64_t last_seen_ = 0;
};

}  // namespace rksec::record
