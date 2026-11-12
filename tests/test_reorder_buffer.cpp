// 按序号重新排队（第 6 步）：几个 NPU 核并行时结果会乱序到达，放出去时必须恢复成帧的顺序。
#include "core/reorder_buffer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <numeric>
#include <random>
#include <vector>

using rksec::ReorderBuffer;

TEST(ReorderBuffer, InOrderPassesStraightThrough)
{
    ReorderBuffer<int> rb;
    for (int i = 0; i < 5; ++i) {
        const auto out = rb.push(static_cast<std::uint64_t>(i), i * 10);
        ASSERT_EQ(out.size(), 1U);
        EXPECT_EQ(out[0], i * 10);
    }
    EXPECT_EQ(rb.stats().held, 0U);
    EXPECT_EQ(rb.stats().released, 5U);
}

TEST(ReorderBuffer, EarlyResultsWaitForTheMissingOne)
{
    ReorderBuffer<int> rb;
    EXPECT_TRUE(rb.push(1, 1).empty()) << "0 还没到，1 要等";
    EXPECT_TRUE(rb.push(2, 2).empty());
    EXPECT_EQ(rb.pending(), 2U);
    const auto out = rb.push(0, 0);
    ASSERT_EQ(out.size(), 3U);
    EXPECT_EQ(out, (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(rb.next(), 3U);
    EXPECT_EQ(rb.pending(), 0U);
    EXPECT_EQ(rb.stats().held, 2U);
    EXPECT_EQ(rb.stats().max_pending, 2U);
}

TEST(ReorderBuffer, DuplicatesAndLateArrivalsAreIgnored)
{
    ReorderBuffer<int> rb;
    rb.push(0, 0);
    EXPECT_TRUE(rb.push(0, 99).empty()) << "已经放出去的序号又来：丢掉，顺序不能倒退";
    rb.push(2, 2);
    EXPECT_TRUE(rb.push(2, 98).empty()) << "暂存中的序号又来：丢掉";
    EXPECT_EQ(rb.stats().ignored, 2U);
    const auto out = rb.push(1, 1);
    EXPECT_EQ(out, (std::vector<int>{1, 2}));
}

TEST(ReorderBuffer, MoveOnlyValues)
{
    ReorderBuffer<std::unique_ptr<int>> rb{10};
    EXPECT_TRUE(rb.push(11, std::make_unique<int>(11)).empty());
    auto out = rb.push(10, std::make_unique<int>(10));
    ASSERT_EQ(out.size(), 2U);
    EXPECT_EQ(*out[0], 10);
    EXPECT_EQ(*out[1], 11);
}

TEST(ReorderBuffer, RandomArrivalOrderAlwaysComesOutSorted)
{
    std::vector<int> ids(200);
    std::iota(ids.begin(), ids.end(), 0);
    std::mt19937 rng{42};
    std::shuffle(ids.begin(), ids.end(), rng);
    ReorderBuffer<int> rb;
    std::vector<int> got;
    for (const int id : ids) {
        for (int v : rb.push(static_cast<std::uint64_t>(id), id)) {
            got.push_back(v);
        }
    }
    ASSERT_EQ(got.size(), ids.size());
    EXPECT_TRUE(std::is_sorted(got.begin(), got.end()));
    EXPECT_EQ(rb.pending(), 0U);
}
