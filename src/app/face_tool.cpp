#include "app/face_tool.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "core/log.hpp"
#include "core/stage_timer.hpp"
#include "media/letterbox.hpp"
#include "media/yuv_frame.hpp"
#ifdef RKSEC_HAVE_RKNN
#include "face/face_engine.hpp"
#endif

namespace rksec::app {

namespace {

using Clock = std::chrono::steady_clock;
[[maybe_unused]] constexpr int kExitOk = 0;
[[maybe_unused]] constexpr int kExitNoHw = 2;
[[maybe_unused]] constexpr int kExitDevice = 4;
[[maybe_unused]] constexpr std::uint8_t kPad = 114;  // letterbox 补边（和第 7b 步 --face-image 一样）
[[maybe_unused]] constexpr int kTimingRuns = 20;

#ifdef RKSEC_HAVE_RKNN

// 对齐后的 RGB 112×112 → Image（B G R X），存 PPM 用
media::Image rgb_to_image(const std::vector<std::uint8_t> &rgb, int w, int h)
{
    media::Image img;
    img.width = w;
    img.height = h;
    img.stride = static_cast<std::size_t>(w) * 4;
    img.pixels.assign(img.stride * static_cast<std::size_t>(h), 255);
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    for (std::size_t i = 0; i < n; ++i) {
        img.pixels[i * 4] = rgb[i * 3 + 2];
        img.pixels[i * 4 + 1] = rgb[i * 3 + 1];
        img.pixels[i * 4 + 2] = rgb[i * 3];
    }
    return img;
}

#endif

}  // namespace

bool decode_jpeg_file(const std::string &path, media::Image &img, std::string &err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "打不开图片 " + path;
        return false;
    }
    const std::vector<std::uint8_t> jpeg{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    media::JpegDecoder dec;
    if (!dec.decode(jpeg.data(), jpeg.size(), 1 << 14, img, err)) {
        err = path + " 解码失败：" + err;
        return false;
    }
    return true;
}

#ifdef RKSEC_HAVE_RKNN

bool embed_image(face::FaceEngine &engine, const media::Image &img, ImageFace &out, std::string &err)
{
    const int side = engine.detect_size();
    const media::Letterbox lb = media::fit_letterbox(img.width, img.height, side, side);
    media::DmaBuffer &in = engine.detect_input();
    in.begin_cpu_write();
    media::letterbox_to_rgb(img, lb, in.data(), side, side, side, kPad);
    in.end_cpu_write();
    std::vector<face::Face> faces;
    if (!engine.detect(faces, err)) {
        return false;
    }
    face::to_source(faces, lb, img.width, img.height);
    out = ImageFace{};
    out.faces = static_cast<int>(faces.size());
    const int i = face::largest_face(faces);
    if (i < 0) {
        return true;
    }
    out.face = faces[static_cast<std::size_t>(i)];
    const face::ImageView src{img.pixels.data(), img.width, img.height, img.stride, face::PixelFormat::xrgb8888};
    return engine.embed(src, out.face, out.embedding, err, &out.aligned, &out.raw_norm);
}

int face_embed(const config::AppConfig &cfg, const std::vector<std::string> &paths, const std::string &save_dir)
{
    std::string err;
    auto engine = face::FaceEngine::open(cfg.face, err);
    if (!engine) {
        LOG_E("face", "%s", err.c_str());
        return kExitDevice;
    }
    std::printf("模型       %s\n", engine->describe().c_str());
    if (!save_dir.empty()) {
        std::filesystem::create_directories(save_dir);
    }

    std::vector<std::string> labels;
    std::vector<std::vector<float>> embs;
    StageStats t_all{"一张图合计（CPU letterbox + 检测 + 对齐 + 识别）"};
    StageStats t_rec{"其中对齐 + 识别模型"};
    for (std::size_t k = 0; k < paths.size(); ++k) {
        const std::string &path = paths[k];
        media::Image img;
        ImageFace r;
        if (!decode_jpeg_file(path, img, err) || !embed_image(*engine, img, r, err)) {
            LOG_E("face", "%s", err.c_str());
            return kExitDevice;
        }
        const std::string name = face::name_from_file(path);
        std::printf("[%zu] %s  %dx%d：%d 张脸", k + 1, path.c_str(), img.width, img.height, r.faces);
        if (r.faces == 0) {
            std::printf("，跳过\n");
            continue;
        }
        const detect::Detection &b = r.face.box;
        std::printf("，取最大的 %.3f (%.0f, %.0f, %.0f, %.0f) 宽 %.0f\n", static_cast<double>(b.score),
                    static_cast<double>(b.x1), static_cast<double>(b.y1), static_cast<double>(b.x2),
                    static_cast<double>(b.y2), static_cast<double>(b.x2 - b.x1));
        std::printf("     特征长度（归一化前）%.2f，前 4 个数 %.4f %.4f %.4f %.4f\n", static_cast<double>(r.raw_norm),
                    static_cast<double>(r.embedding[0]), static_cast<double>(r.embedding[1]),
                    static_cast<double>(r.embedding[2]), static_cast<double>(r.embedding[3]));
        if (k == 0) {
            // 第一张图测一下耗时
            for (int i = 0; i < kTimingRuns; ++i) {
                ImageFace tmp;
                const auto a = Clock::now();
                embed_image(*engine, img, tmp, err);
                t_all.record(Clock::now() - a);
                const face::ImageView src{img.pixels.data(), img.width, img.height, img.stride,
                                          face::PixelFormat::xrgb8888};
                std::vector<float> e;
                const auto c = Clock::now();
                engine->embed(src, r.face, e, err);
                t_rec.record(Clock::now() - c);
            }
        }
        if (!save_dir.empty()) {
            std::string base = save_dir;
            base += "/" + std::to_string(k + 1);
            base += "_" + name;
            const bool ppm =
                media::save_ppm(rgb_to_image(r.aligned, face::kAlignedSize, face::kAlignedSize), base + ".aligned.ppm");
            std::ofstream txt(base + ".emb.txt");
            for (const float v : r.embedding) {
                txt << v << "\n";
            }
            std::printf("     已保存 %s.aligned.ppm、%s.emb.txt%s\n", base.c_str(), base.c_str(),
                        ppm && txt ? "" : " —— 写失败");
        }
        labels.push_back("[" + std::to_string(k + 1) + "] " + name);
        embs.push_back(r.embedding);
    }
    if (t_all.summary().count > 0) {
        std::printf("%s\n%s\n", t_all.report().c_str(), t_rec.report().c_str());
    }
    if (embs.size() >= 2) {
        std::printf("相似度（同一个人应该高、不同人应该低；阈值 face.match_threshold = %.2f）：\n",
                    static_cast<double>(cfg.face.match_threshold));
        for (std::size_t i = 0; i < embs.size(); ++i) {
            for (std::size_t j = i + 1; j < embs.size(); ++j) {
                const float s = face::cosine(embs[i], embs[j]);
                std::printf("  %s  ↔  %s   %.3f%s\n", labels[i].c_str(), labels[j].c_str(), static_cast<double>(s),
                            s >= cfg.face.match_threshold ? "  （≥ 阈值，算同一个人）" : "");
            }
        }
    }
    return kExitOk;
}

#else

int face_embed(const config::AppConfig & /*cfg*/, const std::vector<std::string> & /*paths*/,
               const std::string & /*save_dir*/)
{
    std::printf("这次编译没有 RKNN，--face-embed 用不了\n");
    return kExitNoHw;
}

#endif

}  // namespace rksec::app
