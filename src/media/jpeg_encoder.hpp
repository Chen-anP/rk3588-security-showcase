#pragma once

#include <cstdint>
#include <string>
#include <vector>

// JPEG 编码（libjpeg-turbo，第 7d 步）：人脸库里存对齐后的 112×112 小图。
// 只在登记时用，不在每帧的路径上。
namespace rksec::media {

// RGB888（每行没有填充）→ JPEG。quality 1～100；色度不抽样（4:4:4），小图上颜色不糊
bool encode_rgb_jpeg(const std::uint8_t *rgb, int width, int height, int quality, std::vector<std::uint8_t> &out,
                     std::string &err);

}  // namespace rksec::media
