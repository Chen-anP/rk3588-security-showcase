#pragma once

#include <cstdint>
#include <vector>

#include "detect/yolo_post.hpp"
#include "track/kalman.hpp"

// 多目标跟踪（第 7a 步）：给每个人一个稳定的编号，同一个人在不同帧里编号不变。
// 思路是 ByteTrack（2022），简化实现，不依赖第三方库：
//
//   每一帧：
//   1. 所有轨迹用卡尔曼滤波预测这一帧的位置
//   2. 第一轮：高分框（≥ high_score）和“已确认 + 暂时丢失”的轨迹按 IoU 配对
//   3. 第二轮：第一轮没配上的轨迹，再和低分框（score_threshold ～ high_score）配对
//      ——人被半挡住时分数会掉，低分框不新建轨迹，但能让已有的轨迹接着走（ByteTrack 的关键）
//   4. 第三轮：新出生的（待确认）轨迹和剩下的高分框配对
//   5. 还剩的高分框（≥ new_track_score）新建轨迹（待确认）
//   6. 待确认的轨迹连续 confirm_hits 帧都配上才确认、才对外显示编号；中途一帧没配上就删掉（防误检）
//   7. 已确认的轨迹没配上就变“暂时丢失”，继续预测；超过 max_lost 帧还没找回来才删除
//
// 配对用贪心：所有 (轨迹, 框) 按 IoU 从大到小，依次配，各自只用一次。人数少（≤ 十几个）时
// 和匈牙利算法（全局最优）结果几乎一样，简单、好测；人多时再换（记在未完成清单）。
//
// 编号从 1 开始只增不减，删掉的编号不再用（日志、事件里同一个编号不会指两个人）。
// 只在一个线程里用（流水线里放在按顺序放出结果的地方）。
namespace rksec::track {

struct TrackerOptions {
    float high_score = 0.5F;       // 第一轮用的框的最低分数
    float new_track_score = 0.6F;  // 新建轨迹的最低分数（比 high_score 高一点，减少误检出轨迹）
    float match_iou = 0.2F;        // 第一、三轮：IoU 至少这么多才算配上
    float low_match_iou = 0.5F;    // 第二轮（低分框）：要求更严
    int confirm_hits = 3;          // 连续配上几帧才确认
    int max_lost = 30;             // 丢失多少帧后删除（25 fps 约 1.2 秒）
};

struct Track {
    int id = 0;             // 确认后才有编号（待确认的是 0）
    detect::Detection box;  // 这一帧配上的检测框（原画面坐标、分数、类别）
    double vx = 0;          // 估计的速度（像素/帧），调试和以后的“朝哪个方向走”用
    double vy = 0;
    int hits = 0;  // 一共配上多少帧
    int age = 0;   // 出生以来多少帧
};

class Tracker {
  public:
    explicit Tracker(TrackerOptions opts = {});

    // 输入这一帧的检测框（同一帧、按帧顺序调用），返回这一帧配上了的、已确认的轨迹（按编号从小到大）
    std::vector<Track> update(const std::vector<detect::Detection> &dets);

    struct Stats {
        std::uint64_t created = 0;    // 确认过的轨迹总数（大致等于走过的人数）
        std::uint64_t removed = 0;    // 删除的已确认轨迹数
        std::uint64_t recovered = 0;  // 丢失后又找回来的次数（被挡住又出现）
        std::size_t active = 0;       // 现在的已确认轨迹（含暂时丢失）
    };
    Stats stats() const;

  private:
    enum class State : std::uint8_t { tentative, confirmed, lost };
    struct Slot {
        explicit Slot(const Box &first) : kf(first) {}
        KalmanFilter kf;
        State state = State::tentative;
        int id = 0;
        int hits = 0;
        int streak = 0;  // 连续配上的帧数
        int age = 0;
        int lost = 0;  // 连续没配上的帧数
        detect::Detection last;
    };

    // 在 tracks（下标）和 dets（下标）之间贪心配对；配上的从两个列表里去掉
    void match(std::vector<std::size_t> &tracks, std::vector<std::size_t> &dets,
               const std::vector<detect::Detection> &all, float min_iou,
               std::vector<std::pair<std::size_t, std::size_t>> &pairs) const;

    TrackerOptions opts_;
    std::vector<Slot> slots_;
    int next_id_ = 1;
    Stats stats_;
};

// 检测框（左上、右下）和卡尔曼的框（中心、宽高比、高）互相换算
Box to_box(const detect::Detection &d);
detect::Detection to_detection(const Box &b);

}  // namespace rksec::track
