#pragma once

#include <string>

#include "core/config.hpp"

namespace rksec::app {

// --face-image 图片.jpg：人脸检测一张图片（第 7b 步），打印每张脸的框、分数、5 个关键点，
// 和官方 rknn_retinaface_demo 对照。CPU 解码、CPU letterbox，模型用 float32 输出模式。
// save_path 非空时存一张画了框和关键点的 PPM。
// 返回 0 成功；4 模型或图片打不开；2 这次编译没有 RKNN
int face_image(const config::AppConfig &cfg, const std::string &path, const std::string &save_path);

// --face-bench N：取 N 帧（第 7b 步），MPP 解码 → RGA 把整幅画面 letterbox 到模型输入 → 人脸检测，
// 测耗时、统计每帧几张脸。整幅 1280×720 缩到 320 宽，脸会变得很小
// （这一步只测模型本身；第 7c 步改成在人的框里裁一块再检测）。
// 返回 0 成功；4 设备或模型打不开；2 这次编译没有 MPP、RGA 或 RKNN
int face_bench(const config::AppConfig &cfg, int frames);

}  // namespace rksec::app
