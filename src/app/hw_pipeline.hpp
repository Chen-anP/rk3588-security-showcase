#pragma once

#include <memory>
#include <string>

#include "app/detect_pipeline.hpp"
#include "core/config.hpp"

// 流水线的硬件实现（第 6 步）：MPP 解码 → RGA 出预览图和 NPU 输入 → RKNN 推理 + YOLO11 后处理。
// 第 7c 步加上人脸识别：RGA 裁人的上半身 → RetinaFace 找脸 → 对齐 → MobileFaceNet 算特征。
// 把这些硬件对象组装成 DetectHooks 交给 DetectPipeline。
namespace rksec::app {

class HwPipeline {
  public:
    // 这次编译有没有硬件流水线需要的库（MPP 和 RGA；检测还需要 RKNN）
    static bool available();
    static bool detect_available();

    // 打开解码器、分配缓冲区、加载模型。
    // workers：推理上下文个数（0 = 不检测）。预览部分失败返回 nullptr（err 是原因）；
    // 只有检测部分失败时照样返回，检测关闭，detect_error() 说明原因——没有检测也要有预览。
    // 人脸识别（cfg.face.recognize）同理：起不来只关掉识别，face_error() 说明原因
    static std::unique_ptr<HwPipeline> open(const config::AppConfig &cfg, int workers, std::string &err);

    ~HwPipeline();
    HwPipeline(const HwPipeline &) = delete;
    HwPipeline &operator=(const HwPipeline &) = delete;

    // 给 DetectPipeline 用。返回的函数引用这个对象，它要比流水线活得久
    DetectHooks hooks();
    int workers() const;
    const std::string &detect_error() const;
    std::string describe() const;  // 模型、运行库版本，写日志用

    bool recognizing() const;
    const std::string &face_error() const;
    // 流水线参数：推理线程数、底库、相似度阈值
    PipelineOptions options(const config::AppConfig &cfg) const;

    struct Impl;

  private:
    HwPipeline();
    std::unique_ptr<Impl> impl_;
};

}  // namespace rksec::app
