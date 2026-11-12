#include "app/hw_pipeline.hpp"

#include <cstring>
#include <vector>

#include "core/log.hpp"
#include "detect/coco_names.hpp"
#include "ui/theme.hpp"
#if defined(RKSEC_HAVE_MPP) && defined(RKSEC_HAVE_RGA)
#include "media/dma_buffer.hpp"
#include "media/letterbox.hpp"
#include "media/mpp_jpeg_decoder.hpp"
#include "media/rga_converter.hpp"
#endif
#ifdef RKSEC_HAVE_RKNN
#include "app/face_admin.hpp"
#include "detect/rknn_model.hpp"
#include "face/face_engine.hpp"
#include "face/face_pick.hpp"
#endif

namespace rksec::app {

namespace {
[[maybe_unused]] constexpr int kDecoderBuffers = 4;
[[maybe_unused]] constexpr int kDecoderBuffersWithEncode = 7;
}  // namespace

bool HwPipeline::available()
{
#if defined(RKSEC_HAVE_MPP) && defined(RKSEC_HAVE_RGA)
    return true;
#else
    return false;
#endif
}

bool HwPipeline::detect_available()
{
#if defined(RKSEC_HAVE_MPP) && defined(RKSEC_HAVE_RGA) && defined(RKSEC_HAVE_RKNN)
    return true;
#else
    return false;
#endif
}

#if defined(RKSEC_HAVE_MPP) && defined(RKSEC_HAVE_RGA)

namespace {

constexpr std::uint8_t kPad = 114;  // letterbox 补边的灰度（和 rknn_model_zoo 一致）
constexpr int kModelSize = 640;

}  // namespace

struct HwPipeline::Impl {
    struct Slot {
        std::unique_ptr<media::DmaBuffer> buf;
        // 下面三项由 media 线程在 npu_input 里写，推理线程在 infer 里读。
        // 中间隔着 jobs 队列的加锁、解锁，读的时候一定看得到写好的值
        media::Letterbox lb;
        int src_w = 0;
        int src_h = 0;
    };

    media::MppJpegDecoder dec;
    std::unique_ptr<media::DmaBuffer> preview;
    std::vector<Slot> slots;
    int model_w = kModelSize;
    int model_h = kModelSize;
    detect::PostOptions post;
    std::string detect_error;
    std::string description;
    // 人脸识别（第 7c 步）。face_region、face_lb 由 media 线程在 face_input 里写，人脸线程在 face_infer 里读
    // （中间隔着队列的加锁、解锁）
    std::shared_ptr<face::Gallery> gallery;
    std::string face_error;
    face::Region face_region;
    int face_src_w = 0;  // 原画面大小（判断脸是否贴边，第 9a 步）
    int face_src_h = 0;
    media::Letterbox face_lb;
#ifdef RKSEC_HAVE_RKNN
    std::vector<std::unique_ptr<detect::RknnModel>> models;  // models[0] 是原始的，后面的是复制出来的
    std::unique_ptr<face::FaceEngine> face;
#endif

    ~Impl()
    {
#ifdef RKSEC_HAVE_RKNN
        // 复制出来的上下文先销毁，原始的最后
        while (!models.empty()) {
            models.pop_back();
        }
#endif
    }

    bool open_detect(const config::AppConfig &cfg, int workers)
    {
#ifdef RKSEC_HAVE_RKNN
        std::string err;
        auto first = detect::RknnModel::load(cfg.detect.model, 0, err);
        if (!first || !detect::check_yolo11(*first, err)) {
            detect_error = err;
            return false;
        }
        model_w = first->input_width();
        model_h = first->input_height();
        description = first->describe();
        models.push_back(std::move(first));
        for (int w = 1; w < workers; ++w) {
            auto m = models[0]->duplicate(w, err);
            if (!m) {
                detect_error = "第 " + std::to_string(w) + " 个推理上下文：" + err;
                return false;
            }
            models.push_back(std::move(m));
        }
        for (int w = 0; w < workers; ++w) {
            Slot s;
            s.buf = media::DmaBuffer::allocate(
                static_cast<std::size_t>(model_w) * static_cast<std::size_t>(model_h) * 3, err);
            if (!s.buf) {
                detect_error = "NPU 输入缓冲区：" + err;
                return false;
            }
            // 灰边只填一次：RGA 每帧只写中间的画面区域
            s.buf->begin_cpu_write();
            std::memset(s.buf->data(), kPad, s.buf->size());
            s.buf->end_cpu_write();
            slots.push_back(std::move(s));
        }
        post.score_threshold = cfg.detect.score_threshold;
        post.nms_threshold = cfg.detect.nms_threshold;
        for (const std::string &name : cfg.detect.classes) {
            post.classes.push_back(detect::coco_id(name));
        }
        return true;
#else
        (void)cfg;
        (void)workers;
        detect_error = "这次编译没有 RKNN（找不到 rknn_api.h 或 librknnrt），不能检测";
        return false;
#endif
    }

    bool open_face(const config::AppConfig &cfg)
    {
#ifdef RKSEC_HAVE_RKNN
        std::string err;
        face = face::FaceEngine::open(cfg.face, err);
        if (!face) {
            face_error = err;
            return false;
        }
        description += "\n" + face->describe();
        gallery = std::make_shared<face::Gallery>();
        if (!load_gallery(cfg, *gallery, err)) {
            // 人脸库打不开也照样识别（底库空，有脸都是陌生人），但要醒目地报出来
            LOG_E("face", "人脸库读不了，按没有登记的人运行：%s", err.c_str());
        }
        return true;
#else
        (void)cfg;
        face_error = "这次编译没有 RKNN";
        return false;
#endif
    }
};

HwPipeline::HwPipeline() : impl_(std::make_unique<Impl>()) {}
HwPipeline::~HwPipeline() = default;

std::unique_ptr<HwPipeline> HwPipeline::open(const config::AppConfig &cfg, int workers, std::string &err)
{
    std::unique_ptr<HwPipeline> p{new HwPipeline()};
    Impl &m = *p->impl_;
    // 解码器的输出缓冲区池：平时 4 块。编码阶段的队列里最多压着 2 帧、正在编的 1 帧，
    // 再加 media 线程手上的 1 帧，要 4 + 3 块才不会因为编码占着缓冲区而解不出新帧
    media::MppJpegDecoder::Options dec_opts;
    dec_opts.out_buffers = cfg.encode.enabled ? kDecoderBuffersWithEncode : kDecoderBuffers;
    if (!m.dec.open(dec_opts, err)) {
        err = "MPP 解码器：" + err;
        return nullptr;
    }
    m.preview = media::DmaBuffer::allocate(
        static_cast<std::size_t>(ui::theme::kPreviewW) * static_cast<std::size_t>(ui::theme::kPreviewH) * 4, err);
    if (!m.preview) {
        err = "预览缓冲区：" + err;
        return nullptr;
    }
    if (workers > 0 && !m.open_detect(cfg, workers)) {
        // 检测起不来，预览照常：清掉做了一半的，按不检测运行
#ifdef RKSEC_HAVE_RKNN
        while (!m.models.empty()) {
            m.models.pop_back();
        }
#endif
        m.slots.clear();
    }
    if (!m.slots.empty() && cfg.face.recognize) {
        m.open_face(cfg);
    } else if (!cfg.face.recognize) {
        m.face_error = "配置里关闭了人脸识别（face.recognize = false）";
    } else {
        m.face_error = "没有人员检测，不做人脸识别";
    }
    return p;
}

bool HwPipeline::recognizing() const
{
#ifdef RKSEC_HAVE_RKNN
    return impl_->face != nullptr;
#else
    return false;
#endif
}

const std::string &HwPipeline::face_error() const
{
    return impl_->face_error;
}

PipelineOptions HwPipeline::options(const config::AppConfig &cfg) const
{
    PipelineOptions o = with_workers(workers());
    o.gallery = impl_->gallery;
    o.match_threshold = cfg.face.match_threshold;
    return o;
}

int HwPipeline::workers() const
{
    return static_cast<int>(impl_->slots.size());
}

const std::string &HwPipeline::detect_error() const
{
    return impl_->detect_error;
}

std::string HwPipeline::describe() const
{
    return impl_->description;
}

DetectHooks HwPipeline::hooks()
{
    Impl *m = impl_.get();
    DetectHooks h;
    h.decode = [m](const Frame &f, std::string &err) -> std::shared_ptr<const media::YuvView> {
        return m->dec.decode(f.data, f.bytes, err);
    };
    h.preview = [m](const media::YuvView &yuv, media::Image &out, std::string &err) {
        constexpr int w = ui::theme::kPreviewW;
        constexpr int hgt = ui::theme::kPreviewH;
        if (!media::rga_convert(yuv, {m->preview->fd(), w, hgt, w, media::RgbFormat::xrgb8888}, {0, 0, w, hgt}, err)) {
            return false;
        }
        // 界面库从普通内存里读像素：拷一份（640×360×4 ≈ 0.9 MB）。以后可以让界面直接读这块 DMA-BUF
        out.width = w;
        out.height = hgt;
        out.stride = static_cast<std::size_t>(w) * 4;
        out.pixels.resize(out.stride * static_cast<std::size_t>(hgt));
        m->preview->begin_cpu_read();
        std::memcpy(out.pixels.data(), m->preview->data(), out.pixels.size());
        m->preview->end_cpu_read();
        return true;
    };
    if (m->slots.empty()) {
        return h;  // 不检测
    }
    h.npu_input = [m](const media::YuvView &yuv, int slot, std::string &err) {
        Impl::Slot &s = m->slots[static_cast<std::size_t>(slot)];
        s.src_w = yuv.layout.width;
        s.src_h = yuv.layout.height;
        s.lb = media::fit_letterbox(s.src_w, s.src_h, m->model_w, m->model_h);
        return media::rga_convert(yuv, {s.buf->fd(), m->model_w, m->model_h, m->model_w, media::RgbFormat::rgb888},
                                  {s.lb.left, s.lb.top, s.lb.width, s.lb.height}, err);
    };
#ifdef RKSEC_HAVE_RKNN
    h.infer = [m](int worker, int slot, std::vector<detect::Detection> &out, std::string &err) {
        const Impl::Slot &s = m->slots[static_cast<std::size_t>(slot)];
        detect::RknnModel &model = *m->models[static_cast<std::size_t>(worker)];
        if (!model.run(*s.buf, err)) {
            return false;
        }
        err.clear();  // err 在线程里反复用，先清掉；yolo11_decode 只在输出格式不对时写它
        out = detect::yolo11_decode(model.outputs(), m->model_w, m->model_h, m->post, err);
        if (!err.empty()) {
            return false;
        }
        detect::to_source(out, s.lb, s.src_w, s.src_h);
        return true;
    };
    if (!m->face) {
        return h;
    }
    h.face_input = [m](const media::YuvView &yuv, const face::Region &r, std::string &err) {
        face::FaceEngine &e = *m->face;
        const int side = e.detect_size();
        m->face_region = r;
        m->face_src_w = yuv.layout.width;
        m->face_src_h = yuv.layout.height;
        m->face_lb = media::fit_letterbox(r.width, r.height, side, side);
        // 每次裁的块宽高比不同，letterbox 的边也不同：整块先填灰，再让 RGA 写中间（320×320×3，CPU 填一次几十微秒）
        media::DmaBuffer &buf = e.detect_input();
        buf.begin_cpu_write();
        std::memset(buf.data(), kPad, buf.size());
        buf.end_cpu_write();
        const media::Letterbox &lb = m->face_lb;
        return media::rga_convert(yuv, {r.x, r.y, r.width, r.height},
                                  {buf.fd(), side, side, side, media::RgbFormat::rgb888},
                                  {lb.left, lb.top, lb.width, lb.height}, err);
    };
    h.face_infer = [m](DetectHooks::FaceOutput &out, std::string &err) {
        face::FaceEngine &e = *m->face;
        std::vector<face::Face> faces;
        if (!e.detect(faces, err)) {
            return false;
        }
        // 全部换到原画面坐标（320 输入 → 裁的那块 → 原画面），再按这个人的框挑脸（第 9a 步）
        const media::Letterbox &lb = m->face_lb;
        const face::Region &r = m->face_region;
        std::vector<face::Face> src = faces;
        for (face::Face &sf : src) {
            detect::Detection &b = sf.box;
            b.x1 = static_cast<float>(r.x + lb.to_source_x(static_cast<double>(b.x1)));
            b.x2 = static_cast<float>(r.x + lb.to_source_x(static_cast<double>(b.x2)));
            b.y1 = static_cast<float>(r.y + lb.to_source_y(static_cast<double>(b.y1)));
            b.y2 = static_cast<float>(r.y + lb.to_source_y(static_cast<double>(b.y2)));
            for (face::Point &pt : sf.landmarks) {
                pt.x = static_cast<float>(r.x + lb.to_source_x(static_cast<double>(pt.x)));
                pt.y = static_cast<float>(r.y + lb.to_source_y(static_cast<double>(pt.y)));
            }
        }
        out.faces = static_cast<int>(faces.size());
        const face::PickResult pick = face::pick_face_detail(src, r.person);
        out.pick = pick;
        const int i = pick.index;
        out.found = i >= 0;
        if (!out.found) {
            return true;
        }
        out.not_largest = i != face::largest_face(faces);
        const face::Face &f = faces[static_cast<std::size_t>(i)];
        out.pose = face::estimate_pose(f);
        out.box = src[static_cast<std::size_t>(i)].box;
        // 质量不够（抬头太高、糊、贴边、关键点乱）：当作没找到脸，不拿去识别
        out.quality = face::face_quality(src[static_cast<std::size_t>(i)], out.pose, m->face_src_w, m->face_src_h);
        if (out.quality != face::Quality::ok) {
            out.found = false;
            return true;
        }
        // 在 320×320 的人脸输入上对齐：那块是按人的大小缩放过的，脸在里面的大小差不多
        e.detect_input().begin_cpu_read();
        const bool ok = e.embed(e.detect_view(), f, out.embedding, err);
        e.detect_input().end_cpu_read();
        if (!ok) {
            return false;
        }
        out.box = src[static_cast<std::size_t>(i)].box;
        return true;
    };
#endif
    return h;
}

#else  // 没有 MPP 或 RGA

struct HwPipeline::Impl {
    std::string detect_error = "这次编译没有 MPP 或 RGA";
};

bool HwPipeline::recognizing() const
{
    return false;
}

const std::string &HwPipeline::face_error() const
{
    return impl_->detect_error;
}

PipelineOptions HwPipeline::options(const config::AppConfig & /*cfg*/) const
{
    return with_workers(0);
}

HwPipeline::HwPipeline() : impl_(std::make_unique<Impl>()) {}
HwPipeline::~HwPipeline() = default;

std::unique_ptr<HwPipeline> HwPipeline::open(const config::AppConfig & /*cfg*/, int /*workers*/, std::string &err)
{
    err = "这次编译没有 MPP 或 RGA（不是板子，或者没装 librockchip-mpp-dev、librga-dev）";
    return nullptr;
}

int HwPipeline::workers() const
{
    return 0;
}

const std::string &HwPipeline::detect_error() const
{
    return impl_->detect_error;
}

std::string HwPipeline::describe() const
{
    return {};
}

DetectHooks HwPipeline::hooks()
{
    return {};
}

#endif

}  // namespace rksec::app
