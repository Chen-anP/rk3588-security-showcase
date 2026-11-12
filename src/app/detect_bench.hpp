#pragma once

#include <string>

#include "core/config.hpp"

namespace rksec::app {

// --detect-image 图片.jpg：检测一张图片，打印每个框（全部 80 类），和官方 rknn_yolo11_demo 的结果对照（第 5d 步）。
// 图片用 CPU 解码、CPU letterbox，再走和每帧一样的零拷贝推理 + 后处理。save_path 非空时存一张画了框的 PPM。
// 返回 0 成功；4 模型或图片打不开；2 这次编译没有 RKNN
int detect_image(const config::AppConfig &cfg, const std::string &path, const std::string &save_path);

// --detect-bench N：取 N 帧，逐帧 MPP 解码 → RGA letterbox → NPU 推理 → 后处理，测每一段的耗时（第 5d 步）：
//   推理分别用“绑定 1 个核”和“3 个核联合跑一个模型”两种方式
//   基线：CPU 解码 + CPU letterbox 的耗时；第一个检测到目标的帧用两种预处理分别检测，对比框是否一致
//   （CPU 这边是另一套 letterbox 实现，两边一致说明坐标换算对）
// 返回 0 成功；4 设备或模型打不开；1 两种预处理的检测结果不一致；2 这次编译没有 MPP、RGA 或 RKNN
int detect_bench(const config::AppConfig &cfg, int frames);

// --detect-run 秒数：不接屏幕，跑完整的预览 + 检测流水线（采集实时进来），每 5 秒打印一次状态，
// 结束时报告各阶段耗时、端到端延迟、每个推理线程处理的帧数、乱序次数、CPU（第 6 步）。
// workers：推理线程个数（覆盖配置文件，0 = 用配置文件的）。返回 0 成功；4 设备打不开；2 没有硬件
int detect_run(const config::AppConfig &cfg, int seconds, int workers);

}  // namespace rksec::app
