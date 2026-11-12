#include "face/face_engine.hpp"

#include <cstring>

#include "detect/rknn_model.hpp"
#include "face/gallery.hpp"

namespace rksec::face {

namespace {

constexpr int kFaceCore = 1;  // 见头文件

}  // namespace

struct FaceEngine::Impl {
    std::unique_ptr<detect::RknnModel> det;
    std::unique_ptr<detect::RknnModel> rec;
    std::vector<Prior> priors;
    RetinaFaceOptions opts;
};

FaceEngine::FaceEngine() : impl_(std::make_unique<Impl>()) {}
FaceEngine::~FaceEngine() = default;

std::unique_ptr<FaceEngine> FaceEngine::open(const config::Face &cfg, std::string &err)
{
    std::unique_ptr<FaceEngine> e{new FaceEngine()};
    Impl &p = *e->impl_;
    p.det = detect::RknnModel::load(cfg.detect_model, kFaceCore, err, detect::OutputMode::float32);
    if (!p.det) {
        err = "人脸检测模型：" + err;
        return nullptr;
    }
    if (p.det->input_width() != p.det->input_height()) {
        err = "人脸检测模型的输入应该是正方形";
        return nullptr;
    }
    p.priors = retinaface_priors(p.det->input_width(), p.det->input_height());
    std::vector<std::vector<int>> dims;
    for (const auto &t : p.det->float_outputs()) {
        dims.push_back(t.dims);
    }
    if (!check_retinaface_outputs(dims, p.priors.size(), err)) {
        err = "人脸检测模型的输出不是 rknn_model_zoo 的 RetinaFace 格式：" + err;
        return nullptr;
    }
    p.opts.score_threshold = cfg.detect_threshold;

    p.rec = detect::RknnModel::load(cfg.recognize_model, kFaceCore, err, detect::OutputMode::float32);
    if (!p.rec) {
        err = "人脸识别模型：" + err;
        return nullptr;
    }
    if (p.rec->input_width() != kAlignedSize || p.rec->input_height() != kAlignedSize) {
        err = "人脸识别模型的输入应该是 112×112，实际 " + std::to_string(p.rec->input_width()) + "×" +
              std::to_string(p.rec->input_height());
        return nullptr;
    }
    if (p.rec->float_outputs().size() != 1 || p.rec->float_outputs()[0].data.size() != kEmbeddingSize) {
        err = "人脸识别模型应该只有 1 个输出、512 个数";
        return nullptr;
    }

    e->detect_w_ = p.det->input_width();
    const auto side = static_cast<std::size_t>(e->detect_w_);
    e->detect_in_ = media::DmaBuffer::allocate(side * side * 3, err);
    constexpr auto kAligned = static_cast<std::size_t>(kAlignedSize);
    e->embed_in_ = media::DmaBuffer::allocate(kAligned * kAligned * 3, err);
    if (!e->detect_in_ || !e->embed_in_) {
        err = "人脸模型的输入缓冲区：" + err;
        return nullptr;
    }
    return e;
}

ImageView FaceEngine::detect_view() const
{
    return {detect_in_->data(), detect_w_, detect_w_, static_cast<std::size_t>(detect_w_) * 3, PixelFormat::rgb888};
}

bool FaceEngine::detect(std::vector<Face> &faces, std::string &err)
{
    Impl &p = *impl_;
    if (!p.det->run(*detect_in_, err)) {
        return false;
    }
    const auto &o = p.det->float_outputs();
    faces =
        retinaface_decode(o[0].data.data(), o[1].data.data(), o[2].data.data(), p.priors, detect_w_, detect_w_, p.opts);
    return true;
}

bool FaceEngine::embed(const ImageView &src, const Face &f, std::vector<float> &embedding, std::string &err,
                       std::vector<std::uint8_t> *aligned, float *raw_norm)
{
    Impl &p = *impl_;
    embed_in_->begin_cpu_write();
    const bool ok = align_face(src, f.landmarks, embed_in_->data());
    if (ok && aligned != nullptr) {
        aligned->assign(embed_in_->data(), embed_in_->data() + embed_in_->size());
    }
    embed_in_->end_cpu_write();
    if (!ok) {
        err = "关键点求不出对齐变换（5 个点重合）";
        return false;
    }
    if (!p.rec->run(*embed_in_, err)) {
        return false;
    }
    embedding = p.rec->float_outputs()[0].data;
    const float n = l2_normalize(embedding);
    if (raw_norm != nullptr) {
        *raw_norm = n;
    }
    if (n <= 0) {
        err = "识别模型输出全是 0";
        return false;
    }
    return true;
}

std::string FaceEngine::describe() const
{
    return "人脸检测 " + impl_->det->describe() + "\n人脸识别 " + impl_->rec->describe();
}

}  // namespace rksec::face
