// letterbox 的几何计算（第 5c 步）。检测框要靠它换算回原画面，算错了框就画歪。
#include "media/letterbox.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using rksec::media::fit_letterbox;

TEST(Letterbox, Camera720pIntoModel640)
{
    const auto l = fit_letterbox(1280, 720, 640, 640);
    EXPECT_EQ(l.width, 640);
    EXPECT_EQ(l.height, 360);
    EXPECT_EQ(l.left, 0);
    EXPECT_EQ(l.top, 140) << "上下各补 140 行";
    EXPECT_DOUBLE_EQ(l.scale_x, 0.5);
    EXPECT_DOUBLE_EQ(l.scale_y, 0.5);
    // 方框里的四个角 → 原画面四个角
    EXPECT_DOUBLE_EQ(l.to_source_x(0), 0);
    EXPECT_DOUBLE_EQ(l.to_source_y(140), 0);
    EXPECT_DOUBLE_EQ(l.to_source_x(640), 1280);
    EXPECT_DOUBLE_EQ(l.to_source_y(500), 720);
}

TEST(Letterbox, PortraitIsPaddedLeftAndRight)
{
    const auto l = fit_letterbox(720, 1280, 640, 640);
    EXPECT_EQ(l.width, 360);
    EXPECT_EQ(l.height, 640);
    EXPECT_EQ(l.left, 140);
    EXPECT_EQ(l.top, 0);
}

TEST(Letterbox, EverythingEvenForYuv)
{
    // 1920x1080 → 640：640x360；奇怪的尺寸也要得到偶数
    for (const auto &[w, h] : {std::pair{1920, 1080}, std::pair{1001, 777}, std::pair{333, 1999}}) {
        const auto l = fit_letterbox(w, h, 640, 640);
        EXPECT_EQ(l.width % 2, 0) << w << "x" << h;
        EXPECT_EQ(l.height % 2, 0);
        EXPECT_EQ(l.left % 2, 0);
        EXPECT_EQ(l.top % 2, 0);
        EXPECT_LE(l.left + l.width, 640);
        EXPECT_LE(l.top + l.height, 640);
        // 换算回去的误差不超过 1 个原画面像素
        EXPECT_NEAR(l.to_source_x(l.left + l.width), w, 1.0 / l.scale_x + 1e-9);
        EXPECT_NEAR(l.to_source_y(l.top + l.height), h, 1.0 / l.scale_y + 1e-9);
    }
}

TEST(Letterbox, InvalidSizeGivesEmpty)
{
    const auto l = fit_letterbox(0, 720, 640, 640);
    EXPECT_EQ(l.width, 0);
    EXPECT_EQ(l.height, 0);
}

// CPU 版 letterbox（第 5d 步，--detect-image 和对比基线用）
TEST(Letterbox, CpuVersionFillsPadAndScalesTheImage)
{
    // 4x2 的图：左半红、右半蓝（内存 B G R X）
    rksec::media::Image src;
    src.width = 4;
    src.height = 2;
    src.stride = 16;
    src.pixels.assign(32, 0);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            std::uint8_t *p = src.pixels.data() + y * 16 + x * 4;
            p[x < 2 ? 2 : 0] = 200;
        }
    }
    const auto lb = fit_letterbox(4, 2, 8, 8);  // 放大 2 倍：8x4，上下各补 2 行
    ASSERT_EQ(lb.top, 2);
    ASSERT_EQ(lb.height, 4);
    std::vector<std::uint8_t> dst(8 * 8 * 3, 0);
    rksec::media::letterbox_to_rgb(src, lb, dst.data(), 8, 8, 8, 114);
    const auto at = [&](int x, int y, int c) { return dst[static_cast<std::size_t>((y * 8 + x) * 3 + c)]; };
    EXPECT_EQ(at(0, 0, 0), 114) << "上面的灰边";
    EXPECT_EQ(at(7, 7, 2), 114) << "下面的灰边";
    EXPECT_EQ(at(0, 3, 0), 200) << "左边是红（R 在第 0 个字节）";
    EXPECT_EQ(at(0, 3, 2), 0);
    EXPECT_EQ(at(7, 3, 2), 200) << "右边是蓝";
    EXPECT_EQ(at(7, 3, 0), 0);
    EXPECT_GT(at(3, 3, 0), 0) << "中间是红蓝之间的过渡（双线性）";
    EXPECT_GT(at(4, 3, 2), 0);
}
