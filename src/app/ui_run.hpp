#pragma once

#include "core/config.hpp"

namespace rksec::app {

// --ui：在屏幕上运行界面（第 4b 步），直到 Ctrl+C / SIGTERM。
//
// 线程：
//   capture 线程  摄像头取帧 → 有界队列（容量 1，满了丢旧帧，旧帧立刻还给驱动）
//   preview 线程  取帧 → CPU 解码 MJPG 并缩小到 640x360 → LatestImage（最新一帧信箱）
//   主线程（界面）LVGL 渲染、触摸输入、翻页显示；每秒更新状态，每 10 秒写一行日志
// 结束时打印报告：界面帧率、解码耗时、CPU 占用、内存——作为第 5 步硬件解码的对比基线。
//
// 返回：0 正常结束；4 显示设备打不开（没有屏幕、权限不够、另一个程序占着显示）
int ui_run(const config::AppConfig &cfg);

}  // namespace rksec::app
