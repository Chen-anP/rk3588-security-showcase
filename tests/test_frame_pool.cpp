#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/bounded_queue.hpp"
#include "core/frame_pool.hpp"

using rksec::BoundedQueue;
using rksec::FramePool;
using rksec::FrameRef;

namespace {

auto now()
{
    return std::chrono::steady_clock::now();
}

}  // namespace

TEST(FramePool, AcquireFillsFrame)
{
    FramePool pool{4, [](int) {}};
    const auto t = now();
    auto f = pool.acquire(2, 100, t);
    ASSERT_TRUE(f);
    EXPECT_EQ(f->index, 2);
    EXPECT_EQ(f->sequence, 100U);
    EXPECT_EQ(f->captured, t);
    EXPECT_EQ(pool.in_use(), 1);
    EXPECT_EQ(pool.count(), 4);
}

TEST(FramePool, ReleasedOnceWhenLastCopyIsDropped)
{
    std::vector<int> released;
    FramePool pool{4, [&](int i) { released.push_back(i); }};
    auto a = pool.acquire(1, 0, now());
    FrameRef b = a;  // 第二个使用者（比如显示和检测同时拿着这一帧）
    a.reset();
    EXPECT_TRUE(released.empty()) << "还有人在用，不能还";
    EXPECT_EQ(pool.in_use(), 1);
    b.reset();
    EXPECT_EQ(released, std::vector<int>{1});
    EXPECT_EQ(pool.in_use(), 0);
    const auto s = pool.stats();
    EXPECT_EQ(s.acquired, 1U);
    EXPECT_EQ(s.released, 1U);
}

TEST(FramePool, BufferCanBeReusedAfterRelease)
{
    FramePool pool{1, [](int) {}};
    auto f = pool.acquire(0, 1, now());
    f.reset();
    auto g = pool.acquire(0, 2, now());
    ASSERT_TRUE(g);
    EXPECT_EQ(g->sequence, 2U);
}

TEST(FramePool, RejectsBusyOrInvalidIndex)
{
    FramePool pool{2, [](int) {}};
    auto f = pool.acquire(0, 0, now());
    EXPECT_FALSE(pool.acquire(0, 1, now())) << "还没还就又 acquire，是调用方的 bug";
    EXPECT_FALSE(pool.acquire(-1, 0, now()));
    EXPECT_FALSE(pool.acquire(2, 0, now()));
    EXPECT_EQ(pool.stats().rejected, 3U);
    EXPECT_EQ(pool.in_use(), 1);
}

TEST(FramePool, InvalidCountIsRejected)
{
    EXPECT_THROW(FramePool(0, [](int) {}), std::invalid_argument);
}

TEST(FramePool, CallbackRunsOutsideTheLock)
{
    // 回调里再调用池子：如果拿着锁调用回调，这里会死锁（测试超时失败）
    int seen_in_use = -1;
    std::unique_ptr<FramePool> pool;
    pool = std::make_unique<FramePool>(2, [&](int) { seen_in_use = pool->in_use(); });
    auto f = pool->acquire(0, 0, now());
    f.reset();
    EXPECT_EQ(seen_in_use, 0);
}

TEST(FramePool, FrameOutlivingPoolIsSafeAndNotReturned)
{
    int calls = 0;
    FrameRef f;
    {
        FramePool pool{2, [&](int) { ++calls; }};
        f = pool.acquire(1, 7, now());
    }                            // 池子先没了（采集已停止）
    EXPECT_EQ(f->sequence, 7U);  // 帧还能读（ASan 会检查是不是读了已释放的内存）
    f.reset();
    EXPECT_EQ(calls, 0) << "池子销毁以后不能再调用回调";
}

// 模拟第 3 步的真实用法：采集线程循环使用 4 个缓冲区，经过一个容量 2 的队列交给处理线程。
// 处理慢，队列会丢旧帧；被丢的帧也必须把缓冲区还回来，否则采集很快就没有缓冲区可用。
// 用 tsan 预设跑这个测试。
TEST(FramePool, BuffersAlwaysComeBackThroughDroppingQueue)
{
    constexpr int kBuffers = 4;
    constexpr int kFrames = 3000;

    std::mutex free_mutex;
    std::deque<int> free_list{0, 1, 2, 3};  // 相当于驱动里空闲的缓冲区
    FramePool pool{kBuffers, [&](int i) {
                       const std::lock_guard<std::mutex> lock(free_mutex);
                       free_list.push_back(i);  // 相当于 QBUF
                   }};
    BoundedQueue<FrameRef> q{2};

    std::thread worker([&] {
        while (auto f = q.pop()) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));  // 处理比采集慢
        }
    });

    int captured = 0;
    std::uint64_t seq = 0;
    auto last_progress = now();
    while (captured < kFrames) {
        int index = -1;
        {
            const std::lock_guard<std::mutex> lock(free_mutex);
            if (!free_list.empty()) {
                index = free_list.front();  // 相当于 DQBUF
                free_list.pop_front();
            }
        }
        if (index < 0) {
            // 缓冲区都在用，等一等；2 秒都没等到，说明有缓冲区没还回来（真实设备上就是采集卡死）
            if (now() - last_progress > std::chrono::seconds(2)) {
                ADD_FAILURE() << "2 秒没有空闲缓冲区，已采集 " << captured << " 帧，in_use=" << pool.in_use();
                break;
            }
            std::this_thread::yield();
            continue;
        }
        last_progress = now();
        auto f = pool.acquire(index, seq++, now());
        ASSERT_TRUE(f);
        q.push(std::move(f));
        ++captured;
    }
    q.close();
    worker.join();

    EXPECT_GT(q.stats().dropped, 0U) << "这个测试应该触发丢帧";
    EXPECT_EQ(pool.in_use(), 0);
    const auto s = pool.stats();
    EXPECT_EQ(s.acquired, static_cast<std::uint64_t>(kFrames));
    EXPECT_EQ(s.released, s.acquired);
    const std::lock_guard<std::mutex> lock(free_mutex);
    EXPECT_EQ(free_list.size(), static_cast<std::size_t>(kBuffers));
}
