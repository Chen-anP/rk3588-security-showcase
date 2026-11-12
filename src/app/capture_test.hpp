#pragma once

#include <string>

#include "core/config.hpp"

namespace rksec::app {

// --capture-test：按配置打开摄像头，抓 frames 帧，报告实际帧率、帧间隔、延迟、丢帧。
// save_path 不为空时，把中间那一帧存成 JPEG（3a 实测：这个摄像头的 MJPG 帧自带哈夫曼表，可以直接打开）。
// 返回进程退出码：0 成功；4 设备错误
int capture_test(const config::AppConfig &cfg, int frames, const std::string &save_path);

}  // namespace rksec::app
