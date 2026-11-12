#pragma once

#include <memory>
#include <string>

#include "app/ui_sim.hpp"

// SDL 窗口后端：电脑上（WSL2 的 WSLg、Linux 桌面）显示界面，鼠标左键当手指。
// 只在找到 SDL2 时编译（cmake 自动判断），板子上不需要。
namespace rksec::sim {

// 打开 width×height 的窗口。scale：窗口放大倍数（高分屏上 1024x600 太小时用 2）。失败返回 nullptr，err 是原因
std::unique_ptr<app::SimBackend> sdl_backend(int width, int height, int scale, std::string &err);

}  // namespace rksec::sim
