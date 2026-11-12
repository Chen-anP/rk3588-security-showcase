// 多目标跟踪（第 7a 步）：用合成的运动轨迹检查编号稳不稳。
// 跟踪错了，在界面上表现为编号乱跳；到了第 7c、8 步，会变成“同一个人被识别成两个人”“一个人触发好几次事件”。
#include "track/tracker.hpp"

#include <gtest/gtest.h>

#include <vector>

using rksec::detect::Detection;
using rksec::track::KalmanFilter;
using rksec::track::Track;
using rksec::track::Tracker;
using rksec::track::TrackerOptions;

namespace {

Detection person(double cx, double cy, float score = 0.9F, double w = 80, double h = 200, int cls = 0)
{
    Detection d;
    d.x1 = static_cast<float>(cx - w / 2);
    d.y1 = static_cast<float>(cy - h / 2);
    d.x2 = static_cast<float>(cx + w / 2);
    d.y2 = static_cast<float>(cy + h / 2);
    d.score = score;
    d.cls = cls;
    return d;
}

double center_x(const Track &t)
{
    return (t.box.x1 + t.box.x2) / 2;
}

}  // namespace

TEST(Kalman, LearnsConstantVelocity)
{
    KalmanFilter kf{rksec::track::to_box(person(100, 300))};
    for (int i = 1; i <= 20; ++i) {
        kf.predict();
        kf.update(rksec::track::to_box(person(100 + 10.0 * i, 300)));
    }
    EXPECT_NEAR(kf.vx(), 10.0, 0.5) << "每帧往右走 10 像素";
    EXPECT_NEAR(kf.vy(), 0.0, 0.5);
    kf.predict();
    EXPECT_NEAR(kf.box().cx, 310.0, 2.0) << "没有检测时按速度预测";
}

TEST(Tracker, OnePersonKeepsOneIdAfterConfirmation)
{
    Tracker tr;
    for (int f = 0; f < 60; ++f) {
        const auto out = tr.update({person(100 + 5.0 * f, 300)});
        if (f < 2) {
            EXPECT_TRUE(out.empty()) << "第 " << f << " 帧：连续 3 帧才确认，之前不显示";
        } else {
            ASSERT_EQ(out.size(), 1U) << "第 " << f << " 帧";
            EXPECT_EQ(out[0].id, 1);
        }
    }
    EXPECT_EQ(tr.stats().created, 1U);
}

TEST(Tracker, OneFrameFalseDetectionNeverGetsAnId)
{
    Tracker tr;
    EXPECT_TRUE(tr.update({person(500, 300)}).empty());
    for (int f = 0; f < 5; ++f) {
        EXPECT_TRUE(tr.update({}).empty());
    }
    EXPECT_EQ(tr.stats().created, 0U);
    EXPECT_EQ(tr.stats().active, 0U);
}

TEST(Tracker, LowScoreDetectionsDoNotStartTracks)
{
    Tracker tr;
    for (int f = 0; f < 10; ++f) {
        EXPECT_TRUE(tr.update({person(500, 300, 0.4F)}).empty());
    }
    EXPECT_EQ(tr.stats().created, 0U);
}

TEST(Tracker, LowScoreDetectionsKeepAnExistingTrackAlive)
{
    // 人被半挡住：分数掉到 0.3，已有的轨迹靠第二轮配对接着走，编号不变
    Tracker tr;
    for (int f = 0; f < 5; ++f) {
        tr.update({person(100 + 5.0 * f, 300)});
    }
    for (int f = 5; f < 15; ++f) {
        const auto out = tr.update({person(100 + 5.0 * f, 300, 0.3F)});
        ASSERT_EQ(out.size(), 1U) << "第 " << f << " 帧";
        EXPECT_EQ(out[0].id, 1);
    }
}

TEST(Tracker, CrossingPeopleKeepTheirIds)
{
    // A 从左往右、B 从右往左，中间完全重叠后分开
    Tracker tr;
    int id_a = 0;
    int id_b = 0;
    for (int f = 0; f < 60; ++f) {
        const double xa = 100 + 8.0 * f;
        const double xb = 580 - 8.0 * f;
        const auto out = tr.update({person(xa, 300), person(xb, 310)});
        if (f == 5) {
            ASSERT_EQ(out.size(), 2U);
            for (const auto &t : out) {
                (center_x(t) < 340 ? id_a : id_b) = t.id;
            }
            ASSERT_NE(id_a, id_b);
        }
        if (f == 55) {
            // 交叉以后：A 在右边，B 在左边，编号跟着人走
            for (const auto &t : out) {
                if (center_x(t) > 340) {
                    EXPECT_EQ(t.id, id_a) << "从左往右的人交叉后编号变了";
                } else {
                    EXPECT_EQ(t.id, id_b) << "从右往左的人交叉后编号变了";
                }
            }
        }
    }
    EXPECT_EQ(tr.stats().created, 2U) << "两个人，只该有两个编号";
}

TEST(Tracker, ShortOcclusionKeepsTheId)
{
    Tracker tr;
    for (int f = 0; f < 10; ++f) {
        tr.update({person(100 + 6.0 * f, 300)});
    }
    for (int f = 10; f < 20; ++f) {  // 被挡住 10 帧（约 0.4 秒）
        EXPECT_TRUE(tr.update({}).empty());
    }
    const auto out = tr.update({person(100 + 6.0 * 20, 300)});  // 在预测的位置重新出现
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].id, 1);
    EXPECT_EQ(tr.stats().recovered, 1U);
}

TEST(Tracker, LongAbsenceGetsANewId)
{
    TrackerOptions o;
    o.max_lost = 30;
    Tracker tr{o};
    for (int f = 0; f < 10; ++f) {
        tr.update({person(300, 300)});
    }
    for (int f = 0; f < 31; ++f) {
        tr.update({});
    }
    EXPECT_EQ(tr.stats().removed, 1U);
    EXPECT_EQ(tr.stats().active, 0U);
    std::vector<Track> out;
    for (int f = 0; f < 3; ++f) {
        out = tr.update({person(300, 300)});
    }
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].id, 2) << "删掉的编号不再用";
}

TEST(Tracker, DifferentClassesAreNotMatched)
{
    Tracker tr;
    for (int f = 0; f < 5; ++f) {
        tr.update({person(300, 300)});
    }
    // 同一个位置出现的是别的类别：不能接到人的轨迹上
    const auto out = tr.update({person(300, 300, 0.9F, 80, 200, 2)});
    EXPECT_TRUE(out.empty());
}

TEST(Tracker, OutputIsSortedById)
{
    Tracker tr;
    std::vector<Track> out;
    for (int f = 0; f < 5; ++f) {
        out = tr.update({person(900, 300), person(100, 300), person(500, 300)});
    }
    ASSERT_EQ(out.size(), 3U);
    EXPECT_LT(out[0].id, out[1].id);
    EXPECT_LT(out[1].id, out[2].id);
}
