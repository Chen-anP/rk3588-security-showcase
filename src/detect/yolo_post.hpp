#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "media/letterbox.hpp"

// YOLO11 的后处理（第 5d 步）：NPU 输出的 int8 张量 → 一个个框。纯 CPU 计算，CI 上能测。
//
// 模型是 rknn_model_zoo v2.3.2 转出来的 yolo11n（640×640，INT8），输出 9 个张量，3 个尺度各 3 个：
//   框     [1, 64, H, W]  每个格子 4 条边 × 16 个“距离档位”的分数（DFL，见下）
//   类别   [1, 80, H, W]  每个格子 80 个类别的分数（模型里已经做过 sigmoid，0～1）
//   分数和 [1, 1,  H, W]  80 个类别分数之和，用来快速跳过空格子（没有这一项的模型也支持，6 个输出）
//   H×W 依次是 80×80、40×40、20×20，对应步长（stride）8、16、32：大格子看大目标，小格子看小目标
//
// DFL（Distribution Focal Loss）：一条边到格子中心的距离不是直接输出一个数，而是 16 个档位（0～15 个步长）
// 各一个分数；先 softmax 变成概率，再求期望 Σ i·p(i)，得到带小数的距离。这样框的位置更准。
//
// int8 量化：真实值 = (q - zp) × scale。比较阈值时把阈值也换成 int8，省掉每个数的换算（和官方 demo 一样）。
//
// 流程：每个格子取分数最高的类别（argmax）→ 低于阈值的跳过 → 解 DFL 得到框 → 按类别做 NMS（去掉重叠的重复框）
// → 只保留要的类别 → 换算回原画面坐标。
namespace rksec::detect {

// 一个框。坐标单位是像素，左上角 (x1, y1)、右下角 (x2, y2)
struct Detection {
    float x1 = 0;
    float y1 = 0;
    float x2 = 0;
    float y2 = 0;
    float score = 0;  // 0～1
    int cls = 0;      // COCO 类别编号，0 = person
};

// NPU 的一个输出张量（NCHW，int8，不拥有内存）
struct TensorView {
    const std::int8_t *data = nullptr;
    int c = 0;
    int h = 0;
    int w = 0;
    std::int32_t zp = 0;
    float scale = 1;
};

struct PostOptions {
    float score_threshold = 0.25F;  // 低于这个分数的不要（官方 demo 的 BOX_THRESH）
    float nms_threshold = 0.45F;  // 同类两个框重叠（IoU）超过这个值，去掉分数低的（官方 demo 的 NMS_THRESH）
    std::vector<int> classes;  // 只保留这些类别；空 = 全部
    int max_detections = 100;  // 最多输出多少个框（按分数从高到低）
};

// 9 个（或 6 个）输出 → 模型输入坐标系（model_w × model_h）里的框，按分数从高到低。
// 张量的个数、尺寸不符合上面的约定时返回空，err 是原因
std::vector<Detection> yolo11_decode(const std::vector<TensorView> &outs, int model_w, int model_h,
                                     const PostOptions &opts, std::string &err);

// 检查输出张量的形状是否符合约定（加载模型时检查一次，不用每帧检查）。classes 返回类别数
bool check_yolo11_outputs(const std::vector<TensorView> &outs, int model_w, int model_h, int &classes,
                          std::string &err);

// 两个框的交并比（IoU）：重叠面积 / 合起来的面积，0～1
float iou(const Detection &a, const Detection &b);

// 非极大值抑制：按类别，从分数最高的开始，去掉和它 IoU 超过 threshold 的同类框。返回按分数从高到低
std::vector<Detection> nms(std::vector<Detection> dets, float threshold);

// 模型输入（letterbox 后的方框）坐标 → 原画面坐标，并裁到原画面 [0, src_w) × [0, src_h) 内
void to_source(std::vector<Detection> &dets, const media::Letterbox &lb, int src_w, int src_h);

// DFL：16 个档位的分数 → 期望距离（单位：步长）。单独拿出来测试
float dfl_expect(const float *logits, int bins);

}  // namespace rksec::detect
