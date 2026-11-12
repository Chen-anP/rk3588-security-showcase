#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include "capture/capture_service.hpp"
#include "fake_v4l2.hpp"

using rksec::FrameRef;
using rksec::capture::CaptureService;
using rksec::capture::ServiceOptions;
using rksec::capture::ServiceStats;
using namespace std::chrono_literals;

namespace {

// 测试用的短时间参数：重试 5～20 ms，卡住判定 100 ms
ServiceOptions fast()
{
    ServiceOptions o;
    o.camera.device = "/dev/v4l/by-id/fake-video-index0";
    o.frame_timeout = 10ms;
    o.stall_after = 100ms;
    o.retry_min = 5ms;
    o.retry_max = 20ms;
    return o;
}

// 等到条件成立（最多 5 秒），返回最后一次的统计
ServiceStats wait_until(const CaptureService &svc, const std::function<bool(const ServiceStats &)> &cond)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    ServiceStats s = svc.stats();
    while (!cond(s) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
        s = svc.stats();
    }
    return s;
}

}  // namespace

TEST(CaptureService, DeliversFramesToSink)
{
    fake::FakeV4l2 dev;
    std::atomic<int> got{0};
    CaptureService svc{dev, fast(), [&](FrameRef f) {
                           EXPECT_NE(f->data, nullptr);
                           ++got;
                       }};  // f 在 lambda 结束时放手 → QBUF
    svc.start();
    const auto s = wait_until(svc, [](const ServiceStats &st) { return st.frames >= 100; });
    svc.stop();
    EXPECT_GE(s.frames, 100U);
    EXPECT_EQ(s.sessions, 1U);
    EXPECT_EQ(s.open_failures, 0U);
    EXPECT_EQ(got.load(), static_cast<int>(svc.stats().frames));
    EXPECT_FALSE(svc.stats().streaming);
}

TEST(CaptureService, ReconnectsAfterUnplugAndReplug)
{
    fake::FakeV4l2 dev;
    CaptureService svc{dev, fast(), [](FrameRef) {}};
    svc.start();
    wait_until(svc, [](const ServiceStats &st) { return st.frames >= 20; });

    dev.unplug();
    auto s = wait_until(svc, [](const ServiceStats &st) { return st.open_failures >= 3; });
    EXPECT_FALSE(s.streaming);
    EXPECT_GE(s.open_failures, 3U) << "拔线期间一直在重试";
    const auto frames_before = s.frames;

    dev.replug();
    s = wait_until(svc, [&](const ServiceStats &st) { return st.sessions >= 2 && st.frames > frames_before + 20; });
    svc.stop();
    EXPECT_EQ(s.sessions, 2U) << "插回去自动恢复";
    EXPECT_GT(s.frames, frames_before + 20);
}

TEST(CaptureService, StartsWithoutCameraAndPicksItUpLater)
{
    fake::FakeV4l2 dev;
    dev.present = false;  // 开机时摄像头还没枚举出来
    CaptureService svc{dev, fast(), [](FrameRef) {}};
    svc.start();
    auto s = wait_until(svc, [](const ServiceStats &st) { return st.open_failures >= 2; });
    EXPECT_EQ(s.frames, 0U);
    EXPECT_NE(s.last_error.find("打不开"), std::string::npos) << s.last_error;
    dev.replug();
    s = wait_until(svc, [](const ServiceStats &st) { return st.frames >= 10; });
    svc.stop();
    EXPECT_GE(s.frames, 10U);
}

TEST(CaptureService, StalledCameraIsReopened)
{
    fake::FakeV4l2 dev;
    CaptureService svc{dev, fast(), [](FrameRef) {}};
    svc.start();
    wait_until(svc, [](const ServiceStats &st) { return st.frames >= 10; });
    dev.set_stalled(true);  // 设备还在，但不出帧了
    auto s = wait_until(svc, [](const ServiceStats &st) { return st.stalls >= 1; });
    EXPECT_GE(s.stalls, 1U);
    dev.set_stalled(false);
    const auto frames_before = s.frames;
    s = wait_until(svc, [&](const ServiceStats &st) { return st.frames > frames_before + 10; });
    svc.stop();
    EXPECT_GE(s.sessions, 2U) << "卡住后关掉重开";
    EXPECT_GE(dev.open_count(), 2);
}

TEST(CaptureService, StopIsPromptEvenWhileWaitingToRetry)
{
    fake::FakeV4l2 dev;
    dev.present = false;
    ServiceOptions o = fast();
    o.retry_min = 5s;  // 重试间隔很长
    o.retry_max = 5s;
    CaptureService svc{dev, o, [](FrameRef) {}};
    svc.start();
    const auto s = wait_until(svc, [](const ServiceStats &st) { return st.open_failures >= 1; });
    ASSERT_GE(s.open_failures, 1U) << "采集线程应该已经打开失败、进入了重试等待";
    const auto t0 = std::chrono::steady_clock::now();
    svc.stop();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    EXPECT_LT(ms, 500) << "stop 用了 " << ms << " ms，不能等完 5 秒的重试间隔";
}

TEST(CaptureService, NoDmabufOrMappingLeaksAcrossReconnects)
{
    fake::FakeV4l2 dev;
    CaptureService svc{dev, fast(), [](FrameRef) {}};
    svc.start();
    for (int i = 0; i < 3; ++i) {  // 拔插 3 次
        wait_until(svc, [&](const ServiceStats &st) { return st.sessions >= static_cast<std::uint64_t>(i + 1); });
        dev.unplug();
        wait_until(svc, [](const ServiceStats &st) { return !st.streaming; });
        dev.replug();
    }
    wait_until(svc, [](const ServiceStats &st) { return st.sessions >= 4 && st.streaming; });
    svc.stop();
    EXPECT_EQ(dev.exported_open(), 0U) << "每次断开都要关掉导出的 DMA-BUF fd";
    EXPECT_EQ(dev.opens, dev.closes);
}
