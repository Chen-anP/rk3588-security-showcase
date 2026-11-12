#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// 阶段计时：记录每一帧在每个阶段（采集、预处理、推理、后处理、显示……）花的时间，
// 统计 P50 / P99 / 最大值。性能优化（第 5、6 步）全靠它出数据。
//
//   StageStats infer{"infer"};
//   { ScopedTimer t{infer}; rknn_run(...); }   // 出作用域时记一次
//   LOG_I("perf", "%s", infer.report().c_str()); // "infer n=300 p50=21.3ms p99=27.9ms max=31.0ms"
//
// - 只保留最近 window 个样本（默认 1024），反映“现在”的性能，内存固定
// - 为什么看 P99 不看平均：平均值会被大量正常帧掩盖偶发的卡顿。
//   P99 是排好序后第 99% 位置上的那个值（100 个样本时是第二慢的），偶发卡顿多了会把它拉高
// - 线程安全：一个线程记录、另一个线程定时打印没有问题
namespace rksec {

struct StageSummary {
    std::size_t count = 0;    // 窗口里的样本数
    std::uint64_t total = 0;  // 一共记录过多少次（含已经滚出窗口的）
    double p50_ms = 0;
    double p99_ms = 0;
    double max_ms = 0;
};

class StageStats {
  public:
    explicit StageStats(std::string name, std::size_t window = 1024);

    void record(std::chrono::nanoseconds d);
    StageSummary summary() const;
    std::string report() const;  // 一行文字，打日志用
    void reset();

    const std::string &name() const { return name_; }

  private:
    const std::string name_;
    const std::size_t window_;  // 窗口大小。不用 samples_ns_.capacity()：reserve 只保证“至少”这么大（P5）
    mutable std::mutex mutex_;
    std::vector<std::int64_t> samples_ns_;  // 环形缓冲区，最多 window_ 个
    std::size_t next_ = 0;
    std::uint64_t total_ = 0;
};

// 构造时开始计时，析构时记一次
class ScopedTimer {
  public:
    explicit ScopedTimer(StageStats &s) : stats_(s), start_(std::chrono::steady_clock::now()) {}
    ~ScopedTimer() { stats_.record(std::chrono::steady_clock::now() - start_); }

    ScopedTimer(const ScopedTimer &) = delete;
    ScopedTimer &operator=(const ScopedTimer &) = delete;

  private:
    StageStats &stats_;
    std::chrono::steady_clock::time_point start_;
};

// 第 p 百分位（0 < p ≤ 100），最近秩法：sorted 已从小到大排好，取第 ceil(p/100 × n) 个。样本为空返回 0。
// 单独拿出来是为了能直接测试。
std::int64_t percentile(const std::vector<std::int64_t> &sorted, double p);

}  // namespace rksec
