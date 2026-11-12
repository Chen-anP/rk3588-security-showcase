#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "detect/yolo_post.hpp"
#include "media/dma_buffer.hpp"

// RKNN 模型（NPU 推理），第 5d 步；第 7b 步加了“输出转成 float”模式。
//
//   auto m = RknnModel::load("yolo11n_640_i8_rk3588_tk232.rknn", 0, err);   // 绑定 NPU 核 0
//   check_yolo11(*m, err);                                                  // 输出格式是不是 YOLO11 的
//   auto m2 = m->duplicate(1, err);                                         // 共享权重的第二个上下文，核 1
//   m->run(input, err);                                                    // input：640×640 RGB888 的 DmaBuffer
//   yolo11_decode(m->outputs(), m->input_width(), m->input_height(), opts, err);
//
// 两种输出方式（加载时选，同一个模型只能用一种）：
// - OutputMode::int8（YOLO，每帧都跑）：输入零拷贝——RGA 写好的 DMA-BUF 直接交给 NPU
//   （rknn_create_mem_from_fd + rknn_set_io_mem）；输出也用 rknn_set_io_mem 绑到运行库分配的内存，
//   NCHW、int8 量化值，后处理直接比较 int8。要求每个输出都是 4 维、int8 非对称量化
// - OutputMode::float32（RetinaFace、人脸识别这类输出小、可能混着 FP16 的模型）：
//   输入 rknn_inputs_set（运行库拷一份）、输出 rknn_outputs_get(want_float=1)（运行库换算成 float）。
//   和 rknn_model_zoo 官方 demo 的用法完全一样：输出只有几十 KB，拷贝的代价可以忽略，换来不用关心每个输出
//   是什么类型、什么格式
//
// RK3588 的 NPU 有 3 个核。一个上下文绑一个核；要并行就 duplicate() 出几个上下文，权重只占一份内存。
// 同一个上下文同一时间只能在一个线程里 run()。
//
// 只在板子上编译（需要 librknnrt 和 rknn_api.h，CMake 自动判断，定义 RKSEC_HAVE_RKNN）。
namespace rksec::detect {

enum class OutputMode : std::uint8_t { int8, float32 };

// float32 模式的一个输出：数据（行优先）和形状
struct FloatTensor {
    std::vector<float> data;
    std::vector<int> dims;
};

class RknnModel {
  public:
    // core：0、1、2 绑定到那个 NPU 核；kAllCores 三个核合起来跑一个模型（按层拆开）；-1 让驱动自己选
    static constexpr int kAllCores = 3;
    static std::unique_ptr<RknnModel> load(const std::string &path, int core, std::string &err,
                                           OutputMode mode = OutputMode::int8);

    // 共享权重的新上下文（rknn_dup_context），绑定到另一个核。原来的模型要比复制出来的活得久
    std::unique_ptr<RknnModel> duplicate(int core, std::string &err) const;

    ~RknnModel();
    RknnModel(const RknnModel &) = delete;
    RknnModel &operator=(const RknnModel &) = delete;

    // 推理一次。input 至少 input_width × input_height × 3 字节（RGB888，每行没有填充）。
    // float32 模式下运行库用 CPU 读 input（这里会先让 CPU 缓存失效，读到 RGA 写进去的新数据）
    bool run(const media::DmaBuffer &input, std::string &err);

    // 最近一次 run 的输出（下一次 run 会覆盖）。int8 模式用 outputs()，float32 模式用 float_outputs()
    const std::vector<TensorView> &outputs() const { return views_; }
    const std::vector<FloatTensor> &float_outputs() const { return floats_; }

    OutputMode mode() const { return mode_; }
    int input_width() const { return in_w_; }
    int input_height() const { return in_h_; }
    int core() const { return core_; }
    // 一行文字：运行库和驱动版本、占用的内存、输入输出的形状和量化参数（启动时写日志）
    std::string describe() const;

    struct Impl;  // 实现细节，头文件里不出现 rknn 的类型

  private:
    RknnModel() = default;
    bool setup(std::string &err);
    bool run_copy(const media::DmaBuffer &input, std::size_t bytes, std::string &err);

    std::unique_ptr<Impl> impl_;
    std::vector<TensorView> views_;
    std::vector<FloatTensor> floats_;
    OutputMode mode_ = OutputMode::int8;
    int in_w_ = 0;
    int in_h_ = 0;
    int core_ = -1;
};

// 模型的输出是不是 rknn_model_zoo 的 YOLO11 格式（int8 模式，9 个张量）。不是的话 err 说明哪里不对
bool check_yolo11(const RknnModel &m, std::string &err);

}  // namespace rksec::detect
