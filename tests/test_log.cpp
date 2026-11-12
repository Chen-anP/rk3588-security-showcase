#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

#include "core/log.hpp"

namespace log = rksec::log;

namespace {

// 每个用例把日志收集到 lines 里，结束时恢复默认
class LogTest : public ::testing::Test {
  protected:
    void SetUp() override
    {
        log::set_level(log::Level::debug);
        log::set_journald(false);
        log::set_sink([this](log::Level, std::string_view line) { lines.emplace_back(line); });
    }
    void TearDown() override
    {
        log::set_sink(nullptr);
        log::set_level(log::Level::info);
    }
    std::vector<std::string> lines;
};

}  // namespace

TEST_F(LogTest, FormatsTerminalLine)
{
    LOG_I("camera", "打开 %s，%dx%d", "/dev/x", 1280, 720);
    ASSERT_EQ(lines.size(), 1U);
    // "2026-10-28 14:03:22.123 I [camera] 打开 /dev/x，1280x720"
    const auto &l = lines[0];
    EXPECT_EQ(l.size(), 23U + std::string{" I [camera] 打开 /dev/x，1280x720"}.size()) << l;
    EXPECT_EQ(l.substr(23), " I [camera] 打开 /dev/x，1280x720");
    EXPECT_EQ(l[4], '-');
    EXPECT_EQ(l[19], '.');
}

TEST_F(LogTest, JournaldLineHasSyslogPriority)
{
    log::set_journald(true);
    LOG_D("t", "d");
    LOG_I("t", "i");
    LOG_W("t", "w");
    LOG_E("t", "e");
    ASSERT_EQ(lines.size(), 4U);
    EXPECT_EQ(lines[0], "<7>[t] d");
    EXPECT_EQ(lines[1], "<6>[t] i");
    EXPECT_EQ(lines[2], "<4>[t] w");
    EXPECT_EQ(lines[3], "<3>[t] e");
}

TEST_F(LogTest, LevelFilters)
{
    log::set_level(log::Level::warn);
    LOG_D("t", "d");
    LOG_I("t", "i");
    LOG_W("t", "w");
    LOG_E("t", "e");
    EXPECT_EQ(lines.size(), 2U);
}

TEST_F(LogTest, DisabledLevelDoesNotEvaluateArguments)
{
    log::set_level(log::Level::error);
    int calls = 0;
    const auto expensive = [&calls] {
        ++calls;
        return 1;
    };
    LOG_D("t", "%d", expensive());
    EXPECT_EQ(calls, 0) << "被过滤的日志不应该计算参数";
}

TEST_F(LogTest, LongMessageIsTruncatedOnCharacterBoundary)
{
    log::set_journald(true);
    std::string big;
    for (int i = 0; i < 600; ++i) {
        big += "汉";  // 3 字节，共 1800 字节
    }
    LOG_I("t", "%s", big.c_str());
    ASSERT_EQ(lines.size(), 1U);
    const std::string &l = lines[0];
    const std::string ellipsis = "…";
    ASSERT_GE(l.size(), ellipsis.size());
    EXPECT_EQ(l.substr(l.size() - ellipsis.size()), ellipsis);
    const std::string body = l.substr(std::string{"<6>[t] "}.size(), l.size() - 7 - ellipsis.size());
    EXPECT_LE(body.size(), 1023U);
    EXPECT_EQ(body.size() % 3, 0U) << "不能切在一个汉字中间";
}

TEST_F(LogTest, ParseLevel)
{
    EXPECT_EQ(log::parse_level("warn"), log::Level::warn);
    EXPECT_FALSE(log::parse_level("WARN")) << "只认小写，配置里写错要报错";
    EXPECT_EQ(log::level_name(log::Level::error), "error");
}

TEST_F(LogTest, ConcurrentWritesDoNotInterleave)
{
    // 4 个线程各写 500 条：每条都应完整（TSan 预设下还会检查数据竞争）
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < 500; ++i) {
                LOG_I("thr", "thread=%d i=%d end", t, i);
            }
        });
    }
    for (auto &th : threads) {
        th.join();
    }
    ASSERT_EQ(lines.size(), 2000U);
    for (const auto &l : lines) {
        ASSERT_EQ(l.substr(l.size() - 3), "end") << l;
    }
}
