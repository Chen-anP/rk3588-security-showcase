#include "capture/v4l2_camera.hpp"

#include <fcntl.h>
#include <linux/videodev2.h>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <thread>
#include <utility>

#include "core/log.hpp"

namespace rksec::capture {

namespace {

std::string errstr(int neg_errno)
{
    return std::strerror(-neg_errno);
}

std::string fourcc(std::uint32_t f)
{
    std::string s(4, ' ');
    for (std::size_t i = 0; i < 4; ++i) {
        s[i] = static_cast<char>((f >> (8 * i)) & 0xFFU);
    }
    return s;
}

}  // namespace

int power_line_menu_value(int hz)
{
    switch (hz) {
    case 0:
        return V4L2_CID_POWER_LINE_FREQUENCY_DISABLED;
    case 50:
        return V4L2_CID_POWER_LINE_FREQUENCY_50HZ;
    case 60:
        return V4L2_CID_POWER_LINE_FREQUENCY_60HZ;
    default:
        return -1;
    }
}

V4l2Camera::V4l2Camera(Sys &sys, Options opts) : sys_(sys), opts_(std::move(opts)), shared_(std::make_shared<Shared>())
{
    shared_->sys = &sys_;
}

V4l2Camera::~V4l2Camera()
{
    stop();
}

bool V4l2Camera::open(std::string &err)
{
    stop();
    const std::string &dev = opts_.device;

    // O_NONBLOCK：DQBUF 不会卡住，等待统一交给 poll，才能设超时
    const int fd = sys_.open(dev.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        err = "打不开 " + dev + ": " + errstr(fd);
        return false;
    }
    shared_->fd = fd;

    v4l2_capability cap{};
    if (const int r = sys_.ioctl(fd, VIDIOC_QUERYCAP, &cap); r < 0) {
        err = dev + " 不是 V4L2 设备: " + errstr(r);
        close_device_after_release(std::chrono::milliseconds(0));
        return false;
    }
    const std::uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0 ? cap.device_caps : cap.capabilities;
    if ((caps & V4L2_CAP_VIDEO_CAPTURE) == 0 || (caps & V4L2_CAP_STREAMING) == 0) {
        err = dev + " 不支持视频采集或流式 I/O（可能打开的是同一摄像头的元数据节点 index1）";
        close_device_after_release(std::chrono::milliseconds(0));
        return false;
    }

    // 格式：驱动不支持时会悄悄改成它支持的，所以设完必须核对
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = static_cast<std::uint32_t>(opts_.width);
    fmt.fmt.pix.height = static_cast<std::uint32_t>(opts_.height);
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (const int r = sys_.ioctl(fd, VIDIOC_S_FMT, &fmt); r < 0) {
        err = "设置格式失败: " + errstr(r);
        close_device_after_release(std::chrono::milliseconds(0));
        return false;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG || fmt.fmt.pix.width != static_cast<std::uint32_t>(opts_.width) ||
        fmt.fmt.pix.height != static_cast<std::uint32_t>(opts_.height)) {
        err = "摄像头不支持 MJPG " + std::to_string(opts_.width) + "x" + std::to_string(opts_.height) +
              "，驱动改成了 " + fourcc(fmt.fmt.pix.pixelformat) + " " + std::to_string(fmt.fmt.pix.width) + "x" +
              std::to_string(fmt.fmt.pix.height) + "（v4l2-ctl --list-formats-ext 查看支持的组合）";
        close_device_after_release(std::chrono::milliseconds(0));
        return false;
    }
    width_ = static_cast<int>(fmt.fmt.pix.width);
    height_ = static_cast<int>(fmt.fmt.pix.height);
    buffer_size_ = fmt.fmt.pix.sizeimage;

    // 帧率：驱动取最接近的档位，读回实际值。光线暗时实际帧率还会更低（D03），这里设的是上限
    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = static_cast<std::uint32_t>(opts_.fps);
    if (const int r = sys_.ioctl(fd, VIDIOC_S_PARM, &parm); r < 0) {
        LOG_W("camera", "设置帧率失败（%s），用驱动默认值", errstr(r).c_str());
    }
    const auto &tpf = parm.parm.capture.timeperframe;
    fps_ = tpf.numerator != 0 ? static_cast<double>(tpf.denominator) / tpf.numerator : 0.0;
    if (fps_ > 0 && std::lround(fps_) != opts_.fps) {
        LOG_W("camera", "请求 %d fps，驱动给的是 %.2f fps", opts_.fps, fps_);
    }

    // 防闪烁（D03：50 Hz 交流电下的日光灯，不开会有明暗条纹；开了光线暗时帧率会降）
    v4l2_control ctrl{};
    ctrl.id = V4L2_CID_POWER_LINE_FREQUENCY;
    ctrl.value = power_line_menu_value(opts_.power_line_hz);
    if (const int r = sys_.ioctl(fd, VIDIOC_S_CTRL, &ctrl); r < 0) {
        LOG_W("camera", "设置防闪烁 %d Hz 失败（%s）", opts_.power_line_hz, errstr(r).c_str());
    }

    // 申请缓冲区。驱动可能给得比要的少
    v4l2_requestbuffers req{};
    req.count = static_cast<std::uint32_t>(opts_.buffers);
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (const int r = sys_.ioctl(fd, VIDIOC_REQBUFS, &req); r < 0) {
        err = "申请缓冲区失败: " + errstr(r);
        close_device_after_release(std::chrono::milliseconds(0));
        return false;
    }
    if (req.count < 2) {
        err = "驱动只给了 " + std::to_string(req.count) + " 个缓冲区，至少要 2 个";
        close_device_after_release(std::chrono::milliseconds(0));
        return false;
    }
    if (req.count != static_cast<std::uint32_t>(opts_.buffers)) {
        LOG_W("camera", "请求 %d 个缓冲区，驱动给了 %u 个", opts_.buffers, req.count);
    }

    for (std::uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (const int r = sys_.ioctl(fd, VIDIOC_QUERYBUF, &b); r < 0) {
            err = "查询缓冲区 " + std::to_string(i) + " 失败: " + errstr(r);
            close_device_after_release(std::chrono::milliseconds(0));
            return false;
        }
        int merr = 0;
        void *addr = sys_.mmap(b.length, fd, b.m.offset, merr);
        if (addr == nullptr) {
            err = "映射缓冲区 " + std::to_string(i) + " 失败: " + errstr(merr);
            close_device_after_release(std::chrono::milliseconds(0));
            return false;
        }
        buffers_.push_back({addr, b.length, -1});

        // 同一块内存再导出一个 DMA-BUF fd：以后硬件解码器、RGA 可以直接用它，不经过 CPU 拷贝。
        // 导出失败不算错误（不是所有驱动都支持），只是这个缓冲区没有 dmabuf_fd
        v4l2_exportbuffer exp{};
        exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        exp.index = i;
        exp.flags = O_CLOEXEC | O_RDONLY;
        if (const int r = sys_.ioctl(fd, VIDIOC_EXPBUF, &exp); r == 0) {
            buffers_.back().dmabuf_fd = exp.fd;
        } else if (i == 0) {
            LOG_W("camera", "导出 DMA-BUF 失败（%s），只用 mmap", errstr(r).c_str());
        }
    }

    // 帧句柄：最后一个使用者放手时把缓冲区还给驱动（QBUF）。回调只抓 shared_，不抓 this
    std::shared_ptr<Shared> shared = shared_;
    pool_ = std::make_unique<FramePool>(static_cast<int>(req.count), [shared](int index) {
        const int f = shared->fd.load();
        if (!shared->streaming.load() || f < 0) {
            return;
        }
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = static_cast<std::uint32_t>(index);
        if (shared->sys->ioctl(f, VIDIOC_QBUF, &b) < 0) {
            ++shared->requeue_failed;
        }
    });

    stats_ = Stats{};
    have_last_seq_ = false;
    LOG_I("camera", "打开 %s：MJPG %dx%d，%.2f fps，%zu 个缓冲区 × %zu 字节，DMA-BUF %d/%zu", dev.c_str(), width_,
          height_, fps_, buffers_.size(), buffer_size_, dmabuf_count(), buffers_.size());
    return true;
}

bool V4l2Camera::queue_buffer(int index, std::string &err)
{
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = static_cast<std::uint32_t>(index);
    if (const int r = sys_.ioctl(shared_->fd, VIDIOC_QBUF, &b); r < 0) {
        err = "QBUF " + std::to_string(index) + " 失败: " + errstr(r);
        return false;
    }
    return true;
}

bool V4l2Camera::start(std::string &err)
{
    if (shared_->fd < 0 || !pool_) {
        err = "还没有 open";
        return false;
    }
    for (int i = 0; i < static_cast<int>(buffers_.size()); ++i) {
        if (!queue_buffer(i, err)) {
            return false;
        }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (const int r = sys_.ioctl(shared_->fd, VIDIOC_STREAMON, &type); r < 0) {
        err = "开始采集失败: " + errstr(r);
        return false;
    }
    shared_->streaming = true;
    return true;
}

Result V4l2Camera::next(int timeout_ms)
{
    Result res;
    if (!shared_->streaming) {
        res.status = Status::error;
        res.error = "没有在采集";
        return res;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        const int pr = sys_.poll_readable(shared_->fd, left.count() > 0 ? static_cast<int>(left.count()) : 0);
        if (pr == 0) {
            return res;  // 超时
        }
        if (pr < 0) {
            res.status = Status::error;
            res.error = "等待帧时设备出错（拔线？）: " + errstr(pr);
            return res;
        }

        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (const int r = sys_.ioctl(shared_->fd, VIDIOC_DQBUF, &b); r < 0) {
            if (r == -EAGAIN) {
                continue;  // poll 说可读但没取到，再等
            }
            res.status = Status::error;
            res.error = "取帧失败（拔线？）: " + errstr(r);
            return res;
        }

        // 用序号发现驱动丢掉的帧（nodrop=0 时坏帧被驱动直接丢弃，只在序号上留下空洞）
        if (have_last_seq_) {
            const std::uint32_t gap = b.sequence - last_seq_ - 1U;  // 无符号减法，序号回绕也对
            if (gap < 1000000U) {
                stats_.lost += gap;
            }
        }
        have_last_seq_ = true;
        last_seq_ = b.sequence;

        if ((b.flags & V4L2_BUF_FLAG_ERROR) != 0 || b.bytesused == 0) {
            ++stats_.corrupted;
            std::string qerr;
            if (!queue_buffer(static_cast<int>(b.index), qerr)) {
                res.status = Status::error;
                res.error = qerr;
                return res;
            }
            continue;  // 坏帧不交出去，继续等下一帧
        }

        // 时间戳：驱动给的是 CLOCK_MONOTONIC（3a 实测），和 steady_clock 是同一个时钟
        std::chrono::steady_clock::time_point ts;
        if ((b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
            ts = std::chrono::steady_clock::time_point(std::chrono::seconds(b.timestamp.tv_sec) +
                                                       std::chrono::microseconds(b.timestamp.tv_usec));
        } else {
            ts = std::chrono::steady_clock::now();
            if (!warned_clock_) {
                LOG_W("camera", "驱动时间戳不是 CLOCK_MONOTONIC（flags=0x%x），改用取帧时刻", b.flags);
                warned_clock_ = true;
            }
        }

        const auto &buf = buffers_[b.index];
        res.frame = pool_->acquire(static_cast<int>(b.index), b.sequence, ts,
                                   static_cast<const std::uint8_t *>(buf.addr), b.bytesused, buf.dmabuf_fd);
        if (!res.frame) {
            res.status = Status::error;
            res.error = "驱动交回了一个还在使用中的缓冲区 " + std::to_string(b.index);
            return res;
        }
        ++stats_.frames;
        res.status = Status::frame;
        return res;
    }
}

void V4l2Camera::stop(std::chrono::milliseconds release_wait)
{
    if (shared_->streaming.exchange(false)) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        sys_.ioctl(shared_->fd, VIDIOC_STREAMOFF, &type);
    }
    close_device_after_release(release_wait);
}

void V4l2Camera::close_device_after_release(std::chrono::milliseconds release_wait)
{
    bool can_unmap = true;
    if (pool_) {
        const auto deadline = std::chrono::steady_clock::now() + release_wait;
        while (pool_->in_use() > 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (const int held = pool_->in_use(); held > 0) {
            LOG_E("camera", "停止时还有 %d 帧没被放手，为了不让它们读到已释放的内存，不解除映射（泄漏 %zu 字节）", held,
                  buffers_.size() * buffer_size_);
            can_unmap = false;
        }
        pool_.reset();  // 析构会等正在执行的回调结束；之后不会再有 QBUF
    }
    if (can_unmap) {
        for (auto &b : buffers_) {
            sys_.munmap(b.addr, b.length);
            if (b.dmabuf_fd >= 0) {
                sys_.close(b.dmabuf_fd);  // 和映射一样：帧都放手了才关，否则拿着帧的人手里的 fd 会失效
            }
        }
        if (shared_->fd >= 0 && !buffers_.empty()) {
            v4l2_requestbuffers req{};
            req.count = 0;  // 把缓冲区还给驱动
            req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            req.memory = V4L2_MEMORY_MMAP;
            sys_.ioctl(shared_->fd, VIDIOC_REQBUFS, &req);
        }
    }
    buffers_.clear();
    close_device();
}

void V4l2Camera::close_device()
{
    const int fd = shared_->fd.exchange(-1);
    if (fd >= 0) {
        sys_.close(fd);  // 映射在 close 之后仍然有效，所以泄漏映射的情况下关 fd 也安全
    }
}

int V4l2Camera::dmabuf_count() const
{
    int n = 0;
    for (const auto &b : buffers_) {
        n += b.dmabuf_fd >= 0 ? 1 : 0;
    }
    return n;
}

Stats V4l2Camera::stats() const
{
    Stats s = stats_;
    s.requeue_failed = shared_->requeue_failed.load();
    return s;
}

}  // namespace rksec::capture
