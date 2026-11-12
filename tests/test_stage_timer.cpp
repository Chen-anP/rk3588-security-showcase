#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "core/stage_timer.hpp"

using rksec::percentile;
using rksec::ScopedTimer;
using rksec::StageStats;
using namespace std::chrono_literals;

TEST(StageTimer, PercentileNearestRank)
{
    std::vector<std::int64_t> v;
    for (int i = 1; i <= 100; ++i) {
        v.push_back(i);
    }
    EXPECT_EQ(percentile(v, 50), 50);
    EXPECT_EQ(percentile(v, 99), 99);
    EXPECT_EQ(percentile(v, 100), 100);
    EXPECT_EQ(percentile(v, 7), 7);  // p / 100 * n 的写法这里会得到 8
    EXPECT_EQ(percentile({42}, 99), 42);
    EXPECT_EQ(percentile({}, 50), 0);
}

TEST(StageTimer, EmptySummary)
{
    const StageStats s{"x"};
    const auto r = s.summary();
    EXPECT_EQ(r.count, 0U);
    EXPECT_EQ(r.p99_ms, 0.0);
}

TEST(StageTimer, SummaryInMilliseconds)
{
    StageStats s{"infer"};
    for (int i = 100; i >= 1; --i) {  // 倒着放，确认会排序
        s.record(std::chrono::milliseconds(i));
    }
    const auto r = s.summary();
    EXPECT_EQ(r.count, 100U);
    EXPECT_EQ(r.total, 100U);
    EXPECT_DOUBLE_EQ(r.p50_ms, 50.0);
    EXPECT_DOUBLE_EQ(r.p99_ms, 99.0);
    EXPECT_DOUBLE_EQ(r.max_ms, 100.0);
    EXPECT_EQ(s.report(), "infer n=100 p50=50.0ms p99=99.0ms max=100.0ms");
}

TEST(StageTimer, WindowKeepsOnlyRecentSamples)
{
    StageStats s{"x", 4};
    for (int i = 1; i <= 10; ++i) {
        s.record(std::chrono::milliseconds(i));
    }
    const auto r = s.summary();
    EXPECT_EQ(r.count, 4U);   // 只剩 7、8、9、10
    EXPECT_EQ(r.total, 10U);  // 一共记过 10 次
    EXPECT_DOUBLE_EQ(r.p50_ms, 8.0);
    EXPECT_DOUBLE_EQ(r.max_ms, 10.0);
}

TEST(StageTimer, ResetClearsEverything)
{
    StageStats s{"x", 4};
    s.record(5ms);
    s.reset();
    EXPECT_EQ(s.summary().count, 0U);
    EXPECT_EQ(s.summary().total, 0U);
    for (int i = 1; i <= 6; ++i) {  // 重置后窗口大小不变
        s.record(std::chrono::milliseconds(i));
    }
    EXPECT_EQ(s.summary().count, 4U);
}

TEST(StageTimer, ScopedTimerRecordsOnce)
{
    StageStats s{"x"};
    {
        const ScopedTimer t{s};
        std::this_thread::sleep_for(2ms);
    }
    const auto r = s.summary();
    EXPECT_EQ(r.count, 1U);
    EXPECT_GE(r.max_ms, 2.0);
}

TEST(StageTimer, RecordAndReportFromDifferentThreads)
{
    StageStats s{"x", 64};
    std::thread writer([&] {
        for (int i = 0; i < 20000; ++i) {
            s.record(std::chrono::microseconds(i % 1000));
        }
    });
    for (int i = 0; i < 200; ++i) {
        (void)s.report();
    }
    writer.join();
    EXPECT_EQ(s.summary().total, 20000U);
}

TEST(StageTimer, ZeroWindowIsRejected)
{
    EXPECT_THROW(StageStats("x", 0), std::invalid_argument);
}

TEST(StageTimer, WindowSizeIsExactEvenAfterReset)
{
    // 窗口大小只由构造参数决定，不依赖 vector 实际分配了多少容量（P5）
    StageStats s{"x", 3};
    for (int round = 0; round < 2; ++round) {
        for (int i = 1; i <= 10; ++i) {
            s.record(std::chrono::milliseconds(i));
        }
        EXPECT_EQ(s.summary().count, 3U);
        EXPECT_DOUBLE_EQ(s.summary().max_ms, 10.0);
        s.reset();
    }
}
