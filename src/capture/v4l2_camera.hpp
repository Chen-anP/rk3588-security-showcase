#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "capture/sys.hpp"
#include "core/frame_pool.hpp"

// USB 摄像头采集（V4L2，MJPG，mmap）。
//
//   V4l2Camera cam{real_sys(), opts};
//   if (!cam.open(err) || !cam.start(err)) { … }
//   while (…) {
//       auto r = cam.next(1000);              // 最多等 1 秒
//       if (r.status == Status::frame) { 用 r.frame（FrameRef）… }   // 最后一个使用者放手时自动 QBUF
//   }
//   cam.stop();
//
// - 每个缓冲区同时导出为 DMA-BUF（VIDIOC_EXPBUF），Frame::dmabuf_fd；给第 5 步的硬件解码 / RGA 零拷贝用
//
// 第 3 步摸底的事实（3a）：MJPG 帧自带哈夫曼表，可以直接存成 .jpg；驱动时间戳是 CLOCK_MONOTONIC、
// 取在帧开始（ts-src-soe）；uvcvideo nodrop=0，坏帧由驱动丢掉，表现为序号不连续。
//
// - 不是线程安全的：open / start / next / stop 由同一个线程调用；
//   FrameRef 可以交给任何线程，放手时的 QBUF 可以发生在任何线程（V4L2 允许）
// - stop() 会等所有帧都被放手才解除内存映射；等不到就宁可泄漏映射也不解除——解除了，还拿着帧的线程一读就崩
namespace rksec::capture {

struct Options {
    std::string device;  // /dev/v4l/by-id/…（D04）
    int width = 1280;
    int height = 720;
    int fps = 30;
    int power_line_hz = 50;  // 50 / 60 / 0（关闭）
    int buffers = 4;         // 驱动缓冲区个数
};

enum class Status : std::uint8_t {
    frame,    // 拿到一帧
    timeout,  // 等待超时（没有新帧）
    error,    // 设备出错（拔线等），需要重新 open
};

struct Result {
    Status status = Status::timeout;
    FrameRef frame;
    std::string error;
};

struct Stats {
    std::uint64_t frames = 0;          // 交出去的帧数
    std::uint64_t lost = 0;            // 根据序号推算驱动丢掉的帧数
    std::uint64_t corrupted = 0;       // 驱动标记为出错、被跳过的帧数
    std::uint64_t requeue_failed = 0;  // 放手时 QBUF 失败的次数（停流以后的属正常）
};

class V4l2Camera {
  public:
    V4l2Camera(Sys &sys, Options opts);
    ~V4l2Camera();

    V4l2Camera(const V4l2Camera &) = delete;
    V4l2Camera &operator=(const V4l2Camera &) = delete;

    // 打开设备、设置格式 / 帧率 / 防闪烁、申请并映射缓冲区。失败返回 false，err 是原因
    bool open(std::string &err);
    // 把缓冲区全部交给驱动并开始采集
    bool start(std::string &err);
    // 等一帧，最多 timeout_ms 毫秒
    Result next(int timeout_ms);
    // 停止采集；最多等 release_wait 让所有帧被放手，然后解除映射、关闭设备。可以重复调用
    void stop(std::chrono::milliseconds release_wait = std::chrono::milliseconds(1000));

    // 驱动实际给的参数（open 之后有效）
    int width() const { return width_; }
    int height() const { return height_; }
    double fps() const { return fps_; }
    std::size_t buffer_size() const { return buffer_size_; }
    // 成功导出为 DMA-BUF 的缓冲区个数（open 之后有效）。驱动不支持时为 0，不影响 mmap 采集
    int dmabuf_count() const;

    Stats stats() const;

  private:
    struct Buffer {
        void *addr = nullptr;
        std::size_t length = 0;
        int dmabuf_fd = -1;
    };
    // 放手时 QBUF 用到的状态。单独放在 shared_ptr 里：回调可能在别的线程、在 stop 之后才执行
    struct Shared {
        Sys *sys = nullptr;
        std::atomic<int> fd{-1};
        std::atomic<bool> streaming{false};
        std::atomic<std::uint64_t> requeue_failed{0};
    };

    bool queue_buffer(int index, std::string &err);
    void close_device_after_release(std::chrono::milliseconds release_wait);  // 等帧放手 → 解除映射 → 关设备
    void close_device();

    Sys &sys_;
    Options opts_;
    std::shared_ptr<Shared> shared_;
    std::vector<Buffer> buffers_;
    std::unique_ptr<FramePool> pool_;
    int width_ = 0;
    int height_ = 0;
    double fps_ = 0;
    std::size_t buffer_size_ = 0;
    bool have_last_seq_ = false;
    std::uint32_t last_seq_ = 0;
    bool warned_clock_ = false;
    Stats stats_;
};

// V4L2 防闪烁菜单的值：50 → 1，60 → 2，0 → 0；其他值返回 -1
int power_line_menu_value(int hz);

}  // namespace rksec::capture
