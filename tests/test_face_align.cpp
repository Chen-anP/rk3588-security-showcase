// 人脸对齐（第 7c 步）：相似变换的求解、按变换取像素、在人的框里取景。
// 对齐错了不会报错，只会让识别分数悄悄变低（同一个人认不出来），所以用合成图把每个环节钉住。
#include "face/align.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using rksec::face::ImageView;
using rksec::face::PixelFormat;
using rksec::face::Point;
using rksec::face::Region;
using rksec::face::Similarity;

namespace {

constexpr float kPi = 3.14159265F;

Similarity make(float scale, float degrees, float tx, float ty)
{
    const float r = degrees * kPi / 180;
    return {scale * std::cos(r), scale * std::sin(r), tx, ty};
}

std::array<Point, 5> move_points(const Similarity &m, const std::array<Point, 5> &pts)
{
    std::array<Point, 5> out{};
    for (std::size_t i = 0; i < pts.size(); ++i) {
        out[i] = m.apply(pts[i]);
    }
    return out;
}

// w×h 的 RGB 图，像素值按位置变化（R = x，G = y，B = 常数），方便检查取的是哪个像素
std::vector<std::uint8_t> gradient(int w, int h)
{
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w * h * 3));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto i = static_cast<std::size_t>((y * w + x) * 3);
            px[i] = static_cast<std::uint8_t>(x);
            px[i + 1] = static_cast<std::uint8_t>(y);
            px[i + 2] = 77;
        }
    }
    return px;
}

rksec::detect::Detection box(float x1, float y1, float x2, float y2)
{
    return {x1, y1, x2, y2, 0.9F, 0};
}

}  // namespace

TEST(FaceAlign, RecoversAKnownTransform)
{
    // 参考点经过“缩小一半、转 15 度、平移”得到 src；从 src 求回参考点的变换，应该正好是它的逆
    const Similarity fwd = make(2.5F, 15, 300, 120);
    const auto src = move_points(fwd, rksec::face::kArcFaceRef);
    Similarity m;
    ASSERT_TRUE(rksec::face::estimate_similarity(src, rksec::face::kArcFaceRef, m));
    EXPECT_NEAR(m.scale(), 1 / 2.5F, 1e-4);
    for (std::size_t i = 0; i < src.size(); ++i) {
        const Point p = m.apply(src[i]);
        EXPECT_NEAR(p.x, rksec::face::kArcFaceRef[i].x, 1e-3) << i;
        EXPECT_NEAR(p.y, rksec::face::kArcFaceRef[i].y, 1e-3) << i;
    }
}

TEST(FaceAlign, SamePointsGiveIdentity)
{
    Similarity m;
    ASSERT_TRUE(rksec::face::estimate_similarity(rksec::face::kArcFaceRef, rksec::face::kArcFaceRef, m));
    EXPECT_NEAR(m.a, 1, 1e-5);
    EXPECT_NEAR(m.b, 0, 1e-5);
    EXPECT_NEAR(m.tx, 0, 1e-3);
    EXPECT_NEAR(m.ty, 0, 1e-3);
}

TEST(FaceAlign, DegeneratePointsAreRejected)
{
    const std::array<Point, 5> same = {Point{10, 10}, Point{10, 10}, Point{10, 10}, Point{10, 10}, Point{10, 10}};
    Similarity m;
    EXPECT_FALSE(rksec::face::estimate_similarity(same, rksec::face::kArcFaceRef, m)) << "5 个点重合，求不出方向";
}

TEST(FaceAlign, WarpWithIdentityCopiesAndConvertsXrgb)
{
    const int w = 8;
    const int h = 6;
    const auto rgb = gradient(w, h);
    std::vector<std::uint8_t> out(rgb.size());
    rksec::face::warp_to_rgb({rgb.data(), w, h, static_cast<std::size_t>(w * 3), PixelFormat::rgb888}, Similarity{},
                             out.data(), w, h);
    EXPECT_EQ(out, rgb);

    // 同一张图存成 B G R X：输出还是 R G B
    std::vector<std::uint8_t> xrgb(static_cast<std::size_t>(w * h * 4));
    for (std::size_t i = 0; i < static_cast<std::size_t>(w * h); ++i) {
        xrgb[i * 4] = rgb[i * 3 + 2];
        xrgb[i * 4 + 1] = rgb[i * 3 + 1];
        xrgb[i * 4 + 2] = rgb[i * 3];
        xrgb[i * 4 + 3] = 255;
    }
    rksec::face::warp_to_rgb({xrgb.data(), w, h, static_cast<std::size_t>(w * 4), PixelFormat::xrgb8888}, Similarity{},
                             out.data(), w, h);
    EXPECT_EQ(out, rgb);
}

TEST(FaceAlign, WarpInterpolatesAndFillsOutsideWithBlack)
{
    const int w = 8;
    const int h = 6;
    const auto rgb = gradient(w, h);
    const ImageView src{rgb.data(), w, h, static_cast<std::size_t>(w * 3), PixelFormat::rgb888};
    std::vector<std::uint8_t> out(rgb.size());
    // 输出 = 源图往左挪半个像素：输出 (x, y) 取源图 (x + 0.5, y)，R 是 x + 0.5，四舍五入
    rksec::face::warp_to_rgb(src, Similarity{1, 0, -0.5F, 0}, out.data(), w, h);
    EXPECT_EQ(out[3 * 3], 4) << "源图 x = 3.5 → 3.5 舍入成 4";
    EXPECT_EQ(out[3 * 3 + 1], 0);
    // 最右边一列一半落在图外：图外按 0 算，(7 × 0.5 + 0 × 0.5) ≈ 4
    EXPECT_EQ(out[7 * 3], 4);
    EXPECT_EQ(out[7 * 3 + 2], 39) << "B：77 的一半";
    // 整个挪出去：全黑
    rksec::face::warp_to_rgb(src, Similarity{1, 0, 100, 0}, out.data(), w, h);
    for (const std::uint8_t v : out) {
        ASSERT_EQ(v, 0);
    }
}

TEST(FaceAlign, AlignedLandmarksLandOnTheReference)
{
    // 一张“脸”：400×300 的灰图，5 个关键点处各画一个不同颜色的小方块；脸是歪的、放大了的
    const int w = 400;
    const int h = 300;
    std::vector<std::uint8_t> img(static_cast<std::size_t>(w * h * 3), 128);
    const auto lm = move_points(make(2.2F, -20, 150, 60), rksec::face::kArcFaceRef);
    const std::array<std::array<std::uint8_t, 3>, 5> colors = {
        {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {0, 255, 255}}};
    for (std::size_t k = 0; k < lm.size(); ++k) {
        const int cx = static_cast<int>(std::lround(lm[k].x));
        const int cy = static_cast<int>(std::lround(lm[k].y));
        for (int y = cy - 4; y <= cy + 4; ++y) {
            for (int x = cx - 4; x <= cx + 4; ++x) {
                const auto i = static_cast<std::size_t>((y * w + x) * 3);
                img[i] = colors[k][0];
                img[i + 1] = colors[k][1];
                img[i + 2] = colors[k][2];
            }
        }
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(112 * 112 * 3));
    ASSERT_TRUE(rksec::face::align_face({img.data(), w, h, static_cast<std::size_t>(w * 3), PixelFormat::rgb888}, lm,
                                        out.data()));
    for (std::size_t k = 0; k < lm.size(); ++k) {
        const int x = static_cast<int>(std::lround(rksec::face::kArcFaceRef[k].x));
        const int y = static_cast<int>(std::lround(rksec::face::kArcFaceRef[k].y));
        const auto i = static_cast<std::size_t>((y * 112 + x) * 3);
        EXPECT_EQ(out[i], colors[k][0]) << "第 " << k << " 个关键点";
        EXPECT_EQ(out[i + 1], colors[k][1]) << "第 " << k << " 个关键点";
        EXPECT_EQ(out[i + 2], colors[k][2]) << "第 " << k << " 个关键点";
    }
    EXPECT_EQ(out[0], 128) << "左上角是脸周围的灰色";
}

TEST(FaceAlign, SearchRegionTakesTheUpperPartOfThePerson)
{
    // 1280×720 画面中间一个 200×500 的人：宽 220，高也是 220（正方形），从框顶上 25 像素开始
    const Region r = rksec::face::face_search_region(box(500, 100, 700, 600), 1280, 720);
    EXPECT_EQ(r.width, 220);
    EXPECT_EQ(r.height, 220);
    EXPECT_EQ(r.x, 490);
    EXPECT_EQ(r.y, 74) << "100 − 25 = 75，取偶数";
}

TEST(FaceAlign, SearchRegionLimitsAndAlignment)
{
    // 很远的人（40×100）：至少 96×96
    Region r = rksec::face::face_search_region(box(600, 300, 640, 400), 1280, 720);
    EXPECT_EQ(r.width, 96);
    EXPECT_EQ(r.height, 96);
    // 更矮的（40×60）：高也至少 96
    r = rksec::face::face_search_region(box(600, 300, 640, 360), 1280, 720);
    EXPECT_EQ(r.height, 96);
    // 只露出上半身、宽的人：高不超过人框（+ 上面 5%）
    r = rksec::face::face_search_region(box(300, 500, 900, 720), 1280, 720);
    EXPECT_LE(r.y + r.height, 720);
    EXPECT_LE(r.height, 232);
    // 贴着左上角：平移回画面里
    r = rksec::face::face_search_region(box(-10, -5, 190, 495), 1280, 720);
    EXPECT_EQ(r.x, 0);
    EXPECT_EQ(r.y, 0);
    EXPECT_EQ(r.width, 220);
    // 比画面还大：裁到画面
    r = rksec::face::face_search_region(box(-200, 0, 1500, 720), 1280, 720);
    EXPECT_EQ(r.x, 0);
    EXPECT_EQ(r.width, 1280);
    EXPECT_LE(r.height, 720);
    // 不管什么情况，高都不超过宽（缩到 320×320 时只在上下补边）
    for (const auto &b :
         {box(0, 0, 50, 700), box(600, 300, 640, 400), box(-10, -5, 190, 495), box(1200, 0, 1290, 720)}) {
        r = rksec::face::face_search_region(b, 1280, 720);
        EXPECT_LE(r.height, r.width);
        EXPECT_GE(r.width, 68) << "RGA3 输入宽至少 68";
    }
    // 奇数坐标：都取偶数
    r = rksec::face::face_search_region(box(501, 101, 703, 603), 1279, 719);
    EXPECT_EQ(r.x % 2, 0);
    EXPECT_EQ(r.y % 2, 0);
    EXPECT_EQ(r.width % 2, 0);
    EXPECT_EQ(r.height % 2, 0);
    EXPECT_LE(r.x + r.width, 1279);
}
