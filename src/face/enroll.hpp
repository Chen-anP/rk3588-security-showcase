#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "face/gallery.hpp"

// 登记时怎么选照片、怎么检查（第 7d 步）。纯逻辑，CI 上测。
//
// 摄像头登记：连续取几十帧，每帧找脸、算特征，再从里面挑几张：
// - 只挑“画面里正好一张脸、脸够大、检测分数够高”的帧（有别人入镜、太远、模糊的不要）
// - 按检测分数从高到低挑，挑出来的帧之间至少隔 min_gap 帧（让人稍微转头，几张角度不同；
//   连续几帧几乎一样，多存也没用）
// 第 7c 步的经验：只用手机照片登记时，摄像头画面里本人的分数贴着阈值；加两张摄像头拍的就稳定了。
//
// 挑完再检查两件事，不通过就不登记：
// - 这几张像不像同一个人（两两相似度都 ≥ min_self_similarity）：防止中途换了人
// - 像不像底库里的别人（≥ 识别阈值）：同一个人登记成两个名字，识别时两个名字会来回跳
namespace rksec::face {

struct EnrollOptions {
    int want = 3;            // 挑几张
    int min_gap = 15;        // 挑出来的帧至少隔几帧（25 fps 约 0.6 秒）
    float min_width = 100;   // 脸宽至少多少像素（原画面）
    float min_score = 0.9F;  // 人脸检测分数至少多少
    // 挑出来的几张两两之间的相似度至少多少。7c 实测：同一个人手机照片和摄像头画面之间低到 0.43，
    // 不同人最高 0.15，取中间偏低的 0.3（只为拦住“中途换了人”，不是识别阈值）
    float min_self_similarity = 0.3F;
};

// 一帧（或一张照片）的结果
struct EnrollCandidate {
    int frame = 0;
    int faces = 0;    // 这帧检测到几张脸
    float score = 0;  // 最大那张脸的检测分数
    float width = 0;  // 最大那张脸的宽度
    std::vector<float> embedding;
};

// 挑出来的下标（按帧号排序）。不够 want 张时有几张返回几张
std::vector<std::size_t> pick_enroll_frames(const std::vector<EnrollCandidate> &c, const EnrollOptions &opts);

// 为什么一帧不能用（"" 表示能用）。给登记时打印原因
std::string reject_reason(const EnrollCandidate &c, const EnrollOptions &opts);

struct EnrollCheck {
    bool ok = false;
    std::string reason;         // 不通过的原因
    float min_self = 1;         // 挑出来的几张之间最低的相似度
    std::string closest_other;  // 底库里最像的别人
    float closest_score = -1;   // 和他的相似度
};

// embs：挑出来的特征（已归一化）；existing：现在的底库；name：要登记的名字（同名的不算“别人”）
EnrollCheck check_enroll(const std::vector<std::vector<float>> &embs, const std::string &name, const Gallery &existing,
                         float threshold, const EnrollOptions &opts);

}  // namespace rksec::face
