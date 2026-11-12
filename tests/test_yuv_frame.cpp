// YUV 画面描述和 CPU 颜色转换（第 5b 步）。不需要硬件，CI 上跑。
#include "media/yuv_frame.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

using namespace rksec::media;

namespace {

// 按 BT.601 全范围从 RGB 算 YUV（JPEG 的公式），四舍五入
void rgb_to_yuv(int r, int g, int b, std::uint8_t &y, std::uint8_t &u, std::uint8_t &v)
{
    const auto to8 = [](double d) { return static_cast<std::uint8_t>(std::clamp(d + 0.5, 0.0, 255.0)); };
    const double Y = 0.299 * r + 0.587 * g + 0.114 * b;
    y = to8(Y);
    u = to8((b - Y) / 1.772 + 128);  // 纯红时 V 算出来是 256，要截到 255，否则变成 0
    v = to8((r - Y) / 1.402 + 128);
}

// 做一帧纯色的 YUV，stride 比宽度大，填充区写成乱码（转换时必须跳过）
std::vector<std::uint8_t> solid(const YuvLayout &l, int r, int g, int b)
{
    std::vector<std::uint8_t> buf(l.total_bytes(), 0xAB);
    std::uint8_t y = 0;
    std::uint8_t u = 0;
    std::uint8_t v = 0;
    rgb_to_yuv(r, g, b, y, u, v);
    const int uv_rows = l.format == YuvFormat::nv12 ? l.height / 2 : l.height;
    for (int row = 0; row < l.height; ++row) {
        for (int x = 0; x < l.width; ++x) {
            buf[static_cast<std::size_t>(row * l.hstride + x)] = y;
        }
    }
    for (int row = 0; row < uv_rows; ++row) {
        for (int x = 0; x < l.width; x += 2) {
            const std::size_t o = l.uv_offset() + static_cast<std::size_t>(row * l.hstride + x);
            buf[o] = u;
            buf[o + 1] = v;
        }
    }
    return buf;
}

}  // namespace

TEST(YuvFrame, LayoutSizes)
{
    // 1280x720 NV16（这个摄像头）：Y 921600，UV 一样大
    YuvLayout a{YuvFormat::nv16, 1280, 720, 1280, 720};
    EXPECT_TRUE(a.valid());
    EXPECT_EQ(a.y_bytes(), 921600U);
    EXPECT_EQ(a.uv_bytes(), 921600U);
    EXPECT_EQ(a.total_bytes(), 1843200U) << "5a 里 mpi_dec_test 输出的文件就是这么大";
    // NV12：UV 是 Y 的一半
    YuvLayout b{YuvFormat::nv12, 1280, 720, 1280, 720};
    EXPECT_EQ(b.total_bytes(), 1382400U);
    // 有对齐：1080 行按 16 对齐成 1088，UV 从 hstride × vstride 开始
    YuvLayout c{YuvFormat::nv12, 1920, 1080, 1920, 1088};
    EXPECT_EQ(c.uv_offset(), 1920U * 1088U);
}

TEST(YuvFrame, InvalidLayouts)
{
    EXPECT_FALSE((YuvLayout{YuvFormat::nv12, 0, 720, 1280, 720}.valid()));
    EXPECT_FALSE((YuvLayout{YuvFormat::nv12, 1280, 720, 1000, 720}.valid())) << "stride 比宽度小";
    EXPECT_FALSE((YuvLayout{YuvFormat::nv12, 1281, 720, 1296, 720}.valid())) << "宽度是奇数";
}

TEST(YuvFrame, ConvertsKnownColorsIgnoringStridePadding)
{
    struct C {
        int r, g, b;
    };
    for (const auto fmt : {YuvFormat::nv12, YuvFormat::nv16}) {
        for (const C c : {C{255, 0, 0}, C{0, 255, 0}, C{0, 0, 255}, C{128, 128, 128}, C{255, 255, 255}, C{0, 0, 0}}) {
            const YuvLayout l{fmt, 64, 32, 80, 48};  // stride 和行数都比宽高大
            const auto buf = solid(l, c.r, c.g, c.b);
            Image out;
            yuv_to_xrgb(YuvView{l, buf.data(), -1}, out);
            ASSERT_EQ(out.width, 64);
            ASSERT_EQ(out.height, 32);
            const std::uint8_t *p = out.pixels.data() + out.stride * 31 + 63 * 4;  // 右下角
            EXPECT_NEAR(p[2], c.r, 3) << format_name(fmt);
            EXPECT_NEAR(p[1], c.g, 3) << format_name(fmt);
            EXPECT_NEAR(p[0], c.b, 3) << format_name(fmt);
        }
    }
}
