#pragma once

#include <cstdint>

#include "media/jpeg_decoder.hpp"

// 在 XRGB 图上画简单的图形：存结果图检查用（--detect-image、--face-image），不在每帧的路径上。
namespace rksec::media {

// 空心矩形，线宽 thick 像素，颜色 0xRRGGBB；超出图片的部分不画
void draw_rect(Image &img, int x1, int y1, int x2, int y2, std::uint32_t rgb, int thick);

// 实心小方块（中心 (x, y)，边长 2 × r + 1），标关键点用
void draw_dot(Image &img, int x, int y, int r, std::uint32_t rgb);

}  // namespace rksec::media
