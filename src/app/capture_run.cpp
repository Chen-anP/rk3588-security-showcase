#include "app/capture_run.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "app/proc_stats.hpp"
#include "capture/capture_service.hpp"
#include "capture/v4l2_camera.hpp"
#include "core/bounded_queue.hpp"
#include "core/log.hpp"
#include "core/stage_timer.hpp"

namespace rksec::app {

namespace {

using Clock = std::chrono::steady_clock;

}  // namespace

capture::ServiceOptions service_options(const config::AppConfig &cfg)
{
    capture::ServiceOptions so;
    so.camera.device = cfg.camera.device;
    so.camera.width = cfg.camera.width;
    so.camera.height = cfg.camera.height;
    so.camera.fps = cfg.camera.fps;
    so.camera.power_line_hz = cfg.camera.power_line_hz;
    so.camera.buffers = cfg.camera.buffers;
    return so;
}

bool grab_mjpeg(const config::AppConfig &cfg, int n, std::vector<std::vector<std::uint8_t>> &out)
{
    return for_each_mjpeg(cfg, n, [&out](const std::uint8_t *data, std::size_t bytes) {
        out.emplace_back(data, data + bytes);
        return true;
    });
}

bool for_each_mjpeg(const config::AppConfig &cfg, int max_frames,
                    const std::function<bool(const std::uint8_t *, std::size_t)> &on_frame)
{
    capture::V4l2Camera cam{capture::real_sys(), service_options(cfg).camera};
    std::string err;
    if (!cam.open(err) || !cam.start(err)) {
        LOG_E("camera", "%s", err.c_str());
        return false;
    }
    int timeouts = 0;
    for (int got = 0; got < max_frames;) {
        auto r = cam.next(2000);
        if (r.status == capture::Status::error) {
            LOG_E("camera", "%s", r.error.c_str());
            return false;
        }
        if (r.status == capture::Status::timeout) {
            if (++timeouts >= 3) {
                LOG_E("camera", "6 秒没有新帧");
                return false;
            }
            continue;
        }
        timeouts = 0;
        ++got;
        if (!on_frame(r.frame->data, r.frame->bytes)) {
            break;
        }
    }
    return true;
}

int capture_run(const config::AppConfig &cfg, int seconds)
{
    install_stop_signals();

    const capture::ServiceOptions so = service_options(cfg);

    // 模拟真实结构：采集线程 → 有界队列（容量 2，满了丢旧帧）→ 处理线程
    BoundedQueue<FrameRef> queue{2};
    StageStats interval{"帧间隔", 4096};
    StageStats latency{"驱动时间戳→处理线程取到", 4096};
    std::atomic<std::uint64_t> consumed{0};
    std::thread consumer([&] {
        Clock::time_point prev{};
        while (auto f = queue.pop()) {
            const auto &frame = **f;
            latency.record(Clock::now() - frame.captured);
            const auto gap = frame.captured - prev;
            if (prev != Clock::time_point{} && gap > Clock::duration::zero() && gap < std::chrono::seconds(1)) {
                interval.record(gap);  // 跨越断线的间隔不计入
            }
            prev = frame.captured;
            ++consumed;
        }
    });

    capture::CaptureService svc{capture::real_sys(), so, [&queue](FrameRef f) { queue.push(std::move(f)); }};
    const auto t0 = Clock::now();
    svc.start();

    long rss_start = 0;
    long fds_start = 0;
    std::uint64_t frames_prev = 0;
    auto next_report = t0 + std::chrono::seconds(10);
    LOG_I("run", "开始长时间采集 %d 秒（Ctrl+C 提前结束）", seconds);

    while (!stop_requested() && Clock::now() - t0 < std::chrono::seconds(seconds)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (Clock::now() < next_report) {
            continue;
        }
        next_report += std::chrono::seconds(10);
        const auto s = svc.stats();
        const long rss = rss_kb();
        const long fds = open_fds();
        if (rss_start == 0 && s.streaming) {  // 第一次出帧后的状态作为基准
            rss_start = rss;
            fds_start = fds;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - t0).count();
        LOG_I(
            "run",
            "%5llds %s 帧 %llu（近 10 秒 %.1f fps）丢 %llu 坏 %llu 连接 %llu 卡住 %llu 队列丢弃 %llu RSS %ld KB fd %ld",
            static_cast<long long>(elapsed), s.streaming ? "出帧" : "等待", static_cast<unsigned long long>(s.frames),
            static_cast<double>(s.frames - frames_prev) / 10.0, static_cast<unsigned long long>(s.lost),
            static_cast<unsigned long long>(s.corrupted), static_cast<unsigned long long>(s.sessions),
            static_cast<unsigned long long>(s.stalls), static_cast<unsigned long long>(queue.stats().dropped), rss,
            fds);
        frames_prev = s.frames;
    }

    svc.stop();
    queue.close();
    consumer.join();

    const auto s = svc.stats();
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    const long rss_end = rss_kb();
    const long fds_end = open_fds();
    std::printf("运行       %.0f 秒%s\n", secs, stop_requested() ? "（被 Ctrl+C 提前结束）" : "");
    std::printf("帧数       采集 %llu，处理 %llu，队列丢弃 %llu，平均 %.2f fps\n",
                static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(consumed.load()),
                static_cast<unsigned long long>(queue.stats().dropped), static_cast<double>(s.frames) / secs);
    std::printf("%s\n", interval.report().c_str());
    std::printf("%s\n", latency.report().c_str());
    std::printf("丢帧       %llu；坏帧 %llu\n", static_cast<unsigned long long>(s.lost),
                static_cast<unsigned long long>(s.corrupted));
    std::printf("连接       %llu 次（重连 %llu 次）；打开失败 %llu 次；判定卡住 %llu 次\n",
                static_cast<unsigned long long>(s.sessions),
                static_cast<unsigned long long>(s.sessions > 0 ? s.sessions - 1 : 0),
                static_cast<unsigned long long>(s.open_failures), static_cast<unsigned long long>(s.stalls));
    if (rss_start > 0) {
        std::printf("内存 RSS   %ld KB → %ld KB（%+ld KB）\n", rss_start, rss_end, rss_end - rss_start);
        std::printf("打开文件   %ld → %ld（%+ld）\n", fds_start, fds_end, fds_end - fds_start);
    }
    return 0;
}

}  // namespace rksec::app
