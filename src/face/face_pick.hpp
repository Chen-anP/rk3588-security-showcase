#pragma once

#include <cstdint>
#include <vector>

#include "detect/yolo_post.hpp"
#include "face/retinaface.hpp"

// 在一个人的框里挑哪张脸去识别（第 9a 步）。纯计算，CI 上能测。
//
// 第 7c 步取的是面积最大的脸。实测问题：人举着手机（手机里有人脸）时，手机离镜头近、脸更大，
// 被拿去识别，编号被换成手机里那个人的名字，录像里的“陌生人编号”也是错的。
//
// 现在的规则：脸要在这个人自己的头部位置——
// - 水平：脸的中心离人框中线不超过人框宽的 kMaxOffsetX（举在身体旁边的手机、旁边别人的脸排除）
// - 垂直：脸的中心在人框上面 kMaxDepthY 以内（举在胸前、腰间的手机排除）。
//   例外：脸宽超过人框宽的 kHeadOnlyRatio 时，人框基本只剩头（离镜头 1 米以内），
//   脸中心本来就在人框中下部（11/06 实测深度 0.60～0.90、宽度比 0.55～0.78，被误排除 18 次），
//   这时只要求脸中心在人框里（深度 ≤ 1）
// - 大小：脸宽至少是人框宽的 kMinWidthRatio（后面路过的人的小脸排除）
// 符合的脸里取“最靠上、最靠中线”的（cost = 竖直位置 + 0.5 × 水平偏离，都按人框大小归一化）：
// 一个人的脸总在他自己身体的最上面，手机举得再高也多半在脸的下面或旁边。
// 都不符合时返回 -1：当作“没找到脸”（不识别），而不是拿一张不像是他的脸去认成陌生人。
//
// 坐标：脸和人框要在同一个坐标系（原画面像素）。
namespace rksec::face {

inline constexpr float kMaxOffsetX = 0.35F;
inline constexpr float kMaxDepthY = 0.6F;
inline constexpr float kMinWidthRatio = 0.12F;
inline constexpr float kHeadOnlyRatio = 0.5F;

int pick_face(const std::vector<Face> &faces, const detect::Detection &person);

// 同上，另外给出“最接近条件的那张脸”的三个比例（排查规则是不是太严：被排除的脸差在哪）。
// index 是挑中的（-1 没挑中）；nearest 是代价最小的那张（不管符不符合条件，没有脸时 -1）
struct PickResult {
    int index = -1;
    int nearest = -1;
    float offset_x = 0;     // 离人框中线 ÷ 人框宽（限 kMaxOffsetX）
    float depth_y = 0;      // 脸中心在人框里多深 ÷ 人框高（限 kMaxDepthY；人框只剩头时限 1）
    float width_ratio = 0;  // 脸宽 ÷ 人框宽（至少 kMinWidthRatio）
};
PickResult pick_face_detail(const std::vector<Face> &faces, const detect::Detection &person);

// 粗略的头部姿态，用 5 个关键点估计（统计“哪种角度认不出来”用，不精确）：
// - yaw：鼻尖相对两眼中点的水平偏移 ÷ 两眼距离。正脸约 0，侧脸绝对值变大（±0.3 已经很侧了）。
//   正负是画面里的左右（鼻子偏向画面左边为负），不是人自己的左右
// - pitch：鼻尖到两眼连线的竖直距离 ÷ 两眼到嘴角的竖直距离。平视大约 0.5～0.6，
//   抬头时鼻尖在画面里靠近眼睛（变小），低头时变大
struct Pose {
    float yaw = 0;
    float pitch = 0;
    bool valid = false;  // 关键点退化（两眼重合、嘴在眼睛上面）时 false
};
Pose estimate_pose(const Face &f);

// 这张脸值不值得拿去识别（第 9a 步上板后加）。实测（11/06）本人的低分几乎都来自三种脸：
// 抬头抬得很高（镜头看到下巴底下，0.15、0.24）、走过镜头时糊的或者只露出一部分（0.03、0.12）、
// 关键点乱了（姿态算出 -5.33 这种不可能的数）。这些脸认出来的分数没有参考价值，
// 拿去比只会把本人认成陌生人。质量不够时当作“没找到脸”，过一会儿再试。
enum class Quality : std::uint8_t {
    ok,
    bad_landmarks,  // 关键点退化或姿态不可能（脸检测出错）
    low_score,      // 人脸检测的置信度低
    at_edge,        // 眼睛、鼻子或嘴角贴着 / 出了画面边（只拍到一部分）
    turned,         // 左右转头太大
    tilted,         // 抬头 / 低头太大
};
inline constexpr int kQualityCount = 6;

struct QualityLimits {
    float min_score = 0.7F;  // 人脸检测置信度（检测阈值是 0.5）
    int edge = 2;            // 关键点离画面边几个像素以内算贴边（至少脸宽的 5%）
    float max_yaw = 0.6F;    // 实测转头 0.47 还有 0.59 分，0.6 以上基本是看不到另一只眼
    // pitch 跟摄像头高度有关：这台摄像头比脸低，平视时 0.15～0.4（正对时理论值约 0.5）；
    // 抬头到 0 以下（-0.04、-0.24）分数掉到 0.24、0.15
    float min_pitch = 0.0F;
    float max_pitch = 1.0F;
};

// face 是原画面坐标的脸（框和关键点都要换算过），pose 是 estimate_pose 的结果
Quality face_quality(const Face &face, const Pose &pose, int frame_w, int frame_h, const QualityLimits &lim = {});
const char *quality_text(Quality q);

}  // namespace rksec::face
