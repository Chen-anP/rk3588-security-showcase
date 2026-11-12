#include "app/capture_test.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>

#include "capture/v4l2_camera.hpp"
#include "core/log.hpp"
#include "core/stage_timer.hpp"

namespace rksec::app {

namespace {

constexpr int kExitOk = 0;
constexpr int kExitDevice = 4;
constexpr int kMaxTimeouts = 3;  // 连续 3 次（共 6 秒）一帧都没有，就判定设备有问题

bool save_frame(const Frame &f, const std::string &path)
{
    std::FILE *fp = std::fopen(path.c_str(), "wb");
    if (fp == nullptr) {
        return false;
    }
    const bool ok = std::fwrite(f.data, 1, f.bytes, fp) == f.bytes;
    return std::fclose(fp) == 0 && ok;
}

}  // namespace

int capture_test(const config::AppConfig &cfg, int frames, const std::string &save_path)
{
    capture::Options o;
    o.device = cfg.camera.device;
    o.width = cfg.camera.width;
    o.height = cfg.camera.height;
    o.fps = cfg.camera.fps;
    o.power_line_hz = cfg.camera.power_line_hz;
    o.buffers = cfg.camera.buffers;

    capture::V4l2Camera cam{capture::real_sys(), o};
    std::string err;
    if (!cam.open(err) || !cam.start(err)) {
        LOG_E("camera", "%s", err.c_str());
        return kExitDevice;
    }

    StageStats interval{"帧间隔", 4096};
    StageStats latency{"驱动时间戳→程序取到", 4096};
    std::size_t min_bytes = std::numeric_limits<std::size_t>::max();
    std::size_t max_bytes = 0;
    std::chrono::steady_clock::time_point first;
    std::chrono::steady_clock::time_point prev;
    int got = 0;
    int timeouts = 0;
    bool saved = false;

    while (got < frames) {
        const auto r = cam.next(2000);
        if (r.status == capture::Status::error) {
            LOG_E("camera", "%s（已取到 %d 帧）", r.error.c_str(), got);
            return kExitDevice;
        }
        if (r.status == capture::Status::timeout) {
            if (++timeouts >= kMaxTimeouts) {
                LOG_E("camera", "连续 %d 秒没有新帧（已取到 %d 帧）", 2 * kMaxTimeouts, got);
                return kExitDevice;
            }
            continue;
        }
        timeouts = 0;
        const Frame &f = *r.frame;
        latency.record(std::chrono::steady_clock::now() - f.captured);
        if (got == 0) {
            first = f.captured;
        } else {
            interval.record(f.captured - prev);
        }
        prev = f.captured;
        min_bytes = std::min(min_bytes, f.bytes);
        max_bytes = std::max(max_bytes, f.bytes);
        if (!save_path.empty() && got == frames / 2) {
            saved = save_frame(f, save_path);
            if (!saved) {
                LOG_W("camera", "保存 %s 失败", save_path.c_str());
            }
        }
        ++got;
        // r.frame 在这里析构 → 缓冲区马上还给驱动
    }

    const double seconds = std::chrono::duration<double>(prev - first).count();
    const auto st = cam.stats();
    std::printf("设备       %s\n", o.device.c_str());
    std::printf("格式       MJPG %dx%d，驱动设定 %.2f fps，%d 个缓冲区\n", cam.width(), cam.height(), cam.fps(),
                o.buffers);
    std::printf("DMA-BUF    %d/%d 个缓冲区导出成功\n", cam.dmabuf_count(), o.buffers);
    std::printf("帧数       %d 帧，用时 %.2f 秒，实际帧率 %.2f fps\n", got, seconds,
                seconds > 0 ? (got - 1) / seconds : 0.0);
    std::printf("%s\n", interval.report().c_str());
    std::printf("%s\n", latency.report().c_str());
    std::printf("丢帧       %llu（按序号推算）；坏帧 %llu\n", static_cast<unsigned long long>(st.lost),
                static_cast<unsigned long long>(st.corrupted));
    std::printf("每帧大小   %zu ～ %zu 字节\n", min_bytes, max_bytes);
    if (saved) {
        std::printf("已保存     %s（第 %d 帧）\n", save_path.c_str(), frames / 2 + 1);
    }
    cam.stop();
    return kExitOk;
}

}  // namespace rksec::app
