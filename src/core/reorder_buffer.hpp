#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

// 按序号重新排队（第 6 步）：几个 NPU 核同时推理，第 5 帧可能比第 4 帧先算完；
// 下游（显示框、以后的跟踪）要按帧的先后顺序处理，不能倒着来。
//
//   ReorderBuffer<Result> rb;
//   for (auto &r : rb.push(job_id, result)) { ...按 0、1、2、3… 的顺序拿到... }
//
// - 序号从 first 开始连续编号，每个序号必须正好交一次（失败的也要交一个“失败”结果，否则后面的全卡住）
// - 先到的后面的序号暂存，等前面的到了一起放出来
// - 已经放出去的序号又来一次（重复、迟到）：丢掉并计数，不会让顺序倒退
// - 不是线程安全的，调用方加锁
namespace rksec {

template <typename T> class ReorderBuffer {
  public:
    struct Stats {
        std::uint64_t released = 0;   // 按顺序放出去的个数
        std::uint64_t held = 0;       // 到早了、暂存过的个数（说明发生过乱序）
        std::uint64_t ignored = 0;    // 重复或迟到、被丢掉的个数（正常不应该有）
        std::size_t max_pending = 0;  // 同时暂存的最多个数
    };

    explicit ReorderBuffer(std::uint64_t first = 0) : next_(first) {}

    // 交一个结果；返回因此可以按顺序放出去的结果（可能是 0 个、1 个或好几个）
    std::vector<T> push(std::uint64_t seq, T value)
    {
        std::vector<T> out;
        if (seq < next_ || pending_.count(seq) != 0) {
            ++stats_.ignored;
            return out;
        }
        if (seq != next_) {
            ++stats_.held;
            pending_.emplace(seq, std::move(value));
            stats_.max_pending = std::max(stats_.max_pending, pending_.size());
            return out;
        }
        out.push_back(std::move(value));
        ++next_;
        for (auto it = pending_.begin(); it != pending_.end() && it->first == next_; it = pending_.erase(it)) {
            out.push_back(std::move(it->second));
            ++next_;
        }
        stats_.released += out.size();
        return out;
    }

    std::uint64_t next() const { return next_; }  // 下一个要放出去的序号
    std::size_t pending() const { return pending_.size(); }
    Stats stats() const { return stats_; }

  private:
    std::uint64_t next_;
    std::map<std::uint64_t, T> pending_;
    Stats stats_;
};

}  // namespace rksec
