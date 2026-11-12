#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "media/jpeg_decoder.hpp"

// 最新一帧图像的“信箱”（三缓冲）：解码线程不停地放新图，界面线程想画的时候拿最新的一张。
//
// 为什么是三块：
// - 生产者有一块自己在写（back），写完 publish() 和 ready 交换
// - 消费者有一块自己在读（front），界面库渲染时一直在读它；take() 时和 ready 交换
// - 中间的 ready 是“最新的完整一帧”
// 两边都不用等对方：生产者快了，旧的 ready 被新的覆盖（跳帧，预览只要最新的）；消费者快了，take() 返回
// nullptr，继续用手上那张。 锁只保护三个下标的交换，持锁时间极短，不拷贝像素。
//
// 线程：back()/publish() 只在一个生产者线程调用；take()/front() 只在一个消费者线程调用。
namespace rksec::ui {

class LatestImage {
  public:
    // 生产者：拿到可以写的那块（内容是两次以前的旧图，尺寸可能不对，要自己设置）
    media::Image &back() { return slots_[back_]; }

    // 生产者：back 写好了，变成最新的一帧
    void publish()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::swap(back_, ready_);
        if (fresh_) {
            ++stats_.overwritten;  // 上一帧还没被拿走就被盖掉了
        }
        fresh_ = true;
        ++stats_.published;
    }

    // 消费者：有比手上更新的帧就换过来并返回它；没有返回 nullptr（手上那张仍然有效，用 front() 取）
    const media::Image *take()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!fresh_) {
            return nullptr;
        }
        std::swap(front_, ready_);
        fresh_ = false;
        ++stats_.taken;
        return &slots_[front_];
    }

    // 消费者：手上那张（从没拿到过时 width 为 0）
    const media::Image &front() const { return slots_[front_]; }

    struct Stats {
        std::uint64_t published = 0;
        std::uint64_t taken = 0;
        std::uint64_t overwritten = 0;
    };
    Stats stats() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

  private:
    mutable std::mutex mutex_;
    std::array<media::Image, 3> slots_;
    // back_ 只有生产者改（在锁里），front_ 只有消费者改（在锁里），ready_ 两边都改（都在锁里）
    std::size_t back_ = 0;
    std::size_t ready_ = 1;
    std::size_t front_ = 2;
    bool fresh_ = false;
    Stats stats_;
};

}  // namespace rksec::ui
