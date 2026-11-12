#pragma once

#include <string>

#include "core/config.hpp"

namespace rksec::app {

// --encode-run 秒数 输出文件（第 8a 步）：摄像头 → MPP 解码 → MPP 硬件 H.264 编码，码流原样写进文件。
// 不接屏幕、不做检测，只测编码这一段：编码耗时、实际码率、关键帧间隔、CPU、内存。
// 输出的 .h264 是裸 Annex B 码流，电脑上 ffplay -f h264 文件 或 VLC 能直接播（没有时间信息，按默认帧率播）。
// Ctrl+C 提前结束，同样输出报告。返回 0；没有硬件支持返回 2，设备出错返回 4
int encode_run(const config::AppConfig &cfg, int seconds, const std::string &out_path);

}  // namespace rksec::app
