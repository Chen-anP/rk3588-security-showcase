#pragma once

#include <string>

#include "core/config.hpp"

namespace rksec::app {

// --decode-bench N：先从摄像头取 N 帧 MJPG 存在内存里，再用几种方法分别解一遍，比较耗时和 CPU 占用（第 5b 步）：
//   MPP 硬件解码（全尺寸，输出 NV12/NV16）——编译时有 MPP 才有
//   CPU libjpeg-turbo 全尺寸、输出 YUV（和硬件解码同样的工作）
//   CPU libjpeg-turbo 缩小到 640 宽、输出 XRGB（第 4b 步预览用的方法，基线）
// 同时检查硬件解码的结果对不对：和 libjpeg-turbo 解出的亮度（Y）逐像素比较。
// save_path 非空时，把第一帧的硬件解码结果转成图片存下来（PPM）。
// 先存帧再解码：耗时只和解码有关，不受摄像头帧率影响；几种方法解的是同一批帧。
// 返回 0 成功；4 摄像头或解码器打不开；1 硬件解码结果和 CPU 差太多
int decode_bench(const config::AppConfig &cfg, int frames, const std::string &save_path);

}  // namespace rksec::app
