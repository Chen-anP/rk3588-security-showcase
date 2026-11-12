#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "detect/yolo_post.hpp"
#include "media/letterbox.hpp"

// RetinaFace 人脸检测的后处理（第 7b 步）：模型输出 → 人脸框 + 5 个关键点。纯 CPU 计算，CI 上能测。
//
// 模型是 rknn_model_zoo v2.3.2 的 RetinaFace_mobile320（MobileNet0.25 主干，输入 320×320）。输出 3 个张量
// （按 float 取出，见 RknnModel 的 float32 模式）：
//   框偏移   [1, N, 4]   相对先验框的偏移（中心 x、y，宽、高）
//   分数     [1, N, 2]   背景、人脸两类的概率（模型里已经做过 softmax），取第 2 列
//   关键点   [1, N, 10]  5 个点（左眼、右眼、鼻尖、左嘴角、右嘴角）相对先验框中心的偏移
//   N = 先验框个数。320 输入：步长 8、16、32 三层格子（40×40、20×20、10×10），每格 2 个先验框 → 4200
//
// 先验框（anchor / prior box）：事先在图上铺好的一组参考框，模型只预测“和参考框差多少”，比直接预测坐标好学。
// 每层格子两种大小：步长 8 那层 16、32 像素，16 那层 64、128，32 那层 256、512（小格子找小脸，大格子找大脸）。
//
// 解码（和官方 demo、原始 Pytorch_Retinaface 一样，方差 0.1 / 0.2）：
//   中心 = 先验中心 + 偏移 × 0.1 × 先验宽高；宽高 = 先验宽高 × exp(偏移 × 0.2)
//   关键点 = 先验中心 + 偏移 × 0.1 × 先验宽高
namespace rksec::face {

struct Point {
    float x = 0;
    float y = 0;
};

struct Face {
    detect::Detection box;             // 人脸框（score 是人脸的概率，cls 固定 0）
    std::array<Point, 5> landmarks{};  // 左眼、右眼、鼻尖、左嘴角、右嘴角
};

struct RetinaFaceOptions {
    float score_threshold = 0.5F;  // 官方 demo 的 CONF_THRESHOLD
    float nms_threshold = 0.4F;    // 官方 demo 的 NMS_THRESHOLD
    int max_faces = 32;
};

// 先验框：每个是 {中心 x, 中心 y, 宽, 高}，都是相对输入宽高的比例（0～1），顺序和模型输出一致
using Prior = std::array<float, 4>;
std::vector<Prior> retinaface_priors(int model_w, int model_h);

// 输出张量的形状对不对：3 个，[1,N,4]、[1,N,2]、[1,N,10]，N 等于先验框个数
bool check_retinaface_outputs(const std::vector<std::vector<int>> &dims, std::size_t priors, std::string &err);

// 解码 + NMS。loc / conf / landm 是三个输出的数据（各 N×4、N×2、N×10 个 float）。
// 返回模型输入坐标系（像素）里的人脸，按分数从高到低
std::vector<Face> retinaface_decode(const float *loc, const float *conf, const float *landm,
                                    const std::vector<Prior> &priors, int model_w, int model_h,
                                    const RetinaFaceOptions &opts);

// 模型输入坐标 → 原画面坐标（框和关键点都换算，并裁到画面里）
void to_source(std::vector<Face> &faces, const media::Letterbox &lb, int src_w, int src_h);

// 几张脸里框面积最大的那张的下标（离镜头最近，第 7c 步识别只认它）。空列表返回 -1
int largest_face(const std::vector<Face> &faces);

}  // namespace rksec::face
