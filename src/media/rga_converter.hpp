#pragma once

#include <cstdint>
#include <string>

#include "media/yuv_frame.hpp"

// RGA 硬件做缩放 + 颜色转换（第 5c 步）：解码出来的 NV12/NV16 → NPU 要的 RGB888、界面要的 XRGB8888。
//
// - 输入输出都用 DMA-BUF fd，CPU 不碰像素（零拷贝）
// - 只调度到 RGA3 的两个核（5a 摸底：RGA2 只能访问 4 GB 以下的内存，板子 16 GB、dma-heap 分配的地址不受控制）
// - 颜色转换用 BT.601 全范围（JPEG 的标准）。librga 默认是 BT.601 有限范围，不改的话画面对比度、颜色会不对
// - RGA3 的限制：输入、输出宽高至少 68×2，缩放比例 1/8～8，YUV 每行 16 字节对齐
// - 同步执行：函数返回时硬件已经做完
// - 线程：可以在任何一个线程用；RGA3 的核心选择是按线程生效的，每个线程第一次用时自动设置
//
// 只在板子上编译（需要 librga，CMake 自动判断，定义 RKSEC_HAVE_RGA）。
namespace rksec::media {

enum class RgbFormat : std::uint8_t {
    rgb888,    // 内存里 R G B，每像素 3 字节（NPU 输入）
    xrgb8888,  // 内存里 B G R X，每像素 4 字节（DRM、LVGL 的 XRGB8888）
};

struct RgbTarget {
    int fd = -1;
    int width = 0;
    int height = 0;
    int stride_px = 0;  // 每行像素数（RGA3：RGB888 要 16 对齐，XRGB8888 要 4 对齐）
    RgbFormat format = RgbFormat::rgb888;
};

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// 把 src 整幅缩放、转换后写到 dst 的 rect 区域（rect 外面不动——letterbox 的边事先填好，每帧不用再填）
bool rga_convert(const YuvView &src, const RgbTarget &dst, const Rect &rect, std::string &err);

// 只取 src 的 src_rect 这一块（裁剪，第 7c 步：人脸检测只看人的上半身）。
// src_rect 要在画面里，x、y、宽、高都是偶数（YUV 的色度两个像素一组），宽至少 68
bool rga_convert(const YuvView &src, const Rect &src_rect, const RgbTarget &dst, const Rect &rect, std::string &err);

// librga 的版本号（日志用）
std::string rga_version();

}  // namespace rksec::media
