#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "capture/v4l2_camera.hpp"
#include "core/bounded_queue.hpp"
#include "fake_v4l2.hpp"

using rksec::FrameRef;
using rksec::capture::Options;
using rksec::capture::Status;
using rksec::capture::V4l2Camera;
using namespace std::chrono_literals;

namespace {

Options opts()
{
    Options o;
    o.device = "/dev/v4l/by-id/fake-video-index0";
    return o;
}

// 打开并开始采集，失败时让测试立即停下并给出原因
void open_and_start(V4l2Camera &cam)
{
    std::string err;
    ASSERT_TRUE(cam.open(err)) << err;
    ASSERT_TRUE(cam.start(err)) << err;
}

}  // namespace

TEST(V4l2Camera, DeliversFramesWithDataSequenceAndDriverTimestamp)
{
    fake::FakeV4l2 dev;
    V4l2Camera cam{dev, opts()};
    open_and_start(cam);
    EXPECT_EQ(cam.width(), 1280);
    EXPECT_EQ(cam.height(), 720);
    EXPECT_EQ(cam.buffer_size(), dev.sizeimage);

    auto a = cam.next(100);
    auto b = cam.next(100);
    ASSERT_EQ(a.status, Status::frame) << a.error;
    ASSERT_EQ(b.status, Status::frame) << b.error;
    EXPECT_EQ(a.frame->sequence, 0U);
    EXPECT_EQ(b.frame->sequence, 1U);
    ASSERT_NE(a.frame->data, nullptr);
    EXPECT_EQ(a.frame->data[0], 0xFF);  // JPEG 开头
    EXPECT_EQ(a.frame->data[1], 0xD8);
    EXPECT_EQ(b.frame->data[2], 1);  // 数据确实是第 1 帧的
    EXPECT_EQ(a.frame->bytes, 1000U);
    // 驱动时间戳原样换算：两帧相差 33.333 ms
    EXPECT_EQ(b.frame->captured - a.frame->captured, 33333us);
    EXPECT_EQ(cam.stats().frames, 2U);
}

TEST(V4l2Camera, ReleasingAFrameGivesTheBufferBackToTheDriver)
{
    fake::FakeV4l2 dev;
    V4l2Camera cam{dev, opts()};
    open_and_start(cam);
    std::vector<FrameRef> held;
    for (int i = 0; i < 4; ++i) {  // 4 个缓冲区全部拿在手里
        auto r = cam.next(100);
        ASSERT_EQ(r.status, Status::frame) << r.error;
        held.push_back(r.frame);
    }
    EXPECT_EQ(dev.queued(), 0U);
    EXPECT_EQ(cam.next(10).status, Status::timeout) << "驱动没有缓冲区可写，就不会有新帧";

    held.erase(held.begin());  // 放手一帧 → 自动 QBUF
    EXPECT_EQ(dev.queued(), 1U);
    EXPECT_EQ(cam.next(100).status, Status::frame);
}

TEST(V4l2Camera, FormatChangedByDriverIsAnError)
{
    fake::FakeV4l2 dev;
    dev.sizes = {{640, 480}};  // 不支持 1280x720
    V4l2Camera cam{dev, opts()};
    std::string err;
    EXPECT_FALSE(cam.open(err));
    EXPECT_NE(err.find("驱动改成了 MJPG 640x480"), std::string::npos) << err;
    EXPECT_EQ(dev.opens, dev.closes) << "失败时要把设备关掉";
}

TEST(V4l2Camera, MissingDeviceAndMetadataNodeAreClearErrors)
{
    fake::FakeV4l2 dev;
    dev.present = false;
    V4l2Camera cam{dev, opts()};
    std::string err;
    EXPECT_FALSE(cam.open(err));
    EXPECT_NE(err.find("打不开"), std::string::npos) << err;

    dev.present = true;
    dev.caps = V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING;  // uvcvideo 的 index1 是元数据节点
    EXPECT_FALSE(cam.open(err));
    EXPECT_NE(err.find("index1"), std::string::npos) << err;
}

TEST(V4l2Camera, PowerLineAndFpsAreSetAndReadBack)
{
    EXPECT_EQ(rksec::capture::power_line_menu_value(50), 1);
    EXPECT_EQ(rksec::capture::power_line_menu_value(60), 2);
    EXPECT_EQ(rksec::capture::power_line_menu_value(0), 0);
    EXPECT_EQ(rksec::capture::power_line_menu_value(55), -1);

    fake::FakeV4l2 dev;
    dev.fps_given = 15;  // 驱动只给 15 fps
    Options o = opts();
    o.power_line_hz = 60;
    V4l2Camera cam{dev, o};
    std::string err;
    ASSERT_TRUE(cam.open(err)) << err;
    EXPECT_EQ(dev.power_line_value, 2);
    EXPECT_EQ(dev.requested_fps, 30U);
    EXPECT_DOUBLE_EQ(cam.fps(), 15.0);
}

TEST(V4l2Camera, TooFewBuffersIsAnError)
{
    fake::FakeV4l2 dev;
    dev.max_buffers = 1;
    V4l2Camera cam{dev, opts()};
    std::string err;
    EXPECT_FALSE(cam.open(err));
    EXPECT_NE(err.find("至少要 2 个"), std::string::npos) << err;
    EXPECT_EQ(dev.munmaps, 0);
}

TEST(V4l2Camera, SequenceGapsAreCountedAsLostFrames)
{
    fake::FakeV4l2 dev;
    dev.script = {{1, false, 0}, {1, false, 0}, {3, false, 0}};  // 序号 0、1、4：丢了 2、3
    V4l2Camera cam{dev, opts()};
    open_and_start(cam);
    std::vector<std::uint64_t> seqs;
    for (int i = 0; i < 3; ++i) {
        auto r = cam.next(100);
        ASSERT_EQ(r.status, Status::frame);
        seqs.push_back(r.frame->sequence);
    }
    EXPECT_EQ(seqs, (std::vector<std::uint64_t>{0, 1, 4}));
    EXPECT_EQ(cam.stats().lost, 2U);
}

TEST(V4l2Camera, CorruptedFrameIsSkippedAndItsBufferRequeued)
{
    fake::FakeV4l2 dev;
    dev.script = {{1, false, 0}, {1, true, 0}, {1, false, 0}};  // 第 1 帧出错
    V4l2Camera cam{dev, opts()};
    open_and_start(cam);
    auto a = cam.next(100);
    auto b = cam.next(100);
    ASSERT_EQ(b.status, Status::frame) << b.error;
    EXPECT_EQ(a.frame->sequence, 0U);
    EXPECT_EQ(b.frame->sequence, 2U) << "出错的第 1 帧不交出去";
    EXPECT_EQ(cam.stats().corrupted, 1U);
    EXPECT_EQ(cam.stats().lost, 0U) << "出错的帧不算丢帧，不能重复计数";
    EXPECT_EQ(dev.queued(), 2U) << "4 个缓冲区：手里 2 个，出错那个已经还回去";
}

TEST(V4l2Camera, UnplugIsReportedAsError)
{
    fake::FakeV4l2 dev;
    V4l2Camera cam{dev, opts()};
    open_and_start(cam);
    ASSERT_EQ(cam.next(100).status, Status::frame);
    dev.unplug();
    auto r = cam.next(100);
    EXPECT_EQ(r.status, Status::error);
    EXPECT_NE(r.error.find("拔线"), std::string::npos) << r.error;
}

TEST(V4l2Camera, NonMonotonicTimestampFallsBackToNow)
{
    fake::FakeV4l2 dev;
    dev.monotonic = false;
    V4l2Camera cam{dev, opts()};
    open_and_start(cam);
    const auto before = std::chrono::steady_clock::now();
    auto r = cam.next(100);
    ASSERT_EQ(r.status, Status::frame);
    EXPECT_GE(r.frame->captured, before);
}

TEST(V4l2Camera, StopWaitsForHeldFramesBeforeUnmapping)
{
    fake::FakeV4l2 dev;
    V4l2Camera cam{dev, opts()};
    open_and_start(cam);
    auto r = cam.next(100);
    ASSERT_EQ(r.status, Status::frame);
    std::thread late([f = std::move(r.frame)]() mutable {
        std::this_thread::sleep_for(20ms);  // 另一个线程过一会儿才放手
        f.reset();
    });
    cam.stop(1000ms);
    late.join();
    EXPECT_EQ(dev.munmaps, 4) << "所有帧放手后才解除映射";
    EXPECT_EQ(dev.opens, dev.closes);
}

TEST(V4l2Camera, StopNeverUnmapsMemoryThatIsStillInUse)
{
    fake::FakeV4l2 dev;
    V4l2Camera cam{dev, opts()};
    open_and_start(cam);
    auto r = cam.next(100);
    ASSERT_EQ(r.status, Status::frame);
    cam.stop(10ms);  // 帧一直拿着不放
    EXPECT_EQ(dev.munmaps, 0) << "宁可泄漏映射，也不能让还拿着帧的人读到已释放的内存";
    EXPECT_EQ(r.frame->data[0], 0xFF);  // 还能安全读取（ASan 会检查）
    r.frame.reset();
    EXPECT_EQ(dev.qbufs_after_close, 0) << "设备关闭以后放手，不能再对 fd 做 QBUF";
}

TEST(V4l2Camera, CanBeReopenedAfterStop)
{
    fake::FakeV4l2 dev;
    V4l2Camera cam{dev, opts()};
    open_and_start(cam);
    ASSERT_EQ(cam.next(100).status, Status::frame);
    cam.stop();
    open_and_start(cam);
    EXPECT_EQ(cam.next(100).status, Status::frame);
    EXPECT_EQ(dev.opens, 2);
}

// 真实用法：采集线程取帧 → 有界队列 → 处理线程，处理慢会丢旧帧；被丢的帧也要把缓冲区还给驱动。
// 用 tsan 预设跑：放手时的 QBUF 发生在处理线程和采集线程两边。
TEST(V4l2Camera, FramesFlowThroughDroppingQueueAndAllBuffersComeBack)
{
    fake::FakeV4l2 dev;
    V4l2Camera cam{dev, opts()};
    open_and_start(cam);
    rksec::BoundedQueue<FrameRef> q{2};
    std::thread worker([&] {
        while (auto f = q.pop()) {
            std::this_thread::sleep_for(50us);
        }
    });
    int got = 0;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (got < 2000 && std::chrono::steady_clock::now() < deadline) {
        auto r = cam.next(10);
        ASSERT_NE(r.status, Status::error) << r.error;
        if (r.status == Status::frame) {
            q.push(std::move(r.frame));
            ++got;
        }
    }
    q.close();
    worker.join();
    EXPECT_EQ(got, 2000);
    EXPECT_GT(q.stats().dropped, 0U);
    EXPECT_EQ(dev.queued(), 4U) << "全部缓冲区都回到了驱动手里";
    cam.stop();
    EXPECT_EQ(dev.munmaps, 4);
}

TEST(V4l2Camera, BuffersAreExportedAsDmabufAndClosedOnStop)
{
    fake::FakeV4l2 dev;
    V4l2Camera cam{dev, opts()};
    open_and_start(cam);
    EXPECT_EQ(cam.dmabuf_count(), 4);
    EXPECT_EQ(dev.exported_open(), 4U);
    auto a = cam.next(100);
    auto b = cam.next(100);
    ASSERT_EQ(b.status, Status::frame);
    EXPECT_GE(a.frame->dmabuf_fd, 0);
    EXPECT_NE(a.frame->dmabuf_fd, b.frame->dmabuf_fd) << "每个缓冲区有自己的 DMA-BUF";
    a.frame.reset();
    b.frame.reset();
    cam.stop();
    EXPECT_EQ(dev.exported_open(), 0U);
}

TEST(V4l2Camera, CaptureStillWorksWhenDmabufExportIsUnsupported)
{
    fake::FakeV4l2 dev;
    dev.can_export = false;
    V4l2Camera cam{dev, opts()};
    open_and_start(cam);
    EXPECT_EQ(cam.dmabuf_count(), 0);
    auto r = cam.next(100);
    ASSERT_EQ(r.status, Status::frame);
    EXPECT_EQ(r.frame->dmabuf_fd, -1);
}

TEST(V4l2Camera, DmabufIsNotClosedWhileAFrameIsStillHeld)
{
    fake::FakeV4l2 dev;
    V4l2Camera cam{dev, opts()};
    open_and_start(cam);
    auto r = cam.next(100);
    ASSERT_EQ(r.status, Status::frame);
    cam.stop(10ms);
    EXPECT_EQ(dev.exported_open(), 4U) << "和映射一样：还有人拿着帧，就不关它的 fd";
}
