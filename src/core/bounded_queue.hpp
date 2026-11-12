#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

// 有界队列：线程之间传帧用。
// - 满了再放，丢掉最旧的一个（实时系统宁可丢旧帧，也不能越积越多、延迟越来越大）
// - pop 阻塞等待；close() 之后不再接收，消费者把剩下的取完后拿到 nullopt，用来让线程退出
// - 被丢掉的元素在锁外析构：元素可能是帧句柄，析构时要把缓冲区还给采集，不能拿着队列的锁去做
// - 线程安全，任意多个生产者和消费者
namespace rksec {

enum class PushResult : std::uint8_t {
    ok,              // 放进去了
    dropped_oldest,  // 放进去了，但为此丢掉了最旧的一个
    closed,          // 队列已关闭，没有放进去（传进来的元素被析构）
};

template <typename T> class BoundedQueue {
  public:
    struct Stats {
        std::uint64_t pushed = 0;   // 放进去的总数（含后来被丢掉的）
        std::uint64_t popped = 0;   // 取走的总数
        std::uint64_t dropped = 0;  // 因为满了被丢掉的总数
    };

    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity)
    {
        if (capacity == 0) {
            throw std::invalid_argument("BoundedQueue 容量不能为 0");
        }
    }

    BoundedQueue(const BoundedQueue &) = delete;
    BoundedQueue &operator=(const BoundedQueue &) = delete;

    PushResult push(T value)
    {
        std::optional<T> victim;  // 被丢掉的元素，出了锁再析构
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return PushResult::closed;
            }
            if (items_.size() == capacity_) {
                victim.emplace(std::move(items_.front()));
                items_.pop_front();
                ++stats_.dropped;
            }
            items_.push_back(std::move(value));
            ++stats_.pushed;
        }
        not_empty_.notify_one();
        return victim ? PushResult::dropped_oldest : PushResult::ok;
    }

    // 一直等到有元素；队列关闭且取空后返回 nullopt
    std::optional<T> pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return !items_.empty() || closed_; });
        return take_locked();
    }

    // 最多等 timeout；超时或关闭且取空返回 nullopt
    template <typename Rep, typename Period> std::optional<T> pop_for(std::chrono::duration<Rep, Period> timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait_for(lock, timeout, [this] { return !items_.empty() || closed_; });
        return take_locked();
    }

    // 不等待
    std::optional<T> try_pop()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return take_locked();
    }

    // 关闭：之后 push 返回 closed；等待中的 pop 全部被唤醒
    void close()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
    }

    bool closed() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    std::size_t size() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    std::size_t capacity() const { return capacity_; }

    Stats stats() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

  private:
    std::optional<T> take_locked()
    {
        if (items_.empty()) {
            return std::nullopt;
        }
        std::optional<T> v{std::move(items_.front())};
        items_.pop_front();
        ++stats_.popped;
        return v;
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::deque<T> items_;
    bool closed_ = false;
    Stats stats_;
};

}  // namespace rksec
