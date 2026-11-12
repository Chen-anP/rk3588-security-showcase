#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "face/align.hpp"
#include "face/retinaface.hpp"
#include "media/dma_buffer.hpp"

// 人脸检测 + 识别的模型部分（第 7c 步）：RetinaFace 找脸，MobileFaceNet 算特征。只在板子上编译（RKNN）。
//
// 两个模型都放在 NPU 核 1（核 0 跑人员检测；workers 配成 2、3 时和人员检测的第 2 个上下文共用核 1，
// NPU 驱动按顺序轮流执行，只是慢一点）。两个模型都用 float32 输出模式（输出小，和官方 demo 一样）。
//
// 用法（一次只在一个线程里用）：
//   1. 往 detect_input()（320×320 RGB888）里放图：每帧的路径上 RGA 直接写人的上半身那块，
//      单张图片用 CPU letterbox
//   2. detect()：找脸，坐标是 320×320 输入里的像素
//   3. embed()：在某张图上按关键点对齐（关键点要是这张图的坐标）、算 512 维特征（已归一化）
//      每帧的路径上就在 detect_input 上对齐；单张图片在原图上对齐（分辨率高）
namespace rksec::face {

class FaceEngine {
  public:
    // 加载两个模型并检查形状。失败返回 nullptr，err 是原因
    static std::unique_ptr<FaceEngine> open(const config::Face &cfg, std::string &err);
    ~FaceEngine();
    FaceEngine(const FaceEngine &) = delete;
    FaceEngine &operator=(const FaceEngine &) = delete;

    media::DmaBuffer &detect_input() { return *detect_in_; }
    int detect_size() const { return detect_w_; }  // 正方形（320）
    // detect_input 作为对齐用的源图（RGB888）
    ImageView detect_view() const;

    // detect_input 里的人脸，按分数从高到低
    bool detect(std::vector<Face> &faces, std::string &err);

    // 对齐 + 特征。aligned 非空时拷一份对齐后的 112×112 RGB（存图、和电脑上的 onnxruntime 对照用）。
    // raw_norm：归一化之前的长度（模型输出是不是正常的数）
    bool embed(const ImageView &src, const Face &f, std::vector<float> &embedding, std::string &err,
               std::vector<std::uint8_t> *aligned = nullptr, float *raw_norm = nullptr);

    // 两行：两个模型的输入输出、内存（启动时写日志）
    std::string describe() const;

    struct Impl;

  private:
    FaceEngine();
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<media::DmaBuffer> detect_in_;
    std::unique_ptr<media::DmaBuffer> embed_in_;
    int detect_w_ = 0;
};

}  // namespace rksec::face
