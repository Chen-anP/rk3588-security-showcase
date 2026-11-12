// RGA 缩放 + 颜色转换（第 5c 步）。只在编译了 RGA、有 /dev/rga 和 dma-heap 的板子上跑。
#include <gtest/gtest.h>

#ifdef RKSEC_HAVE_RGA

#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "media/dma_buffer.hpp"
#include "media/rga_converter.hpp"

using namespace rksec::media;

namespace {

bool have_rga()
{
    return ::access("/dev/rga", R_OK | W_OK) == 0 && ::access("/dev/dma_heap/system", R_OK | W_OK) == 0;
}

// BT.601 全范围（JPEG）
void rgb_to_yuv(int r, int g, int b, std::uint8_t &y, std::uint8_t &u, std::uint8_t &v)
{
    const auto to8 = [](double d) { return static_cast<std::uint8_t>(std::clamp(d + 0.5, 0.0, 255.0)); };
    const double Y = 0.299 * r + 0.587 * g + 0.114 * b;
    y = to8(Y);
    u = to8((b - Y) / 1.772 + 128);
    v = to8((r - Y) / 1.402 + 128);
}

// 纯色 NV16。尺寸注意 RGA3 的限制：输入、输出的宽高都至少 68×2（5c 第一次上板测试用了 64 宽的输出，
// 驱动报 “no core match”），YUV 每行 16 字节对齐
std::unique_ptr<DmaBuffer> solid_nv16(const YuvLayout &l, int r, int g, int b)
{
    std::string err;
    auto buf = DmaBuffer::allocate(l.total_bytes(), err);
    if (!buf) {
        return nullptr;
    }
    std::uint8_t y = 0;
    std::uint8_t u = 0;
    std::uint8_t v = 0;
    rgb_to_yuv(r, g, b, y, u, v);
    buf->begin_cpu_write();
    std::memset(buf->data(), y, l.y_bytes());
    for (std::size_t i = 0; i < l.uv_bytes(); i += 2) {
        buf->data()[l.uv_offset() + i] = u;
        buf->data()[l.uv_offset() + i + 1] = v;
    }
    buf->end_cpu_write();
    return buf;
}

}  // namespace

TEST(RgaConverter, Nv16ToXrgbAndRgbWithFullRangeColors)
{
    if (!have_rga()) {
        GTEST_SKIP() << "没有 /dev/rga 或 dma-heap";
    }
    const YuvLayout l{YuvFormat::nv16, 256, 128, 256, 128};
    struct C {
        int r, g, b;
    };
    // (200,200,200) 能区分全范围和有限范围：按有限范围转会变成约 214
    for (const C c : {C{200, 200, 200}, C{255, 0, 0}, C{0, 255, 0}, C{0, 0, 255}, C{30, 30, 30}}) {
        const auto src = solid_nv16(l, c.r, c.g, c.b);
        ASSERT_TRUE(src);
        std::string err;
        const auto out = DmaBuffer::allocate(128 * 64 * 4, err);
        ASSERT_TRUE(out) << err;
        const RgbTarget t{out->fd(), 128, 64, 128, RgbFormat::xrgb8888};
        ASSERT_TRUE(rga_convert(YuvView{l, src->data(), src->fd()}, t, {0, 0, 128, 64}, err)) << err;
        out->begin_cpu_read();
        const std::uint8_t *p = out->data() + (32 * 128 + 64) * 4;  // 中间的像素
        EXPECT_NEAR(p[2], c.r, 4) << "R，颜色 " << c.r << "," << c.g << "," << c.b;
        EXPECT_NEAR(p[1], c.g, 4) << "G";
        EXPECT_NEAR(p[0], c.b, 4) << "B";
        out->end_cpu_read();
    }
}

TEST(RgaConverter, LetterboxWritesOnlyTheRect)
{
    if (!have_rga()) {
        GTEST_SKIP() << "没有 /dev/rga 或 dma-heap";
    }
    const YuvLayout l{YuvFormat::nv16, 256, 128, 256, 128};
    const auto src = solid_nv16(l, 255, 0, 0);
    ASSERT_TRUE(src);
    std::string err;
    const auto out = DmaBuffer::allocate(128 * 128 * 3, err);  // 128x128 RGB888，stride 128 满足 16 对齐
    ASSERT_TRUE(out) << err;
    out->begin_cpu_write();
    std::memset(out->data(), 114, out->size());
    out->end_cpu_write();
    const RgbTarget t{out->fd(), 128, 128, 128, RgbFormat::rgb888};
    ASSERT_TRUE(rga_convert(YuvView{l, src->data(), src->fd()}, t, {0, 32, 128, 64}, err)) << err;  // 上下各补 32 行
    out->begin_cpu_read();
    const auto px = [&](int x, int y) { return out->data() + (y * 128 + x) * 3; };
    EXPECT_EQ(px(10, 5)[0], 114) << "上面的灰边不能动";
    EXPECT_EQ(px(10, 120)[1], 114) << "下面的灰边不能动";
    EXPECT_NEAR(px(64, 64)[0], 255, 4) << "中间是红色（RGB888 第一个字节是 R）";
    EXPECT_NEAR(px(64, 64)[2], 0, 4);
    out->end_cpu_read();
}

// 裁剪（第 7c 步）：画面左半红、右半蓝，只取右边一块 → 输出全是蓝的。
// 起点故意不是 16 的倍数（130）：人的位置是任意的，RGA3 读 YUV 的起点只要求偶数
TEST(RgaConverter, CropTakesOnlyTheSourceRect)
{
    if (!have_rga()) {
        GTEST_SKIP() << "没有 /dev/rga 或 dma-heap";
    }
    const YuvLayout l{YuvFormat::nv16, 256, 128, 256, 128};
    const auto src = solid_nv16(l, 255, 0, 0);
    ASSERT_TRUE(src);
    std::uint8_t y = 0;
    std::uint8_t u = 0;
    std::uint8_t v = 0;
    rgb_to_yuv(0, 0, 255, y, u, v);
    src->begin_cpu_write();
    for (int row = 0; row < 128; ++row) {
        std::uint8_t *yl = src->data() + row * 256;
        std::uint8_t *uvl = src->data() + l.uv_offset() + static_cast<std::size_t>(row) * 256;
        for (int x = 128; x < 256; x += 2) {
            yl[x] = y;
            yl[x + 1] = y;
            uvl[x] = u;
            uvl[x + 1] = v;
        }
    }
    src->end_cpu_write();
    std::string err;
    const auto out = DmaBuffer::allocate(96 * 96 * 3, err);
    ASSERT_TRUE(out) << err;
    const RgbTarget t{out->fd(), 96, 96, 96, RgbFormat::rgb888};
    const YuvView view{l, src->data(), src->fd()};
    ASSERT_TRUE(rga_convert(view, Rect{130, 10, 100, 100}, t, {0, 0, 96, 96}, err)) << err;
    out->begin_cpu_read();
    for (const int i : {0, 95, 96 * 48 + 48, 96 * 96 - 1}) {
        const std::uint8_t *p = out->data() + i * 3;
        EXPECT_NEAR(p[0], 0, 6) << "第 " << i << " 个像素应该是蓝的（R）";
        EXPECT_NEAR(p[2], 255, 6) << "第 " << i << " 个像素应该是蓝的（B）";
    }
    out->end_cpu_read();

    // 只取左边一块：全是红的
    ASSERT_TRUE(rga_convert(view, Rect{0, 0, 100, 128}, t, {0, 0, 96, 96}, err)) << err;
    out->begin_cpu_read();
    EXPECT_NEAR(out->data()[(96 * 48 + 48) * 3], 255, 6);
    out->end_cpu_read();

    // 奇数、超出画面：事先拒绝，说清楚原因
    EXPECT_FALSE(rga_convert(view, Rect{131, 10, 100, 100}, t, {0, 0, 96, 96}, err));
    EXPECT_NE(err.find("偶数"), std::string::npos) << err;
    EXPECT_FALSE(rga_convert(view, Rect{200, 0, 100, 100}, t, {0, 0, 96, 96}, err));
}

#else

TEST(RgaConverter, NotBuiltWithoutRga)
{
    GTEST_SKIP() << "这次编译没有 RGA（CI、电脑上正常）";
}

#endif
