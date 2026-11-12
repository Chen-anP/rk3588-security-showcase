#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "detect/yolo_post.hpp"
#include "face/retinaface.hpp"

// 人脸对齐（第 7c 步）：用 5 个关键点把脸摆正、缩放到识别模型要的 112×112。纯 CPU 计算，CI 上能测。
//
// 识别模型（insightface 的 MobileFaceNet，w600k_mbf）训练时，每张脸都按同一套规则摆好：
// 两只眼睛、鼻尖、两个嘴角落在 112×112 里固定的 5 个位置（kArcFaceRef）。推理时也要按这个规则摆，
// 否则同一个人歪一下头，特征就差很多。
//
// 做法（和 insightface 的 norm_crop 一样）：
// 1. 求一个相似变换（旋转 + 等比缩放 + 平移，不拉伸、不翻转），让检测到的 5 个点
//    尽量贴近参考位置（最小二乘）。相似变换只有 4 个未知数，5 个点有 10 个方程，直接有公式解
// 2. 对 112×112 的每个像素，反算它在原图里的位置，双线性插值取颜色；落在原图外面的补 0（黑）
//
// 另外这里还放“在人的框里找脸”的取景规则（face_search_region）：人脸检测不在整幅画面上做
// （第 7b 步：整幅 1280 缩到 320，远一点的脸只剩几个像素），而是先在人的框里裁一块再缩到 320。
namespace rksec::face {

// 112×112 里 5 个关键点的参考位置（insightface arcface_dst）：左眼、右眼、鼻尖、左嘴角、右嘴角
inline constexpr int kAlignedSize = 112;
inline constexpr std::array<Point, 5> kArcFaceRef = {Point{38.2946F, 51.6963F}, Point{73.5318F, 51.5014F},
                                                     Point{56.0252F, 71.7366F}, Point{41.5493F, 92.3655F},
                                                     Point{70.7299F, 92.2041F}};

// 相似变换：dst = [a −b; b a] · src + [tx; ty]
struct Similarity {
    float a = 1;
    float b = 0;
    float tx = 0;
    float ty = 0;

    Point apply(Point p) const { return {a * p.x - b * p.y + tx, b * p.x + a * p.y + ty}; }
    float scale() const;  // 缩放倍数 √(a² + b²)
};

// 最小二乘求把 src 的 5 个点变到 dst 的相似变换（和 skimage 的 SimilarityTransform.estimate 结果一样）。
// 5 个点重合在一起（求不出来）时返回 false
bool estimate_similarity(const std::array<Point, 5> &src, const std::array<Point, 5> &dst, Similarity &out);

// 源图的像素格式
enum class PixelFormat : std::uint8_t {
    rgb888,    // R G B（RGA 给 NPU 的输入）
    xrgb8888,  // 内存里 B G R X（CPU 解码出来的 Image）
};

struct ImageView {
    const std::uint8_t *data = nullptr;
    int width = 0;
    int height = 0;
    std::size_t stride = 0;  // 每行字节数
    PixelFormat format = PixelFormat::rgb888;
};

// 按 m（源图 → 输出）把源图变换到 dst（RGB888，dst_w × dst_h，每行没有填充）。
// 和 OpenCV 的 warpAffine(INTER_LINEAR, BORDER_CONSTANT 0) 一样：像素坐标取整数点，不加 0.5
void warp_to_rgb(const ImageView &src, const Similarity &m, std::uint8_t *dst, int dst_w, int dst_h);

// 对齐一张脸：landmarks 是源图坐标；结果写进 dst（112×112×3 RGB）。关键点求不出变换时返回 false
bool align_face(const ImageView &src, const std::array<Point, 5> &landmarks, std::uint8_t *dst);

// 画面上一块矩形（像素，x、y、宽、高都是偶数——RGA 读 YUV 时要求 2 对齐）
struct Region {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    detect::Detection person;  // 是在哪个人的框里找（原画面坐标）：几张脸时用它挑（第 9a 步，face_pick.hpp）
};

// 在人的框里找脸时裁哪一块（原画面坐标）：
// - 宽：人框宽的 1.1 倍（框边上留点余量），居中
// - 高：从人框顶上再往上一点（框的 5%，检测框有时切掉头顶）开始，最多和宽一样（正方形），不超过人框
//   ——脸在人的上半部分；只取上面一块，缩到 320 时脸更大。
//   高不超过宽，缩到 320×320 时只在上下补边，左右没有边：RGA 写 RGB888 时起点 x 总是 0，
//   和人员检测的 letterbox（第 5c 步验证过）一样，不用再验证 RGA3 对输出起点 x 的对齐要求
// - 至少 kMinRegion × kMinRegion：RGA3 输入宽至少 68，而且放大倍数不能超过 8（320 / 96 ≈ 3.3）
// - 超出画面的部分平移回来，还放不下就裁到画面大小
// 脸宽大致和人框宽成比例，所以裁出来再缩到 320×320 后，不管人站多远，脸在 320 里都差不多大
// （人越远，放大越多；实际多大由上板测量记录）。人太远时裁出来的块小于 96，再放大也不会有更多细节
inline constexpr int kMinRegion = 96;
Region face_search_region(const detect::Detection &person, int src_w, int src_h);

}  // namespace rksec::face
