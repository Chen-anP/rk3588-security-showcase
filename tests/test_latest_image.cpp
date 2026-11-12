#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>

#include "ui/latest_image.hpp"

using rksec::media::Image;
using rksec::ui::LatestImage;

namespace {

void fill(Image &img, std::uint8_t v)
{
    img.width = 4;
    img.height = 2;
    img.stride = 16;
    img.pixels.assign(32, v);
}

}  // namespace

TEST(LatestImage, NothingBeforeFirstPublish)
{
    LatestImage l;
    EXPECT_EQ(l.take(), nullptr);
    EXPECT_EQ(l.front().width, 0);
}

TEST(LatestImage, ConsumerGetsNewestOnly)
{
    LatestImage l;
    fill(l.back(), 1);
    l.publish();
    fill(l.back(), 2);
    l.publish();  // 1 还没被拿走就被 2 覆盖
    const Image *img = l.take();
    ASSERT_NE(img, nullptr);
    EXPECT_EQ(img->pixels[0], 2);
    EXPECT_EQ(l.take(), nullptr) << "没有新帧时返回空，手上那张继续有效";
    EXPECT_EQ(l.front().pixels[0], 2);
    const auto s = l.stats();
    EXPECT_EQ(s.published, 2U);
    EXPECT_EQ(s.taken, 1U);
    EXPECT_EQ(s.overwritten, 1U);
}

TEST(LatestImage, ProducerNeverWritesIntoConsumersImage)
{
    LatestImage l;
    fill(l.back(), 1);
    l.publish();
    const Image *held = l.take();  // 界面正在用这一张
    ASSERT_NE(held, nullptr);
    for (std::uint8_t v = 2; v < 10; ++v) {  // 生产者连续写好几帧
        fill(l.back(), v);
        l.publish();
        EXPECT_EQ(held->pixels[0], 1) << "界面手上的图不能被改";
    }
    EXPECT_EQ(l.take()->pixels[0], 9);
}

// 两个线程同时跑：每张图的所有字节都是同一个值，消费者拿到的图如果字节不一致，说明读到了写了一半的图。
// width 记录这是第几帧，用来检查拿到的帧越来越新
TEST(LatestImage, ConcurrentNoTornImages)
{
    LatestImage l;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (int i = 1; i <= 20000; ++i) {
            Image &b = l.back();
            b.width = i;
            b.height = 4;
            b.stride = 256;
            b.pixels.resize(1024);
            const auto v = static_cast<std::uint8_t>(i);
            for (auto &p : b.pixels) {
                p = v;
            }
            l.publish();
        }
        done = true;
    });
    std::uint64_t checked = 0;
    int last = 0;
    while (!done || checked == 0) {
        if (const Image *img = l.take()) {
            const std::uint8_t v = img->pixels[0];
            for (const auto p : img->pixels) {
                ASSERT_EQ(p, v) << "读到了写了一半的图";
            }
            EXPECT_EQ(v, static_cast<std::uint8_t>(img->width)) << "像素和帧号对不上";
            EXPECT_GT(img->width, last) << "take() 返回的应该比上一次新";
            last = img->width;
            ++checked;
        }
    }
    producer.join();
    EXPECT_GT(checked, 0U);
}
