// RetinaFace 后处理（第 7b 步）：先验框、解码、NMS、换算回原画面。
// 先验框的生成规则和官方 demo 的那张 4200 行的表（rknn_box_priors.h）逐项对比过（容器里一次性核对，最大差 0）；
// 这里检查几个有代表性的位置，防止以后改坏。
#include "face/retinaface.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using rksec::face::Face;
using rksec::face::Prior;
using rksec::face::RetinaFaceOptions;

namespace {

constexpr int kSize = 320;

// 全是“背景”的输出：框和关键点偏移为 0，人脸概率为 0
struct FakeOut {
    std::vector<float> loc;
    std::vector<float> conf;
    std::vector<float> landm;

    explicit FakeOut(std::size_t n) : loc(n * 4, 0.0F), conf(n * 2, 0.0F), landm(n * 10, 0.0F)
    {
        for (std::size_t i = 0; i < n; ++i) {
            conf[i * 2] = 1.0F;  // 背景
        }
    }
    void face(std::size_t i, float score)
    {
        conf[i * 2] = 1.0F - score;
        conf[i * 2 + 1] = score;
    }
};

}  // namespace

TEST(RetinaFace, PriorsMatchTheOfficialTable)
{
    const auto p = rksec::face::retinaface_priors(kSize, kSize);
    ASSERT_EQ(p.size(), 4200U) << "(40×40 + 20×20 + 10×10) × 2";
    // 官方表的第 0、1、2 行
    EXPECT_FLOAT_EQ(p[0][0], 0.0125F);
    EXPECT_FLOAT_EQ(p[0][2], 0.05F);
    EXPECT_FLOAT_EQ(p[1][2], 0.1F) << "同一个格子的第二个先验框更大";
    EXPECT_FLOAT_EQ(p[2][0], 0.0375F) << "下一个格子往右一个步长";
    // 第二层（步长 16）从第 3200 个开始，最后一个是第三层右下角的大框
    EXPECT_FLOAT_EQ(p[3200][0], 0.025F);
    EXPECT_FLOAT_EQ(p[3200][2], 0.2F);
    EXPECT_FLOAT_EQ(p[4199][0], 0.95F);
    EXPECT_FLOAT_EQ(p[4199][2], 1.6F);
}

TEST(RetinaFace, ZeroOffsetsGiveThePriorBox)
{
    const auto pri = rksec::face::retinaface_priors(kSize, kSize);
    FakeOut o{pri.size()};
    const std::size_t i = 3200 + 2 * (5 * 20 + 7);  // 第二层，第 5 行第 7 列，小的那个先验框（64 像素）
    o.face(i, 0.9F);
    const auto faces =
        rksec::face::retinaface_decode(o.loc.data(), o.conf.data(), o.landm.data(), pri, kSize, kSize, {});
    ASSERT_EQ(faces.size(), 1U);
    const Face &f = faces[0];
    const float cx = (7 + 0.5F) * 16;
    const float cy = (5 + 0.5F) * 16;
    EXPECT_NEAR(f.box.x1, cx - 32, 1e-3);
    EXPECT_NEAR(f.box.y2, cy + 32, 1e-3);
    EXPECT_NEAR(f.box.score, 0.9F, 1e-6);
    for (const auto &pt : f.landmarks) {  // 偏移 0：关键点都在先验框中心
        EXPECT_NEAR(pt.x, cx, 1e-3);
        EXPECT_NEAR(pt.y, cy, 1e-3);
    }
}

TEST(RetinaFace, OffsetsUseTheVariances)
{
    const auto pri = rksec::face::retinaface_priors(kSize, kSize);
    FakeOut o{pri.size()};
    const std::size_t i = 0;  // 左上角，16 像素的先验框，中心 (4, 4)
    o.face(i, 0.8F);
    o.loc[0] = 10.0F;                  // 中心右移 10 × 0.1 × 16 = 16 像素
    o.loc[2] = std::log(2.0F) / 0.2F;  // 宽 = 16 × exp(log2) = 32
    o.landm[0] = -5.0F;                // 左眼 x = 4 − 5 × 0.1 × 16 = −4
    const auto faces =
        rksec::face::retinaface_decode(o.loc.data(), o.conf.data(), o.landm.data(), pri, kSize, kSize, {});
    ASSERT_EQ(faces.size(), 1U);
    EXPECT_NEAR((faces[0].box.x1 + faces[0].box.x2) / 2, 20.0F, 1e-3);
    EXPECT_NEAR(faces[0].box.x2 - faces[0].box.x1, 32.0F, 1e-3);
    EXPECT_NEAR(faces[0].landmarks[0].x, -4.0F, 1e-3) << "关键点在这里不裁剪（换算回原画面时才裁）";
}

TEST(RetinaFace, ThresholdAndNms)
{
    const auto pri = rksec::face::retinaface_priors(kSize, kSize);
    FakeOut o{pri.size()};
    const std::size_t a = 3200 + 2 * (5 * 20 + 7);
    o.face(a, 0.95F);
    o.face(a + 1, 0.9F);  // 同一个格子的大先验框：和 a 重叠 25%（64² / 128²），不算重复
    o.face(a + 2, 0.7F);  // 右边一格的小先验框：和 a 重叠很多，算重复
    o.face(100, 0.4F);    // 低于 0.5
    const auto faces =
        rksec::face::retinaface_decode(o.loc.data(), o.conf.data(), o.landm.data(), pri, kSize, kSize, {});
    ASSERT_EQ(faces.size(), 2U);
    EXPECT_NEAR(faces[0].box.score, 0.95F, 1e-6);
    EXPECT_NEAR(faces[1].box.score, 0.9F, 1e-6);
}

TEST(RetinaFace, OutputShapesAreChecked)
{
    std::string err;
    EXPECT_TRUE(rksec::face::check_retinaface_outputs({{1, 4200, 4, 0}, {1, 4200, 2, 0}, {1, 4200, 10, 0}}, 4200, err))
        << err;
    EXPECT_FALSE(rksec::face::check_retinaface_outputs({{1, 4200, 4}, {1, 4200, 2}}, 4200, err));
    EXPECT_FALSE(rksec::face::check_retinaface_outputs({{1, 16800, 4}, {1, 16800, 2}, {1, 16800, 10}}, 4200, err))
        << "640 输入的模型配 320 的先验框";
    EXPECT_NE(err.find("4200"), std::string::npos) << err;
}

TEST(RetinaFace, ToSourceMapsBoxAndLandmarks)
{
    // 官方 demo 的测试图 640×427 → 320×320：宽缩小一半；高 427 → 212（取偶数），上面补 54 行
    const auto lb = rksec::media::fit_letterbox(640, 427, kSize, kSize);
    ASSERT_EQ(lb.top, 54);
    ASSERT_EQ(lb.height, 212);
    const double sy = 427.0 / 212.0;
    Face f;
    f.box = {151, 90, 238, 202, 0.99F, 0};
    f.landmarks[2] = {200, 150};
    f.landmarks[4] = {200, 20};  // 落在灰边里：裁到画面顶上
    std::vector<Face> v{f};
    rksec::face::to_source(v, lb, 640, 427);
    EXPECT_NEAR(v[0].box.x1, 302, 1e-3);
    EXPECT_NEAR(v[0].box.x2, 476, 1e-3);
    EXPECT_NEAR(v[0].box.y1, (90 - 54) * sy, 1e-3);
    EXPECT_NEAR(v[0].box.y2, (202 - 54) * sy, 1e-3);
    EXPECT_NEAR(v[0].landmarks[2].x, 400, 1e-3);
    EXPECT_NEAR(v[0].landmarks[2].y, (150 - 54) * sy, 1e-3);
    EXPECT_EQ(v[0].landmarks[4].y, 0.0F);
}

TEST(RetinaFace, LargestFace)
{
    EXPECT_EQ(rksec::face::largest_face({}), -1);
    std::vector<Face> v(3);
    v[0].box = {0, 0, 10, 10, 0.99F, 0};
    v[1].box = {0, 0, 30, 20, 0.6F, 0};  // 分数低但最大：离得最近
    v[2].box = {0, 0, 20, 20, 0.9F, 0};
    EXPECT_EQ(rksec::face::largest_face(v), 1);
}
