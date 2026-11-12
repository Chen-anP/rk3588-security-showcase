#include "capture/capture_service.hpp"

#include <pthread.h>

#include <algorithm>
#include <utility>

#include "core/log.hpp"

namespace rksec::capture {

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kRemindEvery = std::chrono::seconds(60);  // 一直连不上时，多久提醒一次

}  // namespace

CaptureService::CaptureService(Sys &sys, ServiceOptions opts, Sink sink)
    : sys_(sys), opts_(std::move(opts)), sink_(std::move(sink))
{
}

CaptureService::~CaptureService()
{
    stop();
}

void CaptureService::start()
{
    if (thread_.joinable()) {
        return;
    }
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void CaptureService::stop()
{
    {
        // 在锁里改标志再通知：避免采集线程刚检查完标志、还没开始等，通知就发出去了（丢失唤醒）
        const std::lock_guard<std::mutex> lock(wake_mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

ServiceStats CaptureService::stats() const
{
    const std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

bool CaptureService::sleep_unless_stopped(std::chrono::milliseconds d)
{
    std::unique_lock<std::mutex> lock(wake_mutex_);
    return !wake_.wait_for(lock, d, [this] { return stopping_.load(); });
}

void CaptureService::set_error(const std::string &e)
{
    const std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.last_error = e;
    stats_.streaming = false;
}

void CaptureService::run()
{
    pthread_setname_np(pthread_self(), "capture");  // top -H、gdb 里能看出是哪个线程
    V4l2Camera cam{sys_, opts_.camera};
    auto retry = opts_.retry_min;
    bool failing = false;             // 正处在“连不上”的状态
    Clock::time_point failing_since;  // 从什么时候开始连不上
    Clock::time_point last_reminder;

    while (!stopping_) {
        std::string err;
        if (!cam.open(err) || !cam.start(err)) {
            cam.stop(std::chrono::milliseconds(0));
            {
                const std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.open_failures;
                stats_.last_error = err;
                stats_.streaming = false;
            }
            const auto now = Clock::now();
            if (!failing) {  // 状态变化：只在第一次打一条警告
                LOG_W("camera", "摄像头不可用，%lld ms 后重试：%s", static_cast<long long>(retry.count()), err.c_str());
                failing = true;
                failing_since = now;
                last_reminder = now;
            } else if (now - last_reminder >= kRemindEvery) {
                const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now - failing_since).count();
                LOG_W("camera", "仍在等待摄像头（已 %lld 秒）：%s", static_cast<long long>(secs), err.c_str());
                last_reminder = now;
            } else {
                LOG_D("camera", "重试打开失败：%s", err.c_str());
            }
            if (!sleep_unless_stopped(retry)) {
                break;
            }
            retry = std::min(retry * 2, opts_.retry_max);
            continue;
        }

        retry = opts_.retry_min;
        std::uint64_t session = 0;
        {
            const std::lock_guard<std::mutex> lock(stats_mutex_);
            session = ++stats_.sessions;
            stats_.streaming = true;
        }
        if (failing) {
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - failing_since).count();
            LOG_I("camera", "摄像头已恢复（断开约 %lld 秒，第 %llu 次连接）", static_cast<long long>(secs),
                  static_cast<unsigned long long>(session));
            failing = false;
        }

        stream(cam);
        cam.stop();

        // 本次连接的丢帧、坏帧并入累计
        const auto st = cam.stats();
        session_base_.lost += st.lost;
        session_base_.corrupted += st.corrupted;

        if (!stopping_ && !failing) {
            failing = true;  // 出帧中断了，进入“连不上”状态，下一轮开始重试
            failing_since = Clock::now();
            last_reminder = failing_since;
        }
    }
    {
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.streaming = false;
    }
}

void CaptureService::stream(V4l2Camera &cam)
{
    const int timeout_ms = static_cast<int>(opts_.frame_timeout.count());
    auto last_frame = Clock::now();
    while (!stopping_) {
        Result r = cam.next(timeout_ms);
        if (r.status == Status::frame) {
            last_frame = Clock::now();
            const auto st = cam.stats();
            {
                const std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.frames;
                stats_.lost = session_base_.lost + st.lost;
                stats_.corrupted = session_base_.corrupted + st.corrupted;
            }
            sink_(std::move(r.frame));
            continue;
        }
        if (r.status == Status::error) {
            LOG_W("camera", "采集中断：%s", r.error.c_str());
            set_error(r.error);
            return;
        }
        // 超时：偶尔一次正常（暗处帧间隔长），太久没有新帧才算卡住
        if (Clock::now() - last_frame >= opts_.stall_after) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(opts_.stall_after).count();
            LOG_W("camera", "超过 %lld ms 没有新帧，判定卡住，重新打开", static_cast<long long>(ms));
            {
                const std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.stalls;
            }
            set_error("超过 " + std::to_string(ms) + " ms 没有新帧");
            return;
        }
    }
}

}  // namespace rksec::capture
