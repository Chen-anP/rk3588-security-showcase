#include <gtest/gtest.h>
#include <turbojpeg.h>

#include <cstdint>
#include <vector>

#include "media/jpeg_decoder.hpp"
#include "media/jpeg_encoder.hpp"

using rksec::media::Image;
using rksec::media::JpegDecoder;
using rksec::media::scaled_size;

namespace {

// 生成一张测试用 JPEG：左半红、右半蓝，4:2:2 采样（和 UVC 摄像头的 MJPG 一样）
std::vector<std::uint8_t> make_jpeg(int w, int h)
{
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w * h * 3));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            auto *p = &rgb[static_cast<std::size_t>((y * w + x) * 3)];
            p[0] = x < w / 2 ? 255 : 0;
            p[1] = 0;
            p[2] = x < w / 2 ? 0 : 255;
        }
    }
    tjhandle h_enc = tjInitCompress();
    unsigned char *out = nullptr;
    unsigned long size = 0;
    tjCompress2(h_enc, rgb.data(), w, 0, h, TJPF_RGB, &out, &size, TJSAMP_422, 90, 0);
    std::vector<std::uint8_t> jpeg(out, out + size);
    tjFree(out);
    tjDestroy(h_enc);
    return jpeg;
}

// XRGB8888 在内存里是 B G R X
struct Bgr {
    int b, g, r;
};
Bgr pixel(const Image &img, int x, int y)
{
    const auto *p = &img.pixels[static_cast<std::size_t>(y) * img.stride + static_cast<std::size_t>(x) * 4];
    return {p[0], p[1], p[2]};
}

}  // namespace

TEST(JpegDecoder, ScaledSizeForCamera)
{
    int w = 0;
    int h = 0;
    scaled_size(1280, 720, 640, w, h);  // 摄像头 → 预览区
    EXPECT_EQ(w, 640);
    EXPECT_EQ(h, 360);
    scaled_size(1280, 720, 2000, w, h);  // 不放大
    EXPECT_EQ(w, 1280);
    scaled_size(1280, 720, 10, w, h);  // 再小也只能到 1/8
    EXPECT_EQ(w, 160);
    EXPECT_EQ(h, 90);
}

TEST(JpegDecoder, DecodesAndScalesToXrgb)
{
    const auto jpeg = make_jpeg(1280, 720);
    JpegDecoder dec;
    Image img;
    std::string err;
    ASSERT_TRUE(dec.decode(jpeg.data(), jpeg.size(), 640, img, err)) << err;
    EXPECT_EQ(img.width, 640);
    EXPECT_EQ(img.height, 360);
    EXPECT_EQ(img.stride, 640U * 4);
    ASSERT_EQ(img.pixels.size(), 640U * 4 * 360);
    const Bgr left = pixel(img, 100, 180);
    const Bgr right = pixel(img, 540, 180);
    EXPECT_GT(left.r, 200);  // 左边红
    EXPECT_LT(left.b, 60);
    EXPECT_GT(right.b, 200);  // 右边蓝
    EXPECT_LT(right.r, 60);
    EXPECT_EQ(dec.stats().ok, 1U);
}

TEST(JpegDecoder, ReusesBuffer)
{
    const auto jpeg = make_jpeg(320, 240);
    JpegDecoder dec;
    Image img;
    std::string err;
    ASSERT_TRUE(dec.decode(jpeg.data(), jpeg.size(), 320, img, err));
    const auto *data = img.pixels.data();
    ASSERT_TRUE(dec.decode(jpeg.data(), jpeg.size(), 320, img, err));
    EXPECT_EQ(img.pixels.data(), data) << "尺寸不变时不应重新分配内存（每帧都分配会让内存碎片化）";
}

TEST(JpegDecoder, RejectsNonJpeg)
{
    JpegDecoder dec;
    Image img;
    std::string err;
    const std::vector<std::uint8_t> junk(1000, 0x55);
    EXPECT_FALSE(dec.decode(junk.data(), junk.size(), 640, img, err));
    EXPECT_NE(err.find("FF D8"), std::string::npos) << err;
    EXPECT_FALSE(dec.decode(nullptr, 0, 640, img, err));
    EXPECT_EQ(dec.stats().failed, 2U);
}

TEST(JpegDecoder, TruncatedFrameDoesNotCrash)
{
    // USB 传输出错时摄像头会给出只有前半截的帧
    const auto jpeg = make_jpeg(640, 480);
    JpegDecoder dec;
    Image img;
    std::string err;
    const bool ok = dec.decode(jpeg.data(), jpeg.size() / 2, 640, img, err);
    // libjpeg-turbo 对截断的数据给警告（补灰色），能显示；不管哪种结果都不能崩溃，且要计数
    const auto s = dec.stats();
    if (ok) {
        EXPECT_EQ(s.warnings, 1U);
    } else {
        EXPECT_EQ(s.failed, 1U);
    }
    // 头都不完整的数据一定失败
    EXPECT_FALSE(dec.decode(jpeg.data(), 20, 640, img, err));
}

// 编码（第 7d 步，人脸库里的对齐小图）：编码再解码，颜色基本不变
TEST(JpegEncoder, RoundTrip)
{
    const int w = 112;
    const int h = 112;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w * h * 3));
    for (std::size_t i = 0; i < rgb.size(); i += 3) {
        rgb[i] = 200;
        rgb[i + 1] = 120;
        rgb[i + 2] = 40;
    }
    std::vector<std::uint8_t> jpeg;
    std::string err;
    ASSERT_TRUE(rksec::media::encode_rgb_jpeg(rgb.data(), w, h, 95, jpeg, err)) << err;
    EXPECT_GT(jpeg.size(), 100U);
    EXPECT_LT(jpeg.size(), 20000U) << "112×112 的小图应该只有几 KB";
    JpegDecoder dec;
    Image img;
    ASSERT_TRUE(dec.decode(jpeg.data(), jpeg.size(), 1 << 14, img, err)) << err;
    ASSERT_EQ(img.width, w);
    const std::uint8_t *p = img.pixels.data() + (56 * img.stride) + 56 * 4;  // B G R X
    EXPECT_NEAR(p[2], 200, 3);
    EXPECT_NEAR(p[1], 120, 3);
    EXPECT_NEAR(p[0], 40, 3);
}
