#pragma once

#include "core/config.hpp"

namespace rksec::app {

// --record-run 秒数（第 8b 步）：不接屏幕跑完整流水线（检测 + 人脸识别 + 硬件编码 + 事件录像），
// 陌生人出现时按配置录像。结束时列出这段时间里产生的事件和文件。
// Ctrl+C 提前结束（正在录的事件就地结束，文件完整）。返回 0；没有硬件支持返回 2，设备出错返回 4
int record_run(const config::AppConfig &cfg, int seconds);

}  // namespace rksec::app
