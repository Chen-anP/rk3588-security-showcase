#include "media/yuv_frame.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace rksec::media {

namespace {

std::uint8_t clamp8(int v)
{
    return static_cast<std::uint8_t>(std::clamp(v, 0, 255));
}

}  // namespace

const char *format_name(YuvFormat f)
{
    return f == YuvFormat::nv12 ? "NV12" : "NV16";
}

bool YuvLayout::valid() const
{
    return width > 0 && height > 0 && hstride >= width && vstride >= height && width % 2 == 0 && height % 2 == 0;
}

void yuv_to_xrgb(const YuvView &in, Image &out)
{
    const YuvLayout &l = in.layout;
    out.width = l.width;
    out.height = l.height;
    out.stride = static_cast<std::size_t>(l.width) * 4;
    out.pixels.resize(out.stride * static_cast<std::size_t>(l.height));
    if (!l.valid() || in.data == nullptr) {
        return;
    }
    const std::uint8_t *y_plane = in.data;
    const std::uint8_t *uv_plane = in.data + l.uv_offset();
    const auto hs = static_cast<std::size_t>(l.hstride);
    for (int y = 0; y < l.height; ++y) {
        const std::uint8_t *yrow = y_plane + static_cast<std::size_t>(y) * hs;
        const int uv_y = l.format == YuvFormat::nv12 ? y / 2 : y;  // 4:2:0 两行 Y 共用一行 UV
        const std::uint8_t *uvrow = uv_plane + static_cast<std::size_t>(uv_y) * hs;
        std::uint8_t *o = out.pixels.data() + static_cast<std::size_t>(y) * out.stride;
        for (int x = 0; x < l.width; ++x) {
            const int Y = yrow[x];
            const auto ui = static_cast<std::size_t>(x & ~1);
            const int U = uvrow[ui] - 128;
            const int V = uvrow[ui + 1] - 128;
            // BT.601 全范围，定点：系数 × 1024
            const int r = Y + ((1436 * V) >> 10);
            const int g = Y - ((352 * U + 731 * V) >> 10);
            const int b = Y + ((1815 * U) >> 10);
            o[0] = clamp8(b);
            o[1] = clamp8(g);
            o[2] = clamp8(r);
            o[3] = 0xFF;
            o += 4;
        }
    }
}

bool save_ppm(const Image &img, const std::string &path)
{
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    bool ok = std::fprintf(f, "P6\n%d %d\n255\n", img.width, img.height) > 0;
    for (int y = 0; ok && y < img.height; ++y) {
        const std::uint8_t *p = img.pixels.data() + static_cast<std::size_t>(y) * img.stride;
        for (int x = 0; ok && x < img.width; ++x, p += 4) {
            const std::array<unsigned char, 3> rgb = {p[2], p[1], p[0]};
            ok = std::fwrite(rgb.data(), 1, rgb.size(), f) == rgb.size();
        }
    }
    return std::fclose(f) == 0 && ok;
}

}  // namespace rksec::media
