#include "detect/rknn_model.hpp"

#include <rknn_api.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>

namespace rksec::detect {

struct RknnModel::Impl {
    rknn_context ctx = 0;
    bool has_ctx = false;
    rknn_tensor_attr in_attr{};
    std::vector<rknn_tensor_attr> out_attrs;
    std::vector<rknn_tensor_mem *> out_mems;
    std::map<int, rknn_tensor_mem *> in_mems;  // 输入 DMA-BUF 的 fd → 登记过的 rknn 内存
    std::string api_version;
    std::string drv_version;
    std::uint64_t weight_bytes = 0;    // 权重占的内存
    std::uint64_t internal_bytes = 0;  // 中间结果占的内存

    ~Impl()
    {
        if (!has_ctx) {
            return;
        }
        for (auto &[fd, m] : in_mems) {
            rknn_destroy_mem(ctx, m);
        }
        for (rknn_tensor_mem *m : out_mems) {
            rknn_destroy_mem(ctx, m);
        }
        rknn_destroy(ctx);
    }
};

namespace {

std::string code(int ret)
{
    return "错误码 " + std::to_string(ret);
}

rknn_core_mask core_mask(int core)
{
    switch (core) {
    case 0:
        return RKNN_NPU_CORE_0;
    case 1:
        return RKNN_NPU_CORE_1;
    case 2:
        return RKNN_NPU_CORE_2;
    case RknnModel::kAllCores:
        return RKNN_NPU_CORE_0_1_2;
    default:
        return RKNN_NPU_CORE_AUTO;
    }
}

std::string dims_text(const rknn_tensor_attr &a)
{
    std::string s;
    for (std::uint32_t i = 0; i < a.n_dims; ++i) {
        s += (i == 0 ? "" : "x") + std::to_string(a.dims[i]);
    }
    return s;
}

}  // namespace

std::unique_ptr<RknnModel> RknnModel::load(const std::string &path, int core, std::string &err, OutputMode mode)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "打不开模型文件 " + path + "（路径对吗？见配置 detect.model）";
        return nullptr;
    }
    std::vector<char> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (data.empty()) {
        err = "模型文件是空的：" + path;
        return nullptr;
    }
    std::unique_ptr<RknnModel> m{new RknnModel()};
    m->impl_ = std::make_unique<Impl>();
    m->core_ = core;
    m->mode_ = mode;
    // rknn_init 会把需要的内容复制走，返回后 data 可以释放
    const int ret = rknn_init(&m->impl_->ctx, data.data(), static_cast<std::uint32_t>(data.size()), 0, nullptr);
    if (ret != RKNN_SUCC) {
        err = "rknn_init 失败（" + code(ret) +
              "）：模型文件坏了，或者转换模型用的 rknn-toolkit2 和板子上的 librknnrt 版本不匹配";
        return nullptr;
    }
    m->impl_->has_ctx = true;
    if (!m->setup(err)) {
        return nullptr;
    }
    return m;
}

std::unique_ptr<RknnModel> RknnModel::duplicate(int core, std::string &err) const
{
    std::unique_ptr<RknnModel> m{new RknnModel()};
    m->impl_ = std::make_unique<Impl>();
    m->core_ = core;
    m->mode_ = mode_;
    const int ret = rknn_dup_context(&impl_->ctx, &m->impl_->ctx);
    if (ret != RKNN_SUCC) {
        err = "rknn_dup_context 失败（" + code(ret) + "）";
        return nullptr;
    }
    m->impl_->has_ctx = true;
    if (!m->setup(err)) {
        return nullptr;
    }
    return m;
}

RknnModel::~RknnModel() = default;

bool RknnModel::setup(std::string &err)
{
    Impl &p = *impl_;
    if (core_ >= 0) {
        if (const int ret = rknn_set_core_mask(p.ctx, core_mask(core_)); ret != RKNN_SUCC) {
            err = "绑定 NPU 核 " + std::to_string(core_) + " 失败（" + code(ret) + "）";
            return false;
        }
    }
    rknn_sdk_version ver{};
    if (rknn_query(p.ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == RKNN_SUCC) {
        p.api_version = ver.api_version;
        p.drv_version = ver.drv_version;
    }
    if (mode_ == OutputMode::float32) {
        // 只在 float32 模式查内存占用：第 7b 步上板发现，int8 模式（零拷贝输出）在绑定输出内存之前查
        // RKNN_QUERY_MEM_SIZE，之后 YOLO 的输出就不对了（灰图检测出人、复制的上下文结果不一致）。
        // 后面留了余量：防止板子上的头文件和运行库的结构体大小不一致时写越界
        struct {
            rknn_mem_size m;
            std::array<char, 256> spare;
        } q{};
        if (rknn_query(p.ctx, RKNN_QUERY_MEM_SIZE, &q.m, sizeof(q.m)) == RKNN_SUCC) {
            p.weight_bytes = q.m.total_weight_size;
            p.internal_bytes = q.m.total_internal_size;
        }
    }
    rknn_input_output_num io{};
    if (const int ret = rknn_query(p.ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)); ret != RKNN_SUCC) {
        err = "查询输入输出个数失败（" + code(ret) + "）";
        return false;
    }
    if (io.n_input != 1) {
        err = "模型应该只有 1 个输入，实际 " + std::to_string(io.n_input) + " 个";
        return false;
    }

    // 输入：[1, H, W, 3]（NHWC）或 [1, 3, H, W]（NCHW），uint8 图像
    p.in_attr.index = 0;
    if (const int ret = rknn_query(p.ctx, RKNN_QUERY_INPUT_ATTR, &p.in_attr, sizeof(p.in_attr)); ret != RKNN_SUCC) {
        err = "查询输入属性失败（" + code(ret) + "）";
        return false;
    }
    const rknn_tensor_attr &a = p.in_attr;
    if (a.n_dims != 4) {
        err = "输入应该是 4 维，实际 " + dims_text(a);
        return false;
    }
    int ch = 0;
    if (a.fmt == RKNN_TENSOR_NCHW) {
        ch = static_cast<int>(a.dims[1]);
        in_h_ = static_cast<int>(a.dims[2]);
        in_w_ = static_cast<int>(a.dims[3]);
    } else {
        in_h_ = static_cast<int>(a.dims[1]);
        in_w_ = static_cast<int>(a.dims[2]);
        ch = static_cast<int>(a.dims[3]);
    }
    if (ch != 3 || in_w_ <= 0 || in_h_ <= 0) {
        err = "输入应该是 3 通道图像，实际 " + dims_text(a);
        return false;
    }
    if (mode_ == OutputMode::int8 && a.w_stride != 0 && static_cast<int>(a.w_stride) != in_w_) {
        // 零拷贝时输入每行要按 NPU 要求的像素数排；640 宽正好满足，其他尺寸先不支持
        err = "模型输入要求每行 " + std::to_string(a.w_stride) + " 像素，和宽度 " + std::to_string(in_w_) +
              " 不同，暂不支持";
        return false;
    }

    p.out_attrs.assign(io.n_output, rknn_tensor_attr{});
    for (std::uint32_t i = 0; i < io.n_output; ++i) {
        rknn_tensor_attr &o = p.out_attrs[i];
        o.index = i;
        if (const int ret = rknn_query(p.ctx, RKNN_QUERY_OUTPUT_ATTR, &o, sizeof(o)); ret != RKNN_SUCC) {
            err = "查询第 " + std::to_string(i) + " 个输出属性失败（" + code(ret) + "）";
            return false;
        }
    }
    if (mode_ == OutputMode::float32) {
        // 输出由运行库换算成 float，每次 run 时拷到这里
        floats_.assign(io.n_output, FloatTensor{});
        for (std::uint32_t i = 0; i < io.n_output; ++i) {
            const rknn_tensor_attr &o = p.out_attrs[i];
            for (std::uint32_t d = 0; d < o.n_dims; ++d) {
                floats_[i].dims.push_back(static_cast<int>(o.dims[d]));
            }
            floats_[i].data.assign(o.n_elems, 0.0F);
        }
        return true;
    }

    // int8 模式：每个输出 NCHW、int8、非对称量化，绑定到运行库分配的内存（零拷贝）
    views_.assign(io.n_output, TensorView{});
    for (std::uint32_t i = 0; i < io.n_output; ++i) {
        rknn_tensor_attr &o = p.out_attrs[i];
        if (o.type != RKNN_TENSOR_INT8 || o.qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC || o.n_dims != 4 ||
            o.fmt != RKNN_TENSOR_NCHW) {
            err = "第 " + std::to_string(i) + " 个输出应该是 NCHW 的 int8 非对称量化张量（INT8 模型），实际 " +
                  dims_text(o) + " " + get_type_string(o.type) + " " + get_qnt_type_string(o.qnt_type) + " " +
                  get_format_string(o.fmt) + "（这种模型要用 OutputMode::float32 加载）";
            return false;
        }
        rknn_tensor_mem *mem = rknn_create_mem(p.ctx, o.size);
        if (mem == nullptr) {
            err = "给第 " + std::to_string(i) + " 个输出分配内存失败";
            return false;
        }
        p.out_mems.push_back(mem);
        if (const int ret = rknn_set_io_mem(p.ctx, mem, &o); ret != RKNN_SUCC) {
            err = "绑定第 " + std::to_string(i) + " 个输出内存失败（" + code(ret) + "）";
            return false;
        }
        TensorView &v = views_[i];
        v.data = static_cast<const std::int8_t *>(mem->virt_addr);
        v.c = static_cast<int>(o.dims[1]);
        v.h = static_cast<int>(o.dims[2]);
        v.w = static_cast<int>(o.dims[3]);
        v.zp = o.zp;
        v.scale = o.scale;
    }
    return true;
}

bool check_yolo11(const RknnModel &m, std::string &err)
{
    if (m.mode() != OutputMode::int8) {
        err = "YOLO11 模型要用 int8 模式加载";
        return false;
    }
    int classes = 0;
    if (!check_yolo11_outputs(m.outputs(), m.input_width(), m.input_height(), classes, err)) {
        err = "模型输出不是 rknn_model_zoo 的 YOLO11 格式：" + err;
        return false;
    }
    return true;
}

bool RknnModel::run(const media::DmaBuffer &input, std::string &err)
{
    Impl &p = *impl_;
    const std::size_t need = static_cast<std::size_t>(in_w_) * static_cast<std::size_t>(in_h_) * 3;
    if (input.size() < need) {
        err = "输入缓冲区 " + std::to_string(input.size()) + " 字节，模型要 " + std::to_string(need) + " 字节";
        return false;
    }
    if (mode_ == OutputMode::float32) {
        return run_copy(input, need, err);
    }
    rknn_tensor_mem *mem = nullptr;
    if (const auto it = p.in_mems.find(input.fd()); it != p.in_mems.end()) {
        mem = it->second;
    } else {
        mem = rknn_create_mem_from_fd(p.ctx, input.fd(), input.data(), static_cast<std::uint32_t>(input.size()), 0);
        if (mem == nullptr) {
            err = "输入 DMA-BUF 交给 NPU 失败（rknn_create_mem_from_fd）";
            return false;
        }
        p.in_mems.emplace(input.fd(), mem);
    }
    // 告诉运行库输入是 uint8 的 NHWC 图像，归一化（÷255）由模型自己做（pass_through = 0）
    rknn_tensor_attr a = p.in_attr;
    a.type = RKNN_TENSOR_UINT8;
    a.fmt = RKNN_TENSOR_NHWC;
    a.pass_through = 0;
    if (const int ret = rknn_set_io_mem(p.ctx, mem, &a); ret != RKNN_SUCC) {
        err = "绑定输入内存失败（" + code(ret) + "）";
        return false;
    }
    if (const int ret = rknn_run(p.ctx, nullptr); ret != RKNN_SUCC) {
        err = "rknn_run 失败（" + code(ret) + "）";
        return false;
    }
    return true;
}

bool RknnModel::run_copy(const media::DmaBuffer &input, std::size_t bytes, std::string &err)
{
    Impl &p = *impl_;
    // 和官方 demo 一样：输入由运行库拷一份（归一化、量化也是它做），输出取成 float
    rknn_input in{};
    in.index = 0;
    in.type = RKNN_TENSOR_UINT8;
    in.fmt = RKNN_TENSOR_NHWC;
    in.size = static_cast<std::uint32_t>(bytes);
    in.buf = input.data();
    in.pass_through = 0;
    input.begin_cpu_read();  // RGA 刚写过：让 CPU 缓存失效，运行库才读得到新数据
    const int set = rknn_inputs_set(p.ctx, 1, &in);
    input.end_cpu_read();
    if (set != RKNN_SUCC) {
        err = "rknn_inputs_set 失败（" + code(set) + "）";
        return false;
    }
    if (const int ret = rknn_run(p.ctx, nullptr); ret != RKNN_SUCC) {
        err = "rknn_run 失败（" + code(ret) + "）";
        return false;
    }
    std::vector<rknn_output> outs(floats_.size());
    for (std::size_t i = 0; i < outs.size(); ++i) {
        outs[i].index = static_cast<std::uint32_t>(i);
        outs[i].want_float = 1;
        outs[i].is_prealloc = 1;  // 直接写进 floats_，不用运行库再分配
        outs[i].buf = floats_[i].data.data();
        outs[i].size = static_cast<std::uint32_t>(floats_[i].data.size() * sizeof(float));
    }
    const int got = rknn_outputs_get(p.ctx, static_cast<std::uint32_t>(outs.size()), outs.data(), nullptr);
    rknn_outputs_release(p.ctx, static_cast<std::uint32_t>(outs.size()), outs.data());
    if (got != RKNN_SUCC) {
        err = "rknn_outputs_get 失败（" + code(got) + "）";
        return false;
    }
    return true;
}

std::string RknnModel::describe() const
{
    const Impl &p = *impl_;
    std::string s = "librknnrt " + p.api_version + "，驱动 " + p.drv_version + "；NPU 核 " +
                    (core_ < 0            ? std::string{"自动"}
                     : core_ == kAllCores ? std::string{"0+1+2 联合"}
                                          : std::to_string(core_)) +
                    "；输入 " + dims_text(p.in_attr) + " " + get_format_string(p.in_attr.fmt) + " " +
                    get_type_string(p.in_attr.type) + "；输出";
    for (const rknn_tensor_attr &o : p.out_attrs) {
        std::array<char, 80> buf{};
        std::snprintf(buf.data(), buf.size(), " %s %s(zp %d, s %.4g)", dims_text(o).c_str(), get_type_string(o.type),
                      o.zp, static_cast<double>(o.scale));
        s += buf.data();
    }
    if (p.weight_bytes > 0) {
        std::array<char, 96> mem{};
        std::snprintf(mem.data(), mem.size(), "；内存：权重 %.1f MB，中间结果 %.1f MB",
                      static_cast<double>(p.weight_bytes) / 1048576.0,
                      static_cast<double>(p.internal_bytes) / 1048576.0);
        s += mem.data();
    }
    return s;
}

}  // namespace rksec::detect
