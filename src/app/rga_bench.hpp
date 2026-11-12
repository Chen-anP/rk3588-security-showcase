#pragma once

#include <string>

#include "core/config.hpp"

namespace rksec::app {

// --rga-bench N：取 N 帧 MJPG，MPP 解码后用 RGA 做两件事，比较耗时、检查结果（第 5c 步）：
//   NPU 输入：整幅缩到 640×360，放进 640×640 RGB888 的中间（上下补灰边，letterbox）
//   界面预览：整幅缩到 640×360 XRGB8888
// 和第 4b 步的预览方法（CPU 解码时缩小）对比整条路径的耗时和 CPU。
// 第一帧用 CPU 算一遍同样的结果逐像素比较；save_dir 非空时把两张输出图存成 PPM。
// 返回 0 成功；4 设备打不开；1 结果不对；2 这次编译没有 MPP 或 RGA
int rga_bench(const config::AppConfig &cfg, int frames, const std::string &save_dir);

}  // namespace rksec::app
