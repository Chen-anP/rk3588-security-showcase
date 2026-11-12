// MPP 硬件解码（第 5b 步）。只在编译了 MPP、而且有 /dev/mpp_service 的板子上跑，其他地方跳过。
#include <gtest/gtest.h>

#ifdef RKSEC_HAVE_MPP

#include <turbojpeg.h>
#include <unistd.h>

#include <cstdlib>
#include <vector>

#include "media/mpp_jpeg_decoder.hpp"

using namespace rksec::media;

namespace {

bool have_device()
{
    return ::access("/dev/mpp_service", R_OK | W_OK) == 0;
}

// 用 libjpeg-turbo 压一张有内容的图（渐变 + 格子），sub 是色度采样
std::vector<std::uint8_t> make_jpeg(int w, int h, int sub)
{
    std::vector<unsigned char> rgb(static_cast<std::size_t>(w * h * 3));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char *p = &rgb[static_cast<std::size_t>((y * w + x) * 3)];
            p[0] = static_cast<unsigned char>(x * 255 / w);
            p[1] = static_cast<unsigned char>(y * 255 / h);
            p[2] = ((x / 32 + y / 32) % 2 != 0) ? 220 : 30;
        }
    }
    tjhandle tj = tjInitCompress();
    unsigned char *out = nullptr;
    unsigned long size = 0;
    tjCompress2(tj, rgb.data(), w, 0, h, TJPF_RGB, &out, &size, sub, 90, 0);
    std::vector<std::uint8_t> jpg(out, out + size);
    tjFree(out);
    tjDestroy(tj);
    return jpg;
}

// 硬件解出的 Y 和 libjpeg-turbo 精确解出的 Y 的平均差
double mean_y_diff(const YuvView &v, const std::vector<std::uint8_t> &jpg)
{
    tjhandle tj = tjInitDecompress();
    int w = 0;
    int h = 0;
    int ss = 0;
    int cs = 0;
    tjDecompressHeader3(tj, jpg.data(), static_cast<unsigned long>(jpg.size()), &w, &h, &ss, &cs);
    std::vector<unsigned char> yuv(tjBufSizeYUV2(w, 1, h, ss));
    tjDecompressToYUV2(tj, jpg.data(), static_cast<unsigned long>(jpg.size()), yuv.data(), w, 1, h, TJFLAG_ACCURATEDCT);
    tjDestroy(tj);
    long long sum = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            sum += std::abs(v.data[static_cast<std::size_t>(y * v.layout.hstride + x)] -
                            yuv[static_cast<std::size_t>(y * w + x)]);
        }
    }
    return static_cast<double>(sum) / (w * h);
}

}  // namespace

TEST(MppJpegDecoder, Decodes422AsNv16And420AsNv12)
{
    if (!have_device()) {
        GTEST_SKIP() << "没有 /dev/mpp_service";
    }
    MppJpegDecoder dec;
    std::string err;
    ASSERT_TRUE(dec.open({}, err)) << err;
    for (const auto &[sub, fmt] : {std::pair{TJSAMP_422, YuvFormat::nv16}, std::pair{TJSAMP_420, YuvFormat::nv12}}) {
        const auto jpg = make_jpeg(1280, 720, sub);
        const auto f = dec.decode(jpg.data(), jpg.size(), err);
        ASSERT_TRUE(f) << err;
        EXPECT_EQ(f->layout.format, fmt);
        EXPECT_EQ(f->layout.width, 1280);
        EXPECT_EQ(f->layout.height, 720);
        EXPECT_GE(f->layout.hstride, 1280);
        EXPECT_GE(f->fd, 0) << "要有 DMA-BUF fd，后面 RGA 零拷贝靠它";
        EXPECT_LT(mean_y_diff(*f, jpg), 2.0) << "亮度和 CPU 解码差太多";
    }
}

TEST(MppJpegDecoder, PoolExhaustionReturnsNullInsteadOfBlocking)
{
    if (!have_device()) {
        GTEST_SKIP() << "没有 /dev/mpp_service";
    }
    MppJpegDecoder dec;
    std::string err;
    MppJpegDecoder::Options o;
    o.out_buffers = 2;
    ASSERT_TRUE(dec.open(o, err)) << err;
    const auto jpg = make_jpeg(640, 480, TJSAMP_422);
    auto a = dec.decode(jpg.data(), jpg.size(), err);
    auto b = dec.decode(jpg.data(), jpg.size(), err);
    ASSERT_TRUE(a && b) << err;
    EXPECT_FALSE(dec.decode(jpg.data(), jpg.size(), err)) << "两块都被拿着，第三次应该立刻失败";
    EXPECT_EQ(dec.stats().no_buffer, 1U);
    a.reset();  // 放手一块
    EXPECT_TRUE(dec.decode(jpg.data(), jpg.size(), err)) << err;
}

TEST(MppJpegDecoder, CorruptFrameDoesNotBreakNextOne)
{
    if (!have_device()) {
        GTEST_SKIP() << "没有 /dev/mpp_service";
    }
    MppJpegDecoder dec;
    std::string err;
    ASSERT_TRUE(dec.open({}, err)) << err;
    const auto jpg = make_jpeg(1280, 720, TJSAMP_422);
    std::vector<std::uint8_t> cut(jpg.begin(),
                                  jpg.begin() + static_cast<long>(jpg.size() / 3));  // USB 传输出错时的样子
    (void)dec.decode(cut.data(), cut.size(), err);  // 成功或失败都可以，不能崩、不能卡死
    const auto f = dec.decode(jpg.data(), jpg.size(), err);
    ASSERT_TRUE(f) << "坏帧之后的好帧要能正常解：" << err;
    EXPECT_LT(mean_y_diff(*f, jpg), 2.0);
}

TEST(MppJpegDecoder, FrameOutlivesDecoder)
{
    if (!have_device()) {
        GTEST_SKIP() << "没有 /dev/mpp_service";
    }
    const auto jpg = make_jpeg(640, 480, TJSAMP_420);
    MppJpegDecoder::Frame keep;
    {
        MppJpegDecoder dec;
        std::string err;
        ASSERT_TRUE(dec.open({}, err)) << err;
        keep = dec.decode(jpg.data(), jpg.size(), err);
        ASSERT_TRUE(keep) << err;
    }  // 解码器先销毁
    EXPECT_LT(mean_y_diff(*keep, jpg), 2.0) << "帧的内存要活到放手为止（ASan 下跑能发现释放后使用）";
}

#else

TEST(MppJpegDecoder, NotBuiltWithoutMpp)
{
    GTEST_SKIP() << "这次编译没有 MPP（CI、电脑上正常）";
}

#endif
