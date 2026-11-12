#include "media/letterbox.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rksec::media {

Letterbox fit_letterbox(int src_w, int src_h, int dst_w, int dst_h)
{
    Letterbox l;
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return l;
    }
    const double sx = static_cast<double>(dst_w) / src_w;
    const double sy = static_cast<double>(dst_h) / src_h;
    const double s = std::min(sx, sy);
    l.width = std::max(2, static_cast<int>(src_w * s) & ~1);
    l.height = std::max(2, static_cast<int>(src_h * s) & ~1);
    l.left = ((dst_w - l.width) / 2) & ~1;
    l.top = ((dst_h - l.height) / 2) & ~1;
    // 取偶数以后实际比例略有变化，用实际宽高重新算，换算坐标才准
    l.scale_x = static_cast<double>(l.width) / src_w;
    l.scale_y = static_cast<double>(l.height) / src_h;
    return l;
}

void letterbox_to_rgb(const Image &src, const Letterbox &lb, std::uint8_t *dst, int dst_w, int dst_h, int stride_px,
                      std::uint8_t pad)
{
    const std::size_t row = static_cast<std::size_t>(stride_px) * 3;
    for (int y = 0; y < dst_h; ++y) {
        std::memset(dst + static_cast<std::size_t>(y) * row, pad, static_cast<std::size_t>(dst_w) * 3);
    }
    if (src.width <= 0 || src.height <= 0 || lb.width <= 0 || lb.height <= 0) {
        return;
    }
    // 双线性：目标像素中心 (x + 0.5) 对应源图的 (x + 0.5) / scale - 0.5，取周围 4 个像素按距离加权
    for (int y = 0; y < lb.height && lb.top + y < dst_h; ++y) {
        const double fy = std::clamp((y + 0.5) / lb.scale_y - 0.5, 0.0, static_cast<double>(src.height - 1));
        const int y0 = static_cast<int>(fy);
        const int y1 = std::min(y0 + 1, src.height - 1);
        const double wy = fy - y0;
        const std::uint8_t *r0 = src.pixels.data() + static_cast<std::size_t>(y0) * src.stride;
        const std::uint8_t *r1 = src.pixels.data() + static_cast<std::size_t>(y1) * src.stride;
        std::uint8_t *d = dst + static_cast<std::size_t>(lb.top + y) * row + static_cast<std::size_t>(lb.left) * 3;
        for (int x = 0; x < lb.width && lb.left + x < dst_w; ++x, d += 3) {
            const double fx = std::clamp((x + 0.5) / lb.scale_x - 0.5, 0.0, static_cast<double>(src.width - 1));
            const int x0 = static_cast<int>(fx);
            const int x1 = std::min(x0 + 1, src.width - 1);
            const double wx = fx - x0;
            for (int c = 0; c < 3; ++c) {
                // 源图内存是 B G R X，目标是 R G B
                const auto ch = static_cast<std::size_t>(2 - c);
                const double top = r0[static_cast<std::size_t>(x0) * 4 + ch] * (1 - wx) +
                                   r0[static_cast<std::size_t>(x1) * 4 + ch] * wx;
                const double bottom = r1[static_cast<std::size_t>(x0) * 4 + ch] * (1 - wx) +
                                      r1[static_cast<std::size_t>(x1) * 4 + ch] * wx;
                d[c] = static_cast<std::uint8_t>(std::lround(top * (1 - wy) + bottom * wy));
            }
        }
    }
}

}  // namespace rksec::media
