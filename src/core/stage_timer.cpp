#include "core/stage_timer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace rksec {

namespace {

double to_ms(std::int64_t ns)
{
    return static_cast<double>(ns) / 1e6;
}

}  // namespace

std::int64_t percentile(const std::vector<std::int64_t> &sorted, double p)
{
    if (sorted.empty()) {
        return 0;
    }
    const auto n = static_cast<double>(sorted.size());
    // 先乘后除：p / 100 * n 在 p=7、n=100 时会算成 7.000000000000001，向上取整变成 8（错）
    auto rank = static_cast<std::size_t>(std::ceil(p * n / 100.0));  // 第几个，从 1 开始
    rank = std::clamp<std::size_t>(rank, 1, sorted.size());
    return sorted[rank - 1];
}

StageStats::StageStats(std::string name, std::size_t window) : name_(std::move(name)), window_(window)
{
    if (window == 0) {
        throw std::invalid_argument("StageStats 窗口不能为 0");
    }
    samples_ns_.reserve(window);
}

void StageStats::record(std::chrono::nanoseconds d)
{
    const std::int64_t ns = d.count();
    const std::lock_guard<std::mutex> lock(mutex_);
    if (samples_ns_.size() < window_) {
        samples_ns_.push_back(ns);
    } else {
        samples_ns_[next_] = ns;
        next_ = (next_ + 1) % samples_ns_.size();
    }
    ++total_;
}

StageSummary StageStats::summary() const
{
    std::vector<std::int64_t> copy;
    StageSummary s;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        copy = samples_ns_;
        s.total = total_;
    }
    // 排序在锁外做，不挡住记录的线程
    std::sort(copy.begin(), copy.end());
    s.count = copy.size();
    if (!copy.empty()) {
        s.p50_ms = to_ms(percentile(copy, 50));
        s.p99_ms = to_ms(percentile(copy, 99));
        s.max_ms = to_ms(copy.back());
    }
    return s;
}

std::string StageStats::report() const
{
    const auto s = summary();
    std::array<char, 160> buf{};
    std::snprintf(buf.data(), buf.size(), "%s n=%zu p50=%.1fms p99=%.1fms max=%.1fms", name_.c_str(), s.count, s.p50_ms,
                  s.p99_ms, s.max_ms);
    return buf.data();
}

void StageStats::reset()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    samples_ns_.clear();  // clear 不释放容量，窗口大小不变
    next_ = 0;
    total_ = 0;
}

}  // namespace rksec
