// 选脸、头部姿态、识别分数统计（第 9a 步）。

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "face/align.hpp"
#include "face/face_pick.hpp"
#include "face/score_stats.hpp"

using rksec::detect::Detection;
using rksec::face::Face;
using rksec::face::pick_face;
using rksec::face::ScoreSample;
using rksec::face::ScoreStats;

namespace {

// 中心 (cx, cy)、宽 w 的脸（高 1.2 倍），关键点按正脸摆
Face face_at(float cx, float cy, float w)
{
    Face f;
    const float h = w * 1.2F;
    f.box = Detection{cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, 0.9F, 0};
    f.landmarks = {{{cx - 0.2F * w, cy - 0.15F * h},
                    {cx + 0.2F * w, cy - 0.15F * h},
                    {cx, cy + 0.05F * h},
                    {cx - 0.15F * w, cy + 0.25F * h},
                    {cx + 0.15F * w, cy + 0.25F * h}}};
    return f;
}

// 离镜头 2 米左右、整个人在画面里：人框 400～600 × 100～700
const Detection kStanding{400, 100, 600, 700, 0.9F, 0};
// 走到镜头前：只剩头和肩膀，框到画面底
const Detection kClose{300, 50, 900, 720, 0.9F, 0};

}  // namespace

TEST(FacePick, OnlyFaceInTheHeadPositionIsPicked)
{
    EXPECT_EQ(pick_face({face_at(500, 140, 50)}, kStanding), 0);
    EXPECT_EQ(pick_face({}, kStanding), -1);
}

TEST(FacePick, PhoneHeldAtTheChestLosesToTheRealFaceEvenIfBigger)
{
    // 手机里的脸在胸前、比本人的脸大（离镜头近）：7c 的“最大的脸”会选错
    const std::vector<Face> faces{face_at(510, 260, 80), face_at(500, 140, 50)};
    EXPECT_EQ(rksec::face::largest_face(faces), 0);
    EXPECT_EQ(pick_face(faces, kStanding), 1);
}

TEST(FacePick, PhoneHeldNextToTheHeadLosesToTheCentredFace)
{
    // 手机举到脸旁边、差不多高：选靠中线的
    const std::vector<Face> faces{face_at(560, 135, 55), face_at(500, 140, 50)};
    EXPECT_EQ(pick_face(faces, kStanding), 1);
}

TEST(FacePick, CloseUpStillPicksTheOwnFace)
{
    // 走近时脸占了人框上面一大块；手机在下面
    const std::vector<Face> faces{face_at(600, 560, 200), face_at(600, 230, 260)};
    EXPECT_EQ(pick_face(faces, kClose), 1);
}

TEST(FacePick, FacesThatCannotBeThePersonsOwnAreRejected)
{
    // 只有举在腰间的手机里有脸（本人背对或低头）：不拿手机里的脸去认
    EXPECT_EQ(pick_face({face_at(500, 520, 70)}, kStanding), -1);
    // 人框外面的别人的脸
    EXPECT_EQ(pick_face({face_at(400, 140, 50)}, kStanding), -1);
    // 后面路过的人的小脸（比人框宽的 12% 还小），哪怕位置更靠上
    EXPECT_EQ(pick_face({face_at(500, 110, 15), face_at(500, 150, 50)}, kStanding), 1);
    // 退化的人框
    EXPECT_EQ(pick_face({face_at(500, 140, 50)}, Detection{10, 10, 10, 50, 0.9F, 0}), -1);
}

TEST(FacePick, SearchRegionCarriesThePerson)
{
    const auto r = rksec::face::face_search_region(kStanding, 1280, 720);
    EXPECT_FLOAT_EQ(r.person.x1, kStanding.x1);
    EXPECT_FLOAT_EQ(r.person.y2, kStanding.y2);
}

TEST(FacePose, FrontalTurnedAndTilted)
{
    const Face f = face_at(100, 100, 50);
    const auto p = rksec::face::estimate_pose(f);
    ASSERT_TRUE(p.valid);
    EXPECT_NEAR(p.yaw, 0.0F, 1e-4F);
    EXPECT_NEAR(p.pitch, 0.5F, 1e-4F);  // 鼻尖在眼睛和嘴中间

    Face turned = f;
    turned.landmarks[2].x -= 8;  // 鼻子往画面左边偏两眼距离（20）的 40%
    EXPECT_NEAR(rksec::face::estimate_pose(turned).yaw, -0.4F, 1e-4F);

    Face up = f;
    up.landmarks[2].y -= 9;  // 鼻尖靠近眼睛：抬头
    EXPECT_LT(rksec::face::estimate_pose(up).pitch, 0.4F);

    Face broken = f;
    broken.landmarks[1] = broken.landmarks[0];  // 两眼重合
    EXPECT_FALSE(rksec::face::estimate_pose(broken).valid);
}

TEST(ScoreStats, PercentilesAndRatio)
{
    ScoreStats s;
    EXPECT_EQ(s.count(), 0U);
    EXPECT_FLOAT_EQ(s.percentile(0.5), 0.0F);
    EXPECT_NE(s.report(0.4F).find("没有样本"), std::string::npos);
    for (int i = 1; i <= 10; ++i) {  // 0.10 0.20 … 1.00
        s.add({static_cast<float>(i) / 10.0F, -1, 80, 0, 0.5F, true});
    }
    EXPECT_EQ(s.count(), 10U);
    EXPECT_FLOAT_EQ(s.percentile(0.1), 0.1F);
    EXPECT_FLOAT_EQ(s.percentile(0.5), 0.5F);
    EXPECT_FLOAT_EQ(s.percentile(0.9), 0.9F);
    EXPECT_FLOAT_EQ(s.percentile(1.0), 1.0F);
    EXPECT_DOUBLE_EQ(s.ratio_at_least(0.4F), 0.7);  // 0.4～1.0 七个，0.40 本身算达到
    EXPECT_DOUBLE_EQ(s.ratio_at_least(0.0F), 1.0);
    s.add({-0.3F, -1, 80, 0, 0.5F, true});  // 负分算 0
    EXPECT_FLOAT_EQ(s.percentile(0.01), 0.0F);
}

TEST(ScoreStats, GroupsBoundaries)
{
    EXPECT_EQ(ScoreStats::yaw_group(0.0F), 0);
    EXPECT_EQ(ScoreStats::yaw_group(-0.2F), 1);
    EXPECT_EQ(ScoreStats::yaw_group(0.2F), 2);
    EXPECT_EQ(ScoreStats::pitch_group(0.25F), 0);  // 这台摄像头平视
    EXPECT_EQ(ScoreStats::pitch_group(0.05F), 1);
    EXPECT_EQ(ScoreStats::pitch_group(0.5F), 2);
    EXPECT_EQ(ScoreStats::size_group(99), 0);
    EXPECT_EQ(ScoreStats::size_group(100), 1);
    EXPECT_EQ(ScoreStats::size_group(200), 2);
    EXPECT_EQ(ScoreStats::size_group(300), 3);
}

TEST(ScoreStats, ReportShowsWhereRecognitionFails)
{
    ScoreStats s;
    for (int i = 0; i < 20; ++i) {
        s.add({0.62F, 0.20F, 120, 0.0F, 0.55F, true});  // 正脸、近：分数高
    }
    for (int i = 0; i < 10; ++i) {
        s.add({0.31F, 0.18F, 50, -0.35F, 0.55F, true});  // 侧脸、远：分数低
    }
    s.add({0.5F, -1, 80, 0, 0, false});  // 关键点退化：只算大小，不算角度
    const std::string r = s.report(0.4F);
    EXPECT_NE(r.find("有脸 31 次"), std::string::npos) << r;
    EXPECT_NE(r.find("正脸 20 次 P50 0.62 认出 100%"), std::string::npos) << r;
    EXPECT_NE(r.find("鼻子偏画面左 10 次 P50 0.31 认出 0%"), std::string::npos) << r;
    EXPECT_EQ(r.find("鼻子偏画面右"), std::string::npos) << "没有样本的组不显示";
    EXPECT_NE(r.find("脸宽 <100 11 次"), std::string::npos) << r;
    EXPECT_NE(r.find("← 阈值在这一格"), std::string::npos) << r;
    EXPECT_NE(r.find("和第二像的人差"), std::string::npos) << r;
}

TEST(FaceQuality, RejectsFacesNotWorthRecognizing)
{
    using rksec::face::face_quality;
    using rksec::face::Quality;
    const Face f = face_at(500, 140, 50);
    const rksec::face::Pose good{0.05F, 0.25F, true};
    EXPECT_EQ(face_quality(f, good, 1280, 720), Quality::ok);
    // 关键点乱了：实测算出过 pitch -5.33；退化的关键点 valid = false
    EXPECT_EQ(face_quality(f, {0.0F, -5.33F, true}, 1280, 720), Quality::bad_landmarks);
    EXPECT_EQ(face_quality(f, {0.0F, 0.0F, false}, 1280, 720), Quality::bad_landmarks);
    EXPECT_EQ(face_quality(f, {2.0F, 0.3F, true}, 1280, 720), Quality::bad_landmarks);
    Face weak = f;
    weak.box.score = 0.6F;
    EXPECT_EQ(face_quality(weak, good, 1280, 720), Quality::low_score);
    EXPECT_EQ(face_quality(face_at(5, 140, 40), good, 1280, 720), Quality::at_edge) << "左眼出了画面";
    EXPECT_EQ(face_quality(face_at(500, 715, 50), good, 1280, 720), Quality::at_edge) << "嘴出了画面";
    // 离得近：脸框上沿（额头）碰到画面顶，但眼睛、鼻子、嘴都在画面里——能识别（11/06 第二轮实测）
    Face close = face_at(640, 230, 400);
    close.box.y1 = 0;
    EXPECT_EQ(face_quality(close, good, 1280, 720), Quality::ok);
    EXPECT_EQ(face_quality(f, {0.7F, 0.25F, true}, 1280, 720), Quality::turned);
    EXPECT_EQ(face_quality(f, {-0.47F, 0.25F, true}, 1280, 720), Quality::ok) << "实测转头 0.47 还有 0.59 分";
    EXPECT_EQ(face_quality(f, {0.0F, -0.04F, true}, 1280, 720), Quality::tilted) << "实测抬头到这里分数 0.24";
    EXPECT_EQ(face_quality(f, {0.0F, 1.2F, true}, 1280, 720), Quality::tilted);
    // 限值可以调
    rksec::face::QualityLimits loose;
    loose.min_pitch = -0.1F;
    EXPECT_EQ(face_quality(f, {0.0F, -0.04F, true}, 1280, 720, loose), Quality::ok);
}

TEST(FaceQuality, EveryReasonHasItsOwnText)
{
    std::vector<std::string> seen;
    for (int q = 0; q < rksec::face::kQualityCount; ++q) {
        const std::string t = rksec::face::quality_text(static_cast<rksec::face::Quality>(q));
        EXPECT_NE(t, "未知");
        EXPECT_EQ(std::find(seen.begin(), seen.end(), t), seen.end()) << t;
        seen.push_back(t);
    }
}

TEST(FacePick, DetailTellsWhyFacesWereRejected)
{
    // 只有腰间的手机：没挑中，最接近的那张深 0.7（超过 0.6）
    const auto waist = rksec::face::pick_face_detail({face_at(500, 520, 70)}, kStanding);
    EXPECT_EQ(waist.index, -1);
    EXPECT_EQ(waist.nearest, 0);
    EXPECT_NEAR(waist.depth_y, 0.7F, 1e-4F);
    EXPECT_NEAR(waist.offset_x, 0.0F, 1e-4F);
    EXPECT_NEAR(waist.width_ratio, 0.35F, 1e-4F);
    // 挑中时 nearest 就是挑中的那张，比例是它的
    const auto ok = rksec::face::pick_face_detail({face_at(510, 260, 80), face_at(500, 140, 50)}, kStanding);
    EXPECT_EQ(ok.index, 1);
    EXPECT_EQ(ok.nearest, 1);
    EXPECT_NEAR(ok.depth_y, 40.0F / 600.0F, 1e-4F);
    // 没有脸
    EXPECT_EQ(rksec::face::pick_face_detail({}, kStanding).nearest, -1);
}

TEST(FacePick, HeadOnlyPersonBoxDoesNotLimitDepth)
{
    // 离镜头 1 米以内：人框基本就是头，脸宽是人框的 0.7、脸中心在人框 0.66 深处（11/06 实测）
    const Detection head{400, 0, 1000, 720, 0.9F, 0};
    const float w = 0.7F * 600;
    const float cy = 0.66F * 720;  // 脸中心在人框 0.66 深处
    EXPECT_EQ(pick_face({face_at(700, cy, w)}, head), 0);
    const auto d = rksec::face::pick_face_detail({face_at(700, cy, w)}, head);
    EXPECT_NEAR(d.width_ratio, 0.7F, 1e-3F);
    EXPECT_NEAR(d.depth_y, 0.66F, 1e-3F);
    // 脸中心跑出人框下面了也不行
    EXPECT_EQ(pick_face({face_at(700, 760, w)}, head), -1);
    // 宽度比不到一半（站着时胸前的手机那种）：深度照样限 0.6
    EXPECT_EQ(pick_face({face_at(500, 520, 98)}, kStanding), -1);
}
