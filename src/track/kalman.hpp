#pragma once

#include <array>

// 跟踪用的卡尔曼滤波（第 7a 步）：估计一个框的位置和速度，框没被检测到时也能预测它在哪。
//
// 状态 8 个数：中心 x、中心 y、宽高比 a、高 h，以及这 4 个量每帧的变化（速度）。
// 匀速模型：下一帧 = 这一帧 + 速度。观测只有前 4 个（检测框）。
// 噪声和框的高度成正比：近处的人（框大）抖动的像素也多。参数和 DeepSORT / ByteTrack 一样。
//
// 为什么要它：两个人交叉走过时，只看“这一帧的框和上一帧哪个框重叠多”会把编号换掉；
// 有了速度，预测的位置会沿着各自的方向走，交叉后还能对上。被挡住几帧时也靠预测接上。
namespace rksec::track {

struct Box {
    double cx = 0;
    double cy = 0;
    double a = 1;  // 宽 / 高
    double h = 0;
};

class KalmanFilter {
  public:
    // 用第一次检测到的框初始化（速度为 0，速度的不确定度给得很大）
    explicit KalmanFilter(const Box &first);

    void predict();                // 往前走一帧
    void update(const Box &meas);  // 用这一帧的检测框修正

    Box box() const;  // 当前估计的框
    double vx() const { return x_[4]; }
    double vy() const { return x_[5]; }

  private:
    std::array<double, 8> x_{};   // 状态
    std::array<double, 64> p_{};  // 协方差（8×8，按行存）
};

}  // namespace rksec::track
