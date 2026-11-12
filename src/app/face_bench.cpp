#include "app/face_bench.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "app/capture_run.hpp"
#include "core/log.hpp"
#include "core/stage_timer.hpp"
#include "face/retinaface.hpp"
#include "media/dma_buffer.hpp"
#include "media/draw.hpp"
#include "media/jpeg_decoder.hpp"
#include "media/letterbox.hpp"
#ifdef RKSEC_HAVE_RKNN
#include "detect/rknn_model.hpp"
#endif
#if defined(RKSEC_HAVE_MPP) && defined(RKSEC_HAVE_RGA)
#include "media/mpp_jpeg_decoder.hpp"
#include "media/rga_converter.hpp"
#endif

namespace rksec::app {

namespace {

using Clock = std::chrono::steady_clock;
[[maybe_unused]] constexpr int kExitOk = 0;
[[maybe_unused]] constexpr int kExitNoHw = 2;
[[maybe_unused]] constexpr int kExitDevice = 4;
[[maybe_unused]] constexpr std::uint8_t kPad = 114;
[[maybe_unused]] constexpr int kImageRuns = 20;
[[maybe_unused]] constexpr int kFaceCore = 1;  // 人脸模型放在 NPU 核 1（核 0 跑人员检测，第 7c 步两个一起跑）

#ifdef RKSEC_HAVE_RKNN

// 加载人脸检测模型（float32 输出模式）并检查输出形状
std::unique_ptr<detect::RknnModel> load_face_model(const config::AppConfig &cfg, std::vector<face::Prior> &priors,
                                                   std::string &err)
{
    auto m = detect::RknnModel::load(cfg.face.detect_model, kFaceCore, err, detect::OutputMode::float32);
    if (!m) {
        return nullptr;
    }
    priors = face::retinaface_priors(m->input_width(), m->input_height());
    std::vector<std::vector<int>> dims;
    for (const auto &t : m->float_outputs()) {
        dims.push_back(t.dims);
    }
    if (!face::check_retinaface_outputs(dims, priors.size(), err)) {
        err = "模型输出不是 rknn_model_zoo 的 RetinaFace 格式：" + err;
        return nullptr;
    }
    return m;
}

std::vector<face::Face> decode(const detect::RknnModel &m, const std::vector<face::Prior> &priors,
                               const config::AppConfig &cfg)
{
    const auto &o = m.float_outputs();
    face::RetinaFaceOptions opts;
    opts.score_threshold = cfg.face.detect_threshold;
    return face::retinaface_decode(o[0].data.data(), o[1].data.data(), o[2].data.data(), priors, m.input_width(),
                                   m.input_height(), opts);
}

void print_faces(const std::vector<face::Face> &faces)
{
    static constexpr std::array<const char *, 5> kNames = {"左眼", "右眼", "鼻尖", "左嘴角", "右嘴角"};
    for (const auto &f : faces) {
        std::printf("  face %.3f  (%4.0f, %4.0f, %4.0f, %4.0f)  ", static_cast<double>(f.box.score),
                    static_cast<double>(f.box.x1), static_cast<double>(f.box.y1), static_cast<double>(f.box.x2),
                    static_cast<double>(f.box.y2));
        for (std::size_t k = 0; k < 5; ++k) {
            std::printf("%s(%.0f,%.0f) ", kNames[k], static_cast<double>(f.landmarks[k].x),
                        static_cast<double>(f.landmarks[k].y));
        }
        std::printf("\n");
    }
}

#endif

}  // namespace

#ifdef RKSEC_HAVE_RKNN

int face_image(const config::AppConfig &cfg, const std::string &path, const std::string &save_path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LOG_E("face", "打不开图片 %s", path.c_str());
        return kExitDevice;
    }
    const std::vector<std::uint8_t> jpeg{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::string err;
    media::JpegDecoder dec;
    media::Image img;
    if (!dec.decode(jpeg.data(), jpeg.size(), 1 << 14, img, err)) {
        LOG_E("face", "图片解码失败：%s", err.c_str());
        return kExitDevice;
    }
    std::vector<face::Prior> priors;
    auto model = load_face_model(cfg, priors, err);
    if (!model) {
        LOG_E("face", "%s", err.c_str());
        return kExitDevice;
    }
    std::printf("模型       %s\n           %s\n", cfg.face.detect_model.c_str(), model->describe().c_str());
    const int mw = model->input_width();
    const int mh = model->input_height();
    auto input = media::DmaBuffer::allocate(static_cast<std::size_t>(mw) * static_cast<std::size_t>(mh) * 3, err);
    if (!input) {
        LOG_E("dma", "%s", err.c_str());
        return kExitDevice;
    }
    const media::Letterbox lb = media::fit_letterbox(img.width, img.height, mw, mh);
    input->begin_cpu_write();
    media::letterbox_to_rgb(img, lb, input->data(), mw, mh, mw, kPad);
    input->end_cpu_write();
    std::printf("图片       %s %dx%d → letterbox %dx%d 放在 (%d, %d)\n", path.c_str(), img.width, img.height, lb.width,
                lb.height, lb.left, lb.top);

    StageStats t_run{"推理（含输入拷贝、输出换算成 float）"};
    StageStats t_post{"后处理（先验框解码 + NMS）"};
    std::vector<face::Face> faces;
    for (int i = 0; i < kImageRuns + 1; ++i) {
        const auto a = Clock::now();
        if (!model->run(*input, err)) {
            LOG_E("face", "%s", err.c_str());
            return kExitDevice;
        }
        const auto b = Clock::now();
        faces = decode(*model, priors, cfg);
        const auto c = Clock::now();
        if (i > 0) {
            t_run.record(b - a);
            t_post.record(c - b);
        }
    }
    face::to_source(faces, lb, img.width, img.height);
    std::printf("%s\n%s\n", t_run.report().c_str(), t_post.report().c_str());
    std::printf("结果       %zu 张脸（阈值 %.2f），坐标是原图像素：\n", faces.size(),
                static_cast<double>(cfg.face.detect_threshold));
    print_faces(faces);
    if (!save_path.empty()) {
        for (const auto &f : faces) {
            media::draw_rect(img, static_cast<int>(f.box.x1), static_cast<int>(f.box.y1), static_cast<int>(f.box.x2),
                             static_cast<int>(f.box.y2), 0x27AE60U, 2);
            for (const auto &p : f.landmarks) {
                media::draw_dot(img, static_cast<int>(p.x), static_cast<int>(p.y), 2, 0xEB5757U);
            }
        }
        std::printf("已保存     %s%s\n", save_path.c_str(), media::save_ppm(img, save_path) ? "" : " —— 写失败");
    }
    return kExitOk;
}

#else

int face_image(const config::AppConfig & /*cfg*/, const std::string & /*path*/, const std::string & /*save_path*/)
{
    std::printf("这次编译没有 RKNN，--face-image 用不了\n");
    return kExitNoHw;
}

#endif

#if defined(RKSEC_HAVE_MPP) && defined(RKSEC_HAVE_RGA) && defined(RKSEC_HAVE_RKNN)

int face_bench(const config::AppConfig &cfg, int frames)
{
    std::vector<std::vector<std::uint8_t>> jpegs;
    LOG_I("bench", "从摄像头取 %d 帧……（脸对着镜头）", frames);
    if (!grab_mjpeg(cfg, frames, jpegs)) {
        return kExitDevice;
    }
    std::string err;
    media::MppJpegDecoder dec;
    if (!dec.open({}, err)) {
        LOG_E("mpp", "%s", err.c_str());
        return kExitDevice;
    }
    std::vector<face::Prior> priors;
    auto model = load_face_model(cfg, priors, err);
    if (!model) {
        LOG_E("face", "%s", err.c_str());
        return kExitDevice;
    }
    std::printf("模型       %s\n", model->describe().c_str());
    const int mw = model->input_width();
    const int mh = model->input_height();
    auto input = media::DmaBuffer::allocate(static_cast<std::size_t>(mw) * static_cast<std::size_t>(mh) * 3, err);
    if (!input) {
        LOG_E("dma", "%s", err.c_str());
        return kExitDevice;
    }
    input->begin_cpu_write();
    std::memset(input->data(), kPad, input->size());
    input->end_cpu_write();
    const media::RgbTarget target{input->fd(), mw, mh, mw, media::RgbFormat::rgb888};

    StageStats t_dec{"MPP 解码"};
    StageStats t_rga{"RGA letterbox（整幅画面 → 模型输入）"};
    StageStats t_run{"人脸检测推理（NPU 核 1，含拷贝和换算）"};
    StageStats t_post{"后处理"};
    std::uint64_t failed = 0;
    std::uint64_t with_face = 0;
    std::uint64_t total = 0;
    std::vector<face::Face> example;
    float smallest = 1e9F;
    for (std::size_t i = 0; i < jpegs.size(); ++i) {
        const auto a = Clock::now();
        auto f = dec.decode(jpegs[i].data(), jpegs[i].size(), err);
        const auto b = Clock::now();
        if (!f) {
            ++failed;
            continue;
        }
        const int sw = f->layout.width;
        const int sh = f->layout.height;
        const media::Letterbox lb = media::fit_letterbox(sw, sh, mw, mh);
        const bool ok = media::rga_convert(*f, target, {lb.left, lb.top, lb.width, lb.height}, err);
        f.reset();
        const auto c = Clock::now();
        if (!ok || !model->run(*input, err)) {
            if (failed++ < 3) {
                LOG_W("bench", "第 %zu 帧：%s", i, err.c_str());
            }
            continue;
        }
        const auto d = Clock::now();
        auto faces = decode(*model, priors, cfg);
        const auto e = Clock::now();
        face::to_source(faces, lb, sw, sh);
        t_dec.record(b - a);
        t_rga.record(c - b);
        t_run.record(d - c);
        t_post.record(e - d);
        total += faces.size();
        with_face += faces.empty() ? 0U : 1U;
        for (const auto &x : faces) {
            smallest = std::min(smallest, x.box.x2 - x.box.x1);
        }
        if (example.empty() && !faces.empty()) {
            example = faces;
        }
    }
    std::printf("%s\n%s\n%s\n%s\n", t_dec.report().c_str(), t_rga.report().c_str(), t_run.report().c_str(),
                t_post.report().c_str());
    std::printf("人脸       %llu 帧里有脸（共 %zu 帧），平均每帧 %.2f 张；最小的脸宽 %.0f 像素（原画面）；失败 %llu\n",
                static_cast<unsigned long long>(with_face), jpegs.size(),
                static_cast<double>(total) / static_cast<double>(std::max<std::size_t>(jpegs.size(), 1)),
                with_face > 0 ? static_cast<double>(smallest) : 0.0, static_cast<unsigned long long>(failed));
    if (!example.empty()) {
        std::printf("例子       第一个有脸的帧（原画面坐标）：\n");
        print_faces(example);
    }
    return kExitOk;
}

#else

int face_bench(const config::AppConfig & /*cfg*/, int /*frames*/)
{
    std::printf("这次编译没有 MPP、RGA 或 RKNN，--face-bench 用不了\n");
    return kExitNoHw;
}

#endif

}  // namespace rksec::app
