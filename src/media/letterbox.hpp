#pragma once

#include <cstdint>

#include "media/jpeg_decoder.hpp"

// 把任意宽高的画面按比例缩放、居中放进固定大小的方框里，四周补边（letterbox），第 5c 步。
//
// YOLO 模型的输入是 640×640。1280×720 的画面直接拉成正方形会变形（人变胖），检测变差；
// 所以按比例缩到 640×360，上下各补 140 行灰色（114，和 rknn_model_zoo 的 demo 一样）。
// 检测出来的框是在 640×640 里的坐标，要用 to_source() 换算回原画面。
namespace rksec::media {

struct Letterbox {
    double scale_x = 1;  // 原画面 → 方框里的缩放比例（宽高取偶数后两个方向可能差一点点，分开存）
    double scale_y = 1;
    int left = 0;  // 画面在方框里的位置和大小
    int top = 0;
    int width = 0;
    int height = 0;

    // 方框里的坐标 → 原画面坐标（可能超出原画面，调用方自己裁）
    double to_source_x(double x) const { return (x - left) / scale_x; }
    double to_source_y(double y) const { return (y - top) / scale_y; }
};

// 宽高和偏移都取偶数（RGA 处理 YUV 时要求 2 对齐）。src、dst 的宽高都要大于 0
Letterbox fit_letterbox(int src_w, int src_h, int dst_w, int dst_h);

// CPU 版 letterbox（第 5d 步）：XRGB 图按 lb 双线性缩放，写进 dst（RGB888，dst_w×dst_h，每行 stride_px 像素），
// 画面外的区域填 pad。每帧的路径上用 RGA；这个用于检测单张图片（--detect-image）和对比基线。
void letterbox_to_rgb(const Image &src, const Letterbox &lb, std::uint8_t *dst, int dst_w, int dst_h, int stride_px,
                      std::uint8_t pad);

}  // namespace rksec::media
