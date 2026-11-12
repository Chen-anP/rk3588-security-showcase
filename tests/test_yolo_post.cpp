// YOLO11 后处理（第 5d 步）：用手工构造的 int8 张量检查解框、阈值、argmax、NMS、类别过滤、坐标换算。
// 后处理错了，框会画歪、漏人或者一个人出好几个框；在板子上很难一眼看出是模型的问题还是后处理的问题。
#include "detect/yolo_post.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using rksec::detect::Detection;
using rksec::detect::PostOptions;
using rksec::detect::TensorView;

namespace {

constexpr int kModel = 640;
constexpr int kClasses = 80;
// 框张量：scale 0.1，q=100 → 10.0、q=-100 → -10.0，softmax 后几乎只有一个档位
constexpr float kBoxScale = 0.1F;
// 类别、分数和：0～1 映射到 -128～127（和真实模型的量化方式一样）
constexpr float kClsScale = 1.0F / 255.0F;
constexpr std::int32_t kClsZp = -128;

std::int8_t q_cls(float v)
{
    return static_cast<std::int8_t>(std::lround(v / kClsScale) + kClsZp);
}

// 3 个尺度（80、40、20）× 框、类别、分数和，全部“空”（分数 0）
struct FakeOutputs {
    std::vector<std::vector<std::int8_t>> data;
    std::vector<TensorView> views;
    bool with_sum = true;

    explicit FakeOutputs(bool sum = true) : with_sum(sum)
    {
        for (const int g : {80, 40, 20}) {
            const std::size_t plane = static_cast<std::size_t>(g) * static_cast<std::size_t>(g);
            data.emplace_back(64 * plane, static_cast<std::int8_t>(-100));
            data.emplace_back(kClasses * plane, q_cls(0));
            if (with_sum) {
                data.emplace_back(plane, q_cls(0));
            }
        }
        const int pb = with_sum ? 3 : 2;
        for (std::size_t k = 0; k < data.size(); ++k) {
            const int g = std::vector<int>{80, 40, 20}[k / static_cast<std::size_t>(pb)];
            const int role = static_cast<int>(k) % pb;
            TensorView v;
            v.data = data[k].data();
            v.h = g;
            v.w = g;
            v.c = role == 0 ? 64 : role == 1 ? kClasses : 1;
            v.zp = role == 0 ? 0 : kClsZp;
            v.scale = role == 0 ? kBoxScale : kClsScale;
            views.push_back(v);
        }
    }

    // 在第 branch 个尺度的格子 (i, j) 放一个目标：类别 cls 分数 score，四条边距离 l、t、r、b 个步长
    void put(int branch, int i, int j, int cls, float score, int l, int t, int r, int b, float sum = -1)
    {
        const std::size_t pb = with_sum ? 3 : 2;
        const std::size_t first = static_cast<std::size_t>(branch) * pb;  // 这个尺度的第一个张量
        const TensorView &bv = views[first];
        const std::size_t plane = static_cast<std::size_t>(bv.h) * static_cast<std::size_t>(bv.w);
        const std::size_t off =
            static_cast<std::size_t>(i) * static_cast<std::size_t>(bv.w) + static_cast<std::size_t>(j);
        auto &box = data[first];
        const std::array<int, 4> dist = {l, t, r, b};
        for (std::size_t side = 0; side < 4; ++side) {
            for (std::size_t k = 0; k < 16; ++k) {
                box[(side * 16 + k) * plane + off] =
                    static_cast<std::int8_t>(static_cast<int>(k) == dist[side] ? 100 : -100);
            }
        }
        data[first + 1][static_cast<std::size_t>(cls) * plane + off] = q_cls(score);
        if (with_sum) {
            // 分数和默认 = 原来的和 + 这个类别的分数（真实模型就是这样）；测试快速过滤时直接给一个值
            auto &s = data[first + 2][off];
            const float cur = static_cast<float>(static_cast<int>(s) - kClsZp) * kClsScale;
            s = q_cls(sum >= 0 ? sum : std::min(1.0F, cur + score));
        }
    }
};

std::vector<Detection> decode(const FakeOutputs &o, const PostOptions &opts = {})
{
    std::string err;
    auto d = rksec::detect::yolo11_decode(o.views, kModel, kModel, opts, err);
    EXPECT_TRUE(err.empty()) << err;
    return d;
}

}  // namespace

TEST(YoloPost, DflOfUniformScoresIsTheMiddle)
{
    const std::vector<float> flat(16, 0.0F);
    EXPECT_NEAR(rksec::detect::dfl_expect(flat.data(), 16), 7.5F, 1e-5);
    std::vector<float> peak(16, -10.0F);
    peak[3] = 10.0F;
    EXPECT_NEAR(rksec::detect::dfl_expect(peak.data(), 16), 3.0F, 1e-3);
}

TEST(YoloPost, EmptyOutputsGiveNoBoxes)
{
    const FakeOutputs o;
    EXPECT_TRUE(decode(o).empty());
}

TEST(YoloPost, OneObjectDecodesToTheRightBox)
{
    FakeOutputs o;
    // 80×80 的格子，步长 8；格子 (10, 20) 的中心是 (20.5, 10.5) × 8
    o.put(0, 10, 20, 0, 0.9F, 2, 3, 4, 5);
    const auto d = decode(o);
    ASSERT_EQ(d.size(), 1U);
    EXPECT_EQ(d[0].cls, 0);
    EXPECT_NEAR(d[0].score, 0.9F, 0.005F);
    EXPECT_NEAR(d[0].x1, (20.5F - 2) * 8, 0.05F);
    EXPECT_NEAR(d[0].y1, (10.5F - 3) * 8, 0.05F);
    EXPECT_NEAR(d[0].x2, (20.5F + 4) * 8, 0.05F);
    EXPECT_NEAR(d[0].y2, (10.5F + 5) * 8, 0.05F);
}

TEST(YoloPost, StrideFollowsTheGridSize)
{
    FakeOutputs o;
    o.put(2, 5, 7, 0, 0.8F, 1, 1, 1, 1);  // 20×20 的格子，步长 32
    const auto d = decode(o);
    ASSERT_EQ(d.size(), 1U);
    EXPECT_NEAR(d[0].x1, (7.5F - 1) * 32, 0.1F);
    EXPECT_NEAR(d[0].y2, (5.5F + 1) * 32, 0.1F);
}

TEST(YoloPost, ScoreThresholdIsStrict)
{
    FakeOutputs o;
    o.put(0, 1, 1, 0, 0.20F, 1, 1, 1, 1);  // 低于 0.25
    o.put(0, 40, 40, 0, 0.30F, 1, 1, 1, 1);
    const auto d = decode(o);
    ASSERT_EQ(d.size(), 1U);
    EXPECT_NEAR(d[0].score, 0.30F, 0.005F);
    PostOptions strict;
    strict.score_threshold = 0.5F;
    EXPECT_TRUE(decode(o, strict).empty());
}

TEST(YoloPost, ScoreSumSkipsCellsQuickly)
{
    // 分数和低于阈值的格子直接跳过（这是加速用的；真实模型里分数和 ≥ 任何一个类别的分数）
    FakeOutputs o;
    o.put(0, 3, 3, 0, 0.9F, 1, 1, 1, 1, /*sum=*/0.1F);
    EXPECT_TRUE(decode(o).empty());
}

TEST(YoloPost, WorksWithoutScoreSumOutputs)
{
    FakeOutputs o{false};  // 6 个输出的模型
    o.put(1, 4, 6, 2, 0.7F, 2, 2, 2, 2);
    const auto d = decode(o);
    ASSERT_EQ(d.size(), 1U);
    EXPECT_EQ(d[0].cls, 2);
}

TEST(YoloPost, BestClassWinsInACell)
{
    FakeOutputs o;
    o.put(0, 20, 20, 0, 0.5F, 2, 2, 2, 2);
    o.put(0, 20, 20, 5, 0.8F, 2, 2, 2, 2);  // 同一格子 bus 分数更高
    const auto d = decode(o);
    ASSERT_EQ(d.size(), 1U);
    EXPECT_EQ(d[0].cls, 5);
}

TEST(YoloPost, NmsRemovesDuplicatesOfTheSameClassOnly)
{
    FakeOutputs o;
    // 相邻两个格子预测同一个人（框几乎重合）
    o.put(0, 30, 30, 0, 0.9F, 3, 3, 3, 3);
    o.put(0, 30, 31, 0, 0.7F, 4, 3, 2, 3);
    // 同一位置的一辆车：类别不同，不互相抑制
    o.put(1, 15, 15, 2, 0.6F, 2, 2, 2, 2);
    const auto d = decode(o);
    ASSERT_EQ(d.size(), 2U);
    EXPECT_EQ(d[0].cls, 0);
    EXPECT_NEAR(d[0].score, 0.9F, 0.005F) << "留下分数高的";
    EXPECT_EQ(d[1].cls, 2);
}

TEST(YoloPost, ClassFilterKeepsOnlyWantedClasses)
{
    FakeOutputs o;
    o.put(0, 10, 10, 0, 0.9F, 1, 1, 1, 1);
    o.put(0, 50, 50, 2, 0.9F, 1, 1, 1, 1);
    PostOptions only_person;
    only_person.classes = {0};
    const auto d = decode(o, only_person);
    ASSERT_EQ(d.size(), 1U);
    EXPECT_EQ(d[0].cls, 0);
}

TEST(YoloPost, ResultsAreSortedAndCapped)
{
    FakeOutputs o;
    for (int k = 0; k < 10; ++k) {
        o.put(0, 5 + 7 * k, 5, 0, 0.3F + 0.05F * static_cast<float>(k), 1, 1, 1, 1);
    }
    PostOptions cap;
    cap.max_detections = 4;
    const auto d = decode(o, cap);
    ASSERT_EQ(d.size(), 4U);
    for (std::size_t i = 1; i < d.size(); ++i) {
        EXPECT_GE(d[i - 1].score, d[i].score);
    }
    EXPECT_NEAR(d[0].score, 0.75F, 0.005F);
}

TEST(YoloPost, BoxesAreClampedToTheModelInput)
{
    FakeOutputs o;
    o.put(0, 0, 0, 0, 0.9F, 5, 5, 1, 1);  // 左上角的格子，往左上伸出去
    const auto d = decode(o);
    ASSERT_EQ(d.size(), 1U);
    EXPECT_EQ(d[0].x1, 0.0F);
    EXPECT_EQ(d[0].y1, 0.0F);
}

TEST(YoloPost, WrongShapesAreRejectedWithAReason)
{
    FakeOutputs o;
    std::string err;
    std::vector<TensorView> five(o.views.begin(), o.views.begin() + 5);
    EXPECT_TRUE(rksec::detect::yolo11_decode(five, kModel, kModel, {}, err).empty());
    EXPECT_NE(err.find("9 个"), std::string::npos) << err;

    auto bad = o.views;
    bad[0].c = 32;
    err.clear();
    int classes = 0;
    EXPECT_FALSE(rksec::detect::check_yolo11_outputs(bad, kModel, kModel, classes, err));
    EXPECT_NE(err.find("64"), std::string::npos) << err;

    err.clear();
    EXPECT_FALSE(rksec::detect::check_yolo11_outputs(o.views, 320, 320, classes, err)) << "模型输入尺寸对不上";
    err.clear();
    EXPECT_TRUE(rksec::detect::check_yolo11_outputs(o.views, kModel, kModel, classes, err)) << err;
    EXPECT_EQ(classes, kClasses);
}

TEST(YoloPost, IouOfKnownBoxes)
{
    const Detection a{0, 0, 10, 10, 1, 0};
    const Detection b{5, 0, 15, 10, 1, 0};
    EXPECT_NEAR(rksec::detect::iou(a, b), 50.0F / 150.0F, 1e-6);
    EXPECT_EQ(rksec::detect::iou(a, Detection{20, 20, 30, 30, 1, 0}), 0.0F);
    EXPECT_NEAR(rksec::detect::iou(a, a), 1.0F, 1e-6);
}

TEST(YoloPost, ToSourceUndoesTheLetterbox)
{
    const auto lb = rksec::media::fit_letterbox(1280, 720, 640, 640);
    std::vector<Detection> d{{0, 140, 640, 500, 0.9F, 0}, {100, 100, 200, 600, 0.9F, 0}};
    rksec::detect::to_source(d, lb, 1280, 720);
    EXPECT_FLOAT_EQ(d[0].x1, 0);
    EXPECT_FLOAT_EQ(d[0].y1, 0);
    EXPECT_FLOAT_EQ(d[0].x2, 1280);
    EXPECT_FLOAT_EQ(d[0].y2, 720);
    // 伸到灰边里的部分被裁掉
    EXPECT_FLOAT_EQ(d[1].y1, 0);
    EXPECT_FLOAT_EQ(d[1].y2, 720);
    EXPECT_FLOAT_EQ(d[1].x1, 200);
}
