#pragma once

#include <array>
#include <cstdint>
#include <string>

// 识别分数的统计（第 9a 步）：用来定阈值、判断登记照够不够。纯计算，CI 上能测。
//
// 每次识别找到了脸，记一个样本：和最像的人的相似度、和第二像的人差多少、脸多大、什么角度。
// 只记数字，不存照片。用固定大小的直方图（分数精确到 0.01），--ui 开一天内存也不涨。
//
// 怎么用：只让一个登记过的人在镜头前，正脸、侧脸、抬头低头、走近走远各待一会儿，看报告——
// - 整体的 P10（最低的 10% 在多少以下）低于阈值很多：阈值偏高，或者登记照不够
// - 某个角度 / 大小的“认出”比例特别低：补登记那个角度的照片，或者说明那个距离太远了
// - 再让没登记的人（或者手机上别人的照片）来一遍，看陌生人的分数最高到多少：
//   阈值要在“本人的低分”和“别人的高分”之间
namespace rksec::face {

struct ScoreSample {
    float score = 0;    // 和最像的人的相似度
    float second = -1;  // 和第二像的人的相似度（底库只有一个人时 -1）
    float width = 0;    // 脸宽（原画面像素）
    float yaw = 0;
    float pitch = 0;
    bool pose_valid = false;
};

class ScoreStats {
  public:
    void add(const ScoreSample &s);

    std::uint64_t count() const { return all_.n; }
    // 分数的百分位（p 是 0～1；没有样本时返回 0），精确到 0.01
    float percentile(double p) const { return all_.percentile(p); }
    // 分数 ≥ threshold 的比例（0～1）
    double ratio_at_least(float threshold) const { return all_.ratio_at_least(threshold); }

    // 多行文字报告：总体、分布、按角度、按大小、和第二像的人的差距。没有样本时一行“没有样本”
    std::string report(float threshold) const;

    // 分组（按角度、大小），report 里用；公开出来给测试
    static int yaw_group(float yaw);  // 0 正脸，1 鼻子偏画面左，2 偏右
    // 抬头低头按这台摄像头实测定（摄像头比脸低，平视 0.15～0.4，见 face_pick.hpp）
    static int pitch_group(float pitch);  // 0 平视（0.1～0.45），1 抬头（<0.1），2 低头（>0.45）
    static int size_group(float width);   // 0 <100，1 100～200，2 200～300，3 ≥300 像素

    struct Hist {
        static constexpr int kBins = 101;  // 0.00～1.00，负数算 0
        std::array<std::uint32_t, kBins> bins{};
        std::uint64_t n = 0;
        void add(float v);
        float percentile(double p) const;
        double ratio_at_least(float threshold) const;
    };

  private:
    Hist all_;
    std::array<Hist, 3> yaw_;
    std::array<Hist, 3> pitch_;
    std::array<Hist, 4> size_;
    Hist margin_;  // score - second（只有底库里有两个人以上时有）
};

}  // namespace rksec::face
