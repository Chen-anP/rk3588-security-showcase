#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "core/bounded_queue.hpp"

using rksec::BoundedQueue;
using rksec::PushResult;
using namespace std::chrono_literals;

TEST(BoundedQueue, FifoOrder)
{
    BoundedQueue<int> q{4};
    for (int i = 1; i <= 3; ++i) {
        EXPECT_EQ(q.push(i), PushResult::ok);
    }
    EXPECT_EQ(q.size(), 3U);
    EXPECT_EQ(q.try_pop(), 1);
    EXPECT_EQ(q.try_pop(), 2);
    EXPECT_EQ(q.try_pop(), 3);
    EXPECT_EQ(q.try_pop(), std::nullopt);
}

TEST(BoundedQueue, FullQueueDropsOldest)
{
    BoundedQueue<int> q{3};
    for (int i = 1; i <= 3; ++i) {
        q.push(i);
    }
    EXPECT_EQ(q.push(4), PushResult::dropped_oldest);
    EXPECT_EQ(q.push(5), PushResult::dropped_oldest);
    EXPECT_EQ(q.size(), 3U);
    EXPECT_EQ(q.try_pop(), 3);
    EXPECT_EQ(q.try_pop(), 4);
    EXPECT_EQ(q.try_pop(), 5);
    const auto s = q.stats();
    EXPECT_EQ(s.pushed, 5U);
    EXPECT_EQ(s.dropped, 2U);
    EXPECT_EQ(s.popped, 3U);
}

TEST(BoundedQueue, ZeroCapacityIsRejected)
{
    EXPECT_THROW(BoundedQueue<int>{0}, std::invalid_argument);
}

TEST(BoundedQueue, WorksWithMoveOnlyTypes)
{
    BoundedQueue<std::unique_ptr<int>> q{1};
    q.push(std::make_unique<int>(1));
    q.push(std::make_unique<int>(2));  // 丢掉 1，unique_ptr 正常释放（ASan 会检查泄漏）
    auto v = q.try_pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(**v, 2);
}

TEST(BoundedQueue, PopForTimesOut)
{
    BoundedQueue<int> q{1};
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(q.pop_for(30ms), std::nullopt);
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 25ms);
}

TEST(BoundedQueue, CloseWakesBlockedConsumer)
{
    BoundedQueue<int> q{1};
    std::atomic<bool> returned{false};
    std::thread consumer([&] {
        EXPECT_EQ(q.pop(), std::nullopt);
        returned = true;
    });
    std::this_thread::sleep_for(20ms);  // 让消费者先进入等待
    EXPECT_FALSE(returned);
    q.close();
    consumer.join();
    EXPECT_TRUE(returned);
}

TEST(BoundedQueue, ClosedQueueRejectsPushButDrainsRemaining)
{
    BoundedQueue<int> q{4};
    q.push(1);
    q.push(2);
    q.close();
    EXPECT_TRUE(q.closed());
    EXPECT_EQ(q.push(3), PushResult::closed);
    EXPECT_EQ(q.pop(), 1);  // 关闭前放进去的还能取出来
    EXPECT_EQ(q.pop(), 2);
    EXPECT_EQ(q.pop(), std::nullopt);
}

namespace {

// 析构时访问队列：如果队列拿着锁析构被丢掉的元素，这里会死锁（测试超时失败）
struct TouchesQueueOnDestroy {
    BoundedQueue<TouchesQueueOnDestroy> *q = nullptr;
    TouchesQueueOnDestroy() = default;
    explicit TouchesQueueOnDestroy(BoundedQueue<TouchesQueueOnDestroy> *queue) : q(queue) {}
    TouchesQueueOnDestroy(TouchesQueueOnDestroy &&o) noexcept : q(o.q) { o.q = nullptr; }
    TouchesQueueOnDestroy &operator=(TouchesQueueOnDestroy &&o) noexcept
    {
        q = o.q;
        o.q = nullptr;
        return *this;
    }
    TouchesQueueOnDestroy(const TouchesQueueOnDestroy &) = delete;
    TouchesQueueOnDestroy &operator=(const TouchesQueueOnDestroy &) = delete;
    ~TouchesQueueOnDestroy()
    {
        if (q != nullptr) {
            (void)q->size();
        }
    }
};

}  // namespace

TEST(BoundedQueue, DroppedItemIsDestroyedOutsideTheLock)
{
    BoundedQueue<TouchesQueueOnDestroy> q{1};
    q.push(TouchesQueueOnDestroy{&q});
    EXPECT_EQ(q.push(TouchesQueueOnDestroy{&q}), PushResult::dropped_oldest);
    q.close();
    while (q.try_pop()) {
    }
}

// 两个生产者、一个消费者，队列很小，一定会丢。检查：
// 1) 数量守恒：放进去的 = 取走的 + 丢掉的
// 2) 每个生产者的数据在消费者那里仍然是递增的（丢旧不会打乱顺序）
// 用 tsan 预设跑这个测试，能发现加锁上的错误
TEST(BoundedQueue, ConcurrentProducersAndConsumerKeepCountsAndOrder)
{
    constexpr int kPerProducer = 20000;
    BoundedQueue<std::pair<int, int>> q{8};  // (生产者编号, 序号)
    std::vector<int> last_seen{-1, -1};
    std::uint64_t received = 0;

    std::thread consumer([&] {
        while (auto v = q.pop()) {
            const auto [producer, seq] = *v;
            EXPECT_GT(seq, last_seen[static_cast<std::size_t>(producer)]);
            last_seen[static_cast<std::size_t>(producer)] = seq;
            ++received;
        }
    });
    std::vector<std::thread> producers;
    for (int p = 0; p < 2; ++p) {
        producers.emplace_back([&q, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                q.push({p, i});
            }
        });
    }
    for (auto &t : producers) {
        t.join();
    }
    q.close();
    consumer.join();

    const auto s = q.stats();
    EXPECT_EQ(s.pushed, 2U * kPerProducer);
    EXPECT_EQ(s.pushed, s.popped + s.dropped);
    EXPECT_EQ(s.popped, received);
}
