#include "media/draw.hpp"

#include <cstddef>

namespace rksec::media {

namespace {

void put(Image &img, int x, int y, std::uint32_t rgb)
{
    if (x < 0 || y < 0 || x >= img.width || y >= img.height) {
        return;
    }
    std::uint8_t *p = img.pixels.data() + static_cast<std::size_t>(y) * img.stride + static_cast<std::size_t>(x) * 4;
    p[0] = static_cast<std::uint8_t>(rgb & 0xFFU);  // 内存顺序 B G R X
    p[1] = static_cast<std::uint8_t>((rgb >> 8U) & 0xFFU);
    p[2] = static_cast<std::uint8_t>((rgb >> 16U) & 0xFFU);
}

}  // namespace

void draw_rect(Image &img, int x1, int y1, int x2, int y2, std::uint32_t rgb, int thick)
{
    for (int t = 0; t < thick; ++t) {
        for (int x = x1; x <= x2; ++x) {
            put(img, x, y1 + t, rgb);
            put(img, x, y2 - t, rgb);
        }
        for (int y = y1; y <= y2; ++y) {
            put(img, x1 + t, y, rgb);
            put(img, x2 - t, y, rgb);
        }
    }
}

void draw_dot(Image &img, int x, int y, int r, std::uint32_t rgb)
{
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            put(img, x + dx, y + dy, rgb);
        }
    }
}

}  // namespace rksec::media
