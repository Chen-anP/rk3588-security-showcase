#include "record/pre_record.hpp"

#include <utility>

namespace rksec::record {

PreRecordBuffer::PreRecordBuffer(std::int64_t keep_us, std::size_t max_packets)
    : keep_us_(keep_us), max_packets_(max_packets)
{
}

void PreRecordBuffer::pop_front()
{
    bytes_ -= packets_.front()->data.size();
    packets_.pop_front();
}

void PreRecordBuffer::push(PacketPtr p)
{
    if (!p) {
        return;
    }
    if (packets_.empty() && !p->keyframe) {
        return;  // 缓冲区只从关键帧开始
    }
    bytes_ += p->data.size();
    packets_.push_back(std::move(p));

    // 找最后一个“时间早于 newest - keep”的关键帧，它前面的都可以扔掉
    const std::int64_t cut = packets_.back()->pts_us - keep_us_;
    std::size_t start = 0;
    for (std::size_t i = 1; i < packets_.size(); ++i) {
        if (packets_[i]->pts_us > cut) {
            break;
        }
        if (packets_[i]->keyframe) {
            start = i;
        }
    }
    for (std::size_t i = 0; i < start; ++i) {
        pop_front();
    }

    // 保护上限：扔到下一个关键帧为止（没有下一个关键帧就全清空，等新的关键帧）
    if (packets_.size() > max_packets_) {
        pop_front();
        while (!packets_.empty() && !packets_.front()->keyframe) {
            pop_front();
        }
    }
}

std::vector<PacketPtr> PreRecordBuffer::snapshot() const
{
    return {packets_.begin(), packets_.end()};
}

void PreRecordBuffer::clear()
{
    packets_.clear();
    bytes_ = 0;
}

std::int64_t PreRecordBuffer::span_us() const
{
    return packets_.empty() ? 0 : packets_.back()->pts_us - packets_.front()->pts_us;
}

}  // namespace rksec::record
