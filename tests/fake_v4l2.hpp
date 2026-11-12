#pragma once

// 假的 V4L2 摄像头：实现 capture::Sys，在内存里模拟 uvcvideo 驱动的行为。
// 单元测试用它制造真机上很难按需出现的情况：驱动改格式、丢帧、坏帧、拔线、只给 1 个缓冲区……
// 只模拟采集模块用到的那几个 ioctl；行为以 3a 摸底时真机的表现为准。

#include <linux/videodev2.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "capture/sys.hpp"

namespace fake {

// 一帧的“剧本”：驱动下一次 DQBUF 交出什么
struct ScriptedFrame {
    std::uint32_t seq_step = 1;  // 序号比上一帧大多少（>1 表示驱动丢了帧）
    bool error = false;          // 驱动标记为出错
    std::uint32_t bytes = 0;     // 0 表示用默认值
};

class FakeV4l2 final : public rksec::capture::Sys {
  public:
    // ---- 可以在测试里改的“设备特性” ----
    bool present = true;  // 设备节点存在（开始采集前设置；采集中要变用 unplug() / replug()）
    std::uint32_t caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes{{1280, 720}, {640, 480}};  // 支持的 MJPG 分辨率
    std::uint32_t fps_given = 0;                                                          // 0 = 给多少就是多少
    std::uint32_t max_buffers = 8;
    std::uint32_t sizeimage = 2764800;  // 1280*720*3：3b 实测这个摄像头 MJPG 720p 的 sizeimage
    bool monotonic = true;
    bool can_export = true;            // 支持 VIDIOC_EXPBUF
    std::deque<ScriptedFrame> script;  // 用完以后按默认（序号 +1、不出错）

    // ---- 测试里读的“发生了什么” ----
    int opens = 0, closes = 0, munmaps = 0, qbufs_after_close = 0;
    std::int32_t power_line_value = -100;
    std::uint32_t requested_fps = 0;

    // 拔线：已经打开的 fd 之后的操作都失败，设备节点消失（open 返回 ENOENT），直到 replug()
    void unplug()
    {
        const std::lock_guard<std::mutex> lock(m_);
        unplugged_ = true;
        present = false;
    }
    void replug()
    {
        const std::lock_guard<std::mutex> lock(m_);
        present = true;
    }
    // 卡住：设备还在，但不再出帧（poll 一直超时）——有些 USB 摄像头会这样
    void set_stalled(bool on)
    {
        const std::lock_guard<std::mutex> lock(m_);
        stalled_ = on;
    }
    int open_count() const
    {
        const std::lock_guard<std::mutex> lock(m_);
        return opens;
    }
    std::size_t exported_open() const  // 导出了还没关的 DMA-BUF fd 个数
    {
        const std::lock_guard<std::mutex> lock(m_);
        return exported_.size();
    }
    std::size_t queued() const
    {
        const std::lock_guard<std::mutex> lock(m_);
        return queued_.size();
    }

    // ---- Sys ----
    int open(const char * /*path*/, int /*flags*/) override
    {
        const std::lock_guard<std::mutex> lock(m_);
        if (!present) {
            return -ENOENT;
        }
        ++opens;
        open_ = true;
        unplugged_ = false;
        return kFd;
    }

    int close(int fd) override
    {
        const std::lock_guard<std::mutex> lock(m_);
        if (exported_.erase(fd) > 0) {
            return 0;
        }
        if (fd == kFd) {
            ++closes;
            open_ = false;
            streaming_ = false;
            queued_.clear();
        }
        return 0;
    }

    int ioctl(int fd, unsigned long req, void *arg) override
    {
        const std::lock_guard<std::mutex> lock(m_);
        if (fd != kFd || !open_) {
            if (req == VIDIOC_QBUF) {
                ++qbufs_after_close;
            }
            return -EBADF;
        }
        if (unplugged_ && req != VIDIOC_STREAMOFF && req != VIDIOC_REQBUFS) {
            return -ENODEV;
        }
        switch (req) {
        case VIDIOC_QUERYCAP: {
            auto *c = static_cast<v4l2_capability *>(arg);
            c->capabilities = caps | V4L2_CAP_DEVICE_CAPS;
            c->device_caps = caps;
            return 0;
        }
        case VIDIOC_S_FMT: {
            auto &p = static_cast<v4l2_format *>(arg)->fmt.pix;
            bool ok = false;
            for (const auto &[w, h] : sizes) {
                ok = ok || (w == p.width && h == p.height);
            }
            if (!ok) {  // 和真驱动一样：不报错，悄悄改成支持的
                p.width = sizes.front().first;
                p.height = sizes.front().second;
            }
            p.pixelformat = V4L2_PIX_FMT_MJPEG;
            p.sizeimage = sizeimage;
            return 0;
        }
        case VIDIOC_S_PARM: {
            auto &t = static_cast<v4l2_streamparm *>(arg)->parm.capture.timeperframe;
            requested_fps = t.denominator;
            if (fps_given != 0) {
                t.numerator = 1;
                t.denominator = fps_given;
            }
            return 0;
        }
        case VIDIOC_S_CTRL: {
            auto *c = static_cast<v4l2_control *>(arg);
            if (c->id == V4L2_CID_POWER_LINE_FREQUENCY) {
                power_line_value = c->value;
            }
            return 0;
        }
        case VIDIOC_REQBUFS: {
            auto *r = static_cast<v4l2_requestbuffers *>(arg);
            r->count = std::min(r->count, max_buffers);
            nbuf_ = r->count;
            return 0;
        }
        case VIDIOC_QUERYBUF: {
            auto *b = static_cast<v4l2_buffer *>(arg);
            if (b->index >= nbuf_) {
                return -EINVAL;
            }
            b->length = sizeimage;
            b->m.offset = b->index * 4096U;
            return 0;
        }
        case VIDIOC_QBUF: {
            auto *b = static_cast<v4l2_buffer *>(arg);
            if (b->index >= nbuf_ || in_queue(b->index)) {
                return -EINVAL;
            }
            queued_.push_back(b->index);
            return 0;
        }
        case VIDIOC_EXPBUF: {
            auto *e = static_cast<v4l2_exportbuffer *>(arg);
            if (!can_export || e->index >= nbuf_) {
                return -ENOTTY;
            }
            e->fd = kDmabufBase + static_cast<int>(e->index) + 10 * opens;  // 每次打开给不同的号
            exported_.insert(e->fd);
            return 0;
        }
        case VIDIOC_DQBUF:
            return dqbuf(*static_cast<v4l2_buffer *>(arg));
        case VIDIOC_STREAMON:
            streaming_ = true;
            return 0;
        case VIDIOC_STREAMOFF:
            streaming_ = false;
            queued_.clear();  // 真驱动 STREAMOFF 后所有缓冲区回到“未入队”状态
            return 0;
        default:
            return -ENOTTY;
        }
    }

    void *mmap(std::size_t length, int /*fd*/, std::int64_t offset, int &err) override
    {
        const std::lock_guard<std::mutex> lock(m_);
        auto &mem = mem_[offset];
        mem.assign(length, 0);
        err = 0;
        return mem.data();
    }

    int munmap(void *addr, std::size_t /*length*/) override
    {
        const std::lock_guard<std::mutex> lock(m_);
        ++munmaps;
        for (auto it = mem_.begin(); it != mem_.end(); ++it) {
            if (it->second.data() == addr) {
                mem_.erase(it);  // 真的释放：之后还有人读就是 use-after-free，ASan 会报
                break;
            }
        }
        return 0;
    }

    int poll_readable(int /*fd*/, int timeout_ms) override
    {
        {
            const std::lock_guard<std::mutex> lock(m_);
            if (unplugged_) {
                return -ENODEV;
            }
            if (!stalled_ && streaming_ && !queued_.empty()) {
                return 1;
            }
        }
        // 没有可取的帧：只象征性地等 1 毫秒就“超时”，测试不用真等那么久，也不会空转占满 CPU
        if (timeout_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 0;
    }

  private:
    static constexpr int kFd = 42;
    static constexpr int kDmabufBase = 100;

    bool in_queue(std::uint32_t i) const
    {
        for (const auto q : queued_) {
            if (q == i) {
                return true;
            }
        }
        return false;
    }

    int dqbuf(v4l2_buffer &b)
    {
        if (!streaming_ || queued_.empty()) {
            return -EAGAIN;
        }
        ScriptedFrame f;
        if (!script.empty()) {
            f = script.front();
            script.pop_front();
        }
        b.index = queued_.front();
        queued_.pop_front();
        seq_ = produced_ == 0 ? 0 : seq_ + f.seq_step;
        b.sequence = seq_;
        b.bytesused = f.bytes != 0 ? f.bytes : 1000U + seq_;
        b.flags = (monotonic ? V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC : 0U) | (f.error ? V4L2_BUF_FLAG_ERROR : 0U);
        // 时间戳：从 100 秒开始，每帧 33.333 ms
        const auto us = 100'000'000LL + static_cast<long long>(seq_) * 33'333LL;
        b.timestamp.tv_sec = us / 1'000'000;
        b.timestamp.tv_usec = us % 1'000'000;
        // 帧数据：JPEG 开头 FF D8，后面写上序号，测试可以核对拿到的是哪一帧
        auto &mem = mem_.at(static_cast<std::int64_t>(b.index) * 4096);
        mem[0] = 0xFF;
        mem[1] = 0xD8;
        mem[2] = static_cast<std::uint8_t>(seq_ & 0xFFU);
        ++produced_;
        return 0;
    }

    mutable std::mutex m_;
    bool open_ = false;
    bool unplugged_ = false;
    bool stalled_ = false;
    std::set<int> exported_;
    bool streaming_ = false;
    std::uint32_t nbuf_ = 0;
    std::deque<std::uint32_t> queued_;
    std::map<std::int64_t, std::vector<std::uint8_t>> mem_;
    std::uint32_t seq_ = 0;
    std::uint64_t produced_ = 0;
};

}  // namespace fake
