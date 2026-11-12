#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "capture/v4l2_camera.hpp"

// 采集服务：一个后台线程，负责“摄像头一直有画面”。
//
//   CaptureService svc{real_sys(), opts, [&](FrameRef f) { queue.push(std::move(f)); }};
//   svc.start();  …  svc.stop();
//
// 现场会发生的事，它都自己处理，不需要上层操心：
// - 开机时摄像头还没插 / 还没枚举出来：每隔一段时间重试打开（0.5 s 起，每次翻倍，最长 5 s）
// - 采集中拔线（D04：USB error -71 断开过）：取帧出错 → 关掉 → 按上面的节奏重试，插回去自动恢复
// - 设备还在但不出帧（卡住）：超过 stall_after 没有新帧 → 当作出错，关掉重开
// 日志不刷屏：状态变化时打一条；一直连不上时每分钟提醒一次。
//
// 线程：sink 在采集线程里被调用，要尽快返回（一般只是 push 进有界队列）。stats() 可以在任何线程调用。
namespace rksec::capture {

struct ServiceOptions {
    Options camera;
    std::chrono::milliseconds frame_timeout{200};  // 每次等帧最多多久（也决定 stop() 最慢多久生效）
    std::chrono::milliseconds stall_after{3000};  // 多久没有新帧算卡住（3b 实测最暗时帧间隔 140 ms）
    std::chrono::milliseconds retry_min{500};     // 重试间隔：从这里开始翻倍……
    std::chrono::milliseconds retry_max{5000};    // ……到这里封顶
};

struct ServiceStats {
    std::uint64_t frames = 0;         // 交给 sink 的总帧数
    std::uint64_t lost = 0;           // 驱动丢帧（序号空洞），各次连接累计
    std::uint64_t corrupted = 0;      // 坏帧，各次连接累计
    std::uint64_t sessions = 0;       // 成功开始采集的次数（第一次 + 重连）
    std::uint64_t open_failures = 0;  // 打开失败的次数
    std::uint64_t stalls = 0;         // 判定卡住、主动重开的次数
    bool streaming = false;           // 现在是否在出帧
    std::string last_error;           // 最近一次出错的原因
};

class CaptureService {
  public:
    using Sink = std::function<void(FrameRef)>;

    CaptureService(Sys &sys, ServiceOptions opts, Sink sink);
    ~CaptureService();  // 会调用 stop()

    CaptureService(const CaptureService &) = delete;
    CaptureService &operator=(const CaptureService &) = delete;

    void start();
    void stop();  // 最多等 frame_timeout + 摄像头 stop 的时间
    ServiceStats stats() const;

  private:
    void run();
    void stream(V4l2Camera &cam);
    bool sleep_unless_stopped(std::chrono::milliseconds d);  // 被 stop() 打断返回 false
    void set_error(const std::string &e);

    Sys &sys_;
    ServiceOptions opts_;
    Sink sink_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::mutex wake_mutex_;
    std::condition_variable wake_;

    mutable std::mutex stats_mutex_;
    ServiceStats stats_;
    Stats session_base_;  // 本次连接开始前，各次连接累计的 lost / corrupted
};

}  // namespace rksec::capture
