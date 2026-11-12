#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "capture/capture_service.hpp"
#include "core/config.hpp"

namespace rksec::app {

// 配置文件里的 [camera] → 采集服务的参数
capture::ServiceOptions service_options(const config::AppConfig &cfg);

// --capture-run 秒数：长时间运行采集服务（带断线重连），每 10 秒打印一行状态，
// 结束时报告帧率、延迟、丢帧、重连次数，以及内存（RSS）和打开文件数的变化——用来发现泄漏。
// Ctrl+C（SIGINT）或 SIGTERM 提前结束，同样输出报告。返回 0。
int capture_run(const config::AppConfig &cfg, int seconds);

// 直接从摄像头连续取 n 帧 MJPG 存进内存（各种 bench 用：先存帧再处理，耗时不受摄像头帧率影响）。
// 摄像头打不开或 6 秒没有新帧返回 false（已写日志）
bool grab_mjpeg(const config::AppConfig &cfg, int n, std::vector<std::vector<std::uint8_t>> &out);

// 边取边处理（第 7d 步摄像头登记）：每来一帧 MJPG 调一次 on_frame，它返回 false 就停；最多 max_frames 帧。
// 处理慢了驱动会丢帧，不影响结果。摄像头出错返回 false（已写日志）
bool for_each_mjpeg(const config::AppConfig &cfg, int max_frames,
                    const std::function<bool(const std::uint8_t *, std::size_t)> &on_frame);

}  // namespace rksec::app
