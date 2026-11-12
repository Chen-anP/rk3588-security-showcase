#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "media/jpeg_decoder.hpp"

// 硬件解码器输出的 YUV 画面的描述（第 5 步）。只描述，不拥有内存：内存属于解码器的输出缓冲区。
//
// 半平面（semi-planar）格式：先是整块 Y（亮度），后面紧跟交错的 UV（色度）
//   NV12（YUV 4:2:0）：UV 宽、高都是 Y 的一半 → UV 平面行数 = Y 的一半
//   NV16（YUV 4:2:2）：UV 宽是 Y 的一半、高一样 → UV 平面行数 = Y 的行数
// 第 5a 步摸底：这个摄像头的 MJPG 是 4:2:2（yuvj422p），MPP 按原样输出 NV16，不是常见的 NV12。
//
// stride：硬件要求每行、每平面按一定字节对齐，所以“每行字节数”（hstride）可能大于宽度，
// “行数”（vstride）可能大于高度。读写像素必须按 stride 算位置，不能按 width × height。
namespace rksec::media {

enum class YuvFormat : std::uint8_t { nv12, nv16 };

const char *format_name(YuvFormat f);

struct YuvLayout {
    YuvFormat format = YuvFormat::nv12;
    int width = 0;
    int height = 0;
    int hstride = 0;  // Y 平面每行字节数（≥ width）
    int vstride = 0;  // Y 平面行数（≥ height）；UV 平面紧跟在 hstride × vstride 之后

    std::size_t y_bytes() const { return static_cast<std::size_t>(hstride) * static_cast<std::size_t>(vstride); }
    std::size_t uv_offset() const { return y_bytes(); }
    std::size_t uv_bytes() const { return format == YuvFormat::nv12 ? y_bytes() / 2 : y_bytes(); }
    std::size_t total_bytes() const { return y_bytes() + uv_bytes(); }
    // 宽高为正、stride 不小于宽高、宽高是偶数（色度按 2 个像素一组）
    bool valid() const;
};

// 一帧 YUV 画面：数据指针和（如果有）DMA-BUF fd，都属于产生它的那一方
struct YuvView {
    YuvLayout layout;
    const std::uint8_t *data = nullptr;
    int fd = -1;
};

// CPU 转换成 XRGB8888（JPEG 用的 BT.601 全范围），只用于检查和存图，不在每帧的路径上（每帧用 RGA，第 5c 步）
void yuv_to_xrgb(const YuvView &in, Image &out);

// 存成 PPM（P6），Windows 上用 IrfanView 或 convert 转 PNG 看
bool save_ppm(const Image &img, const std::string &path);

}  // namespace rksec::media
