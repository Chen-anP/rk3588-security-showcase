// 编码阶段（第 8a 步）：用假的编码器测线程、丢帧、统计。TSan 下也要通过。
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "app/encode_stage.hpp"

using rksec::app::EncodeStage;
using rksec::media::EncodedPacket;
using rksec::media::VideoEncoder;
using rksec::media::YuvView;

namespace {

// 假编码器：每帧输出 4 字节（pts 的低位），每 gop 帧一个“关键帧”，可以指定慢、可以指定第几帧失败
class FakeEncoder final : public VideoEncoder {
  public:
    FakeEncoder(int gop, std::chrono::milliseconds delay, int fail_at = -1)
        : gop_(gop), delay_(delay), fail_at_(fail_at)
    {
    }

    bool encode(const YuvView &, std::int64_t pts_us, EncodedPacket &out, std::string &err) override
    {
        std::this_thread::sleep_for(delay_);
        const int n = calls_++;
        if (n == fail_at_) {
            err = "假的编码失败";
            return false;
        }
        out.keyframe = n % gop_ == 0;
        out.pts_us = pts_us;
        out.data.assign(4, static_cast<std::uint8_t>(pts_us & 0xFF));
        return true;
    }
    std::vector<std::uint8_t> parameter_sets() const override { return {0, 0, 0, 1, 0x67}; }

  private:
    int gop_;
    std::chrono::milliseconds delay_;
    int fail_at_;
    int calls_ = 0;
};

std::shared_ptr<const YuvView> frame()
{
    return std::make_shared<const YuvView>();
}

}  // namespace

TEST(EncodeStage, EncodesInOrderAndCounts)
{
    std::mutex m;
    std::vector<EncodedPacket> got;
    EncodeStage stage{std::make_unique<FakeEncoder>(3, std::chrono::milliseconds{0}), [&](const EncodedPacket &p) {
                          const std::lock_guard<std::mutex> lock(m);
                          got.push_back(p);
                      }};
    stage.start();
    for (int i = 0; i < 7; ++i) {
        stage.submit(frame(), 1000 + i);
        // 一帧一帧送，等它编完，免得队列满了挤掉
        for (int w = 0; w < 500 && stage.stats().encoded <= static_cast<std::uint64_t>(i); ++w) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }
    stage.stop();
    const auto s = stage.stats();
    EXPECT_EQ(s.submitted, 7U);
    EXPECT_EQ(s.encoded, 7U);
    EXPECT_EQ(s.dropped, 0U);
    EXPECT_EQ(s.failed, 0U);
    EXPECT_EQ(s.keyframes, 3U) << "第 0、3、6 帧";
    EXPECT_EQ(s.bytes, 28U);
    ASSERT_EQ(got.size(), 7U);
    for (std::size_t i = 0; i < got.size(); ++i) {
        EXPECT_EQ(got[i].pts_us, 1000 + static_cast<std::int64_t>(i)) << "顺序不乱";
    }
    EXPECT_EQ(stage.parameter_sets().back(), 0x67);
}

TEST(EncodeStage, SlowEncoderDropsOldestNotNewest)
{
    std::mutex m;
    std::vector<std::int64_t> pts;
    EncodeStage stage{std::make_unique<FakeEncoder>(100, std::chrono::milliseconds{30}), [&](const EncodedPacket &p) {
                          const std::lock_guard<std::mutex> lock(m);
                          pts.push_back(p.pts_us);
                      }};
    stage.start();
    for (int i = 0; i < 20; ++i) {
        stage.submit(frame(), i);  // 一口气送 20 帧，编码器一帧要 30 ms
    }
    stage.stop();
    const auto s = stage.stats();
    EXPECT_EQ(s.submitted, 20U);
    EXPECT_GT(s.dropped, 10U) << "编码跟不上，大部分被挤掉";
    EXPECT_EQ(s.encoded + s.dropped, 20U) << "每帧不是编了就是被挤掉，没有凭空消失";
    const std::lock_guard<std::mutex> lock(m);
    ASSERT_FALSE(pts.empty());
    EXPECT_EQ(pts.back(), 19) << "最新的一帧一定编到了（丢的是旧的）";
    for (std::size_t i = 1; i < pts.size(); ++i) {
        EXPECT_GT(pts[i], pts[i - 1]) << "输出顺序和送入顺序一致";
    }
}

TEST(EncodeStage, FailureIsCountedAndDoesNotStopTheStage)
{
    std::atomic<int> sunk{0};
    EncodeStage stage{std::make_unique<FakeEncoder>(100, std::chrono::milliseconds{0}, 1),
                      [&](const EncodedPacket &) { ++sunk; }};
    stage.start();
    for (int i = 0; i < 4; ++i) {
        stage.submit(frame(), i);
        for (int w = 0; w < 500 && stage.stats().encoded + stage.stats().failed <= static_cast<std::uint64_t>(i); ++w) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }
    stage.stop();
    const auto s = stage.stats();
    EXPECT_EQ(s.failed, 1U);
    EXPECT_EQ(s.encoded, 3U);
    EXPECT_EQ(sunk.load(), 3) << "失败的帧不交给下游";
}

TEST(EncodeStage, NullFrameAndLateSubmitAreIgnored)
{
    EncodeStage stage{std::make_unique<FakeEncoder>(1, std::chrono::milliseconds{0}), nullptr};
    stage.start();
    stage.submit(nullptr, 1);
    stage.stop();
    stage.submit(frame(), 2);  // 停了以后再送：不崩、不计数
    stage.stop();              // 可以重复停
    const auto s = stage.stats();
    EXPECT_EQ(s.submitted, 0U);
    EXPECT_EQ(s.encoded, 0U);
}

TEST(EncodeStage, HeldFramesAreReleasedAfterEncoding)
{
    // 解码器的缓冲区靠 shared_ptr 计数归还：编完、挤掉、停止以后都不能还拿着
    auto f = frame();
    {
        EncodeStage stage{std::make_unique<FakeEncoder>(1, std::chrono::milliseconds{5}), nullptr};
        stage.start();
        for (int i = 0; i < 10; ++i) {
            stage.submit(f, i);
        }
        stage.stop();
    }
    EXPECT_EQ(f.use_count(), 1) << "队列里挤掉的、编完的、停止时剩下的都放手了";
}
