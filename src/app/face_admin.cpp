#include "app/face_admin.hpp"

#include <cstdio>
#include <map>

#include "app/capture_run.hpp"
#include "app/face_tool.hpp"
#include "core/log.hpp"
#include "face/enroll.hpp"
#include "face/face_db.hpp"
#include "media/jpeg_decoder.hpp"
#include "media/jpeg_encoder.hpp"
#include "ui/glyphs.hpp"
#include "ui/lvgl_port.hpp"
#ifdef RKSEC_HAVE_RKNN
#include "face/face_engine.hpp"
#endif

namespace rksec::app {

namespace {

constexpr int kExitOk = 0;
[[maybe_unused]] constexpr int kExitRejected = 1;
[[maybe_unused]] constexpr int kExitNoHw = 2;
constexpr int kExitDevice = 4;
[[maybe_unused]] constexpr int kWarmupFrames = 10;      // 摄像头刚打开的几帧曝光还没稳定，不用
[[maybe_unused]] constexpr int kEnrollMaxFrames = 300;  // 最多等 12 秒左右，挑够了就提前结束
[[maybe_unused]] constexpr int kAlignedQuality = 95;    // 对齐小图的 JPEG 质量

std::unique_ptr<face::FaceDb> open_db(const config::AppConfig &cfg)
{
    std::string err;
    auto db = face::FaceDb::open(cfg.face.database, err);
    if (!db) {
        LOG_E("face", "%s", err.c_str());
    }
    return db;
}

#ifdef RKSEC_HAVE_RKNN

// 名字合不合规矩、界面能不能显示
bool name_ok(const std::string &name)
{
    std::string err;
    if (!face::check_name(name, err)) {
        std::printf("不能登记：%s\n", err.c_str());
        return false;
    }
    ui::lvgl_init();
    const std::string missing = ui::missing_name_glyphs(name);
    ui::lvgl_deinit();
    if (!missing.empty()) {
        std::printf("不能登记：界面字体里没有“%s”（人名用字是 GB2312 一级汉字 3755 个，生僻字请换成拼音）\n",
                    missing.c_str());
        return false;
    }
    return true;
}

face::EnrollCandidate to_candidate(int frame, const ImageFace &r)
{
    face::EnrollCandidate c;
    c.frame = frame;
    c.faces = r.faces;
    if (r.faces > 0) {
        c.score = r.face.box.score;
        c.width = r.face.box.x2 - r.face.box.x1;
        c.embedding = r.embedding;
    }
    return c;
}

#endif

}  // namespace

bool load_gallery(const config::AppConfig &cfg, face::Gallery &gallery, std::string &err)
{
    auto db = face::FaceDb::open(cfg.face.database, err);
    if (!db) {
        return false;
    }
    int skipped = 0;
    if (!db->load(face::model_key(cfg.face.recognize_model), gallery, skipped, err)) {
        return false;
    }
    LOG_I("face", "人脸库 %s：%zu 人，%zu 张照片", cfg.face.database.c_str(), gallery.people(), gallery.size());
    if (skipped > 0) {
        LOG_W("face", "人脸库里 %d 张照片的特征是别的识别模型算的，没有用上：运行 --face-reembed 重新算", skipped);
    }
    return true;
}

#ifdef RKSEC_HAVE_RKNN

int face_enroll(const config::AppConfig &cfg, const std::string &name, const std::vector<std::string> &photos)
{
    if (!name_ok(name)) {
        return kExitRejected;
    }
    std::string err;
    auto engine = face::FaceEngine::open(cfg.face, err);
    if (!engine) {
        LOG_E("face", "%s", err.c_str());
        return kExitDevice;
    }
    auto db = open_db(cfg);
    if (!db) {
        return kExitDevice;
    }
    face::Gallery existing;
    int skipped = 0;
    if (!db->load(face::model_key(cfg.face.recognize_model), existing, skipped, err)) {
        LOG_E("face", "%s", err.c_str());
        return kExitDevice;
    }

    // 1. 每帧（每张照片）找脸、算特征
    face::EnrollOptions opts;
    std::vector<face::EnrollCandidate> cands;
    std::vector<std::vector<std::uint8_t>> aligned;  // 和 cands 一一对应
    std::vector<std::string> sources;
    const auto add = [&](int frame, const media::Image &img, const std::string &source) -> bool {
        ImageFace r;
        if (!embed_image(*engine, img, r, err)) {
            LOG_E("face", "%s", err.c_str());
            return false;
        }
        cands.push_back(to_candidate(frame, r));
        aligned.push_back(r.aligned);
        sources.push_back(source);
        return true;
    };
    if (photos.empty()) {
        // 边取边找脸，挑够 want 张就停（第一版先取 3 秒再处理：人还没站好时间就到了，75 帧里 66 帧没脸）
        std::printf("看着镜头，可以慢慢左右转一点头（画面里只能有一个人），挑够 %d 张自动结束，最多等 12 秒……\n",
                    opts.want);
        media::JpegDecoder dec;
        int index = 0;
        std::size_t shown = 0;
        bool failed = false;
        const auto on_frame = [&](const std::uint8_t *data, std::size_t bytes) {
            const int i = index++;
            media::Image img;
            if (i < kWarmupFrames || !dec.decode(data, bytes, 1 << 14, img, err)) {
                return true;  // 曝光没稳定的前几帧、偶尔的坏帧：跳过
            }
            if (!add(i - kWarmupFrames, img, "camera")) {
                failed = true;
                return false;
            }
            const std::size_t n = face::pick_enroll_frames(cands, opts).size();
            if (n > shown) {
                shown = n;
                std::printf("  已挑到 %zu / %d\n", n, opts.want);
                std::fflush(stdout);
            }
            return static_cast<int>(n) < opts.want;
        };
        if (!for_each_mjpeg(cfg, kWarmupFrames + kEnrollMaxFrames, on_frame) || failed) {
            return kExitDevice;
        }
    } else {
        opts.want = static_cast<int>(photos.size());
        opts.min_gap = 0;
        for (std::size_t k = 0; k < photos.size(); ++k) {
            media::Image img;
            if (!decode_jpeg_file(photos[k], img, err)) {
                LOG_E("face", "%s", err.c_str());
                return kExitDevice;
            }
            if (!add(static_cast<int>(k), img, photos[k])) {
                return kExitDevice;
            }
        }
    }

    // 2. 挑帧。照片模式下每张都必须合格
    std::map<std::string, int> reasons;
    for (std::size_t i = 0; i < cands.size(); ++i) {
        if (const std::string r = face::reject_reason(cands[i], opts); !r.empty()) {
            ++reasons[r.substr(0, r.find("（"))];
            if (!photos.empty()) {
                std::printf("不能登记：%s %s\n", photos[i].c_str(), r.c_str());
                return kExitRejected;
            }
        }
    }
    const std::vector<std::size_t> picked = face::pick_enroll_frames(cands, opts);
    int rejected = 0;
    for (const auto &[r, c] : reasons) {
        rejected += c;
    }
    std::printf("可用 %d / %zu %s", static_cast<int>(cands.size()) - rejected, cands.size(),
                photos.empty() ? "帧" : "张");
    for (const auto &[r, c] : reasons) {
        std::printf("；%s %d", r.c_str(), c);
    }
    std::printf("\n");
    if (picked.empty()) {
        std::printf("不能登记：没有合格的画面（靠近一点、正对镜头、光线亮一点）\n");
        return kExitRejected;
    }
    std::vector<std::vector<float>> embs;
    for (const std::size_t i : picked) {
        const auto &c = cands[i];
        std::printf("  选中 %s 第 %d %s：脸宽 %.0f 像素，分数 %.3f\n", photos.empty() ? "画面" : "照片",
                    photos.empty() ? c.frame : c.frame + 1, photos.empty() ? "帧" : "张", static_cast<double>(c.width),
                    static_cast<double>(c.score));
        embs.push_back(c.embedding);
    }

    // 3. 检查：像同一个人、不像别人
    const face::EnrollCheck chk = face::check_enroll(embs, name, existing, cfg.face.match_threshold, opts);
    std::printf("  几张之间最低相似度 %.2f；底库里最像的别人：%s\n", static_cast<double>(chk.min_self),
                chk.closest_other.empty()
                    ? "（没有别人）"
                    : (chk.closest_other + " " + std::to_string(chk.closest_score).substr(0, 4)).c_str());
    if (!chk.ok) {
        std::printf("不能登记：%s\n", chk.reason.c_str());
        return kExitRejected;
    }

    // 4. 存：特征 + 对齐小图
    std::vector<face::NewFace> faces;
    for (const std::size_t i : picked) {
        face::NewFace f;
        f.embedding = cands[i].embedding;
        f.source = sources[i];
        if (!media::encode_rgb_jpeg(aligned[i].data(), face::kAlignedSize, face::kAlignedSize, kAlignedQuality,
                                    f.aligned, err)) {
            LOG_E("face", "%s", err.c_str());
            return kExitDevice;
        }
        faces.push_back(std::move(f));
    }
    if (!db->add(name, faces, face::model_key(cfg.face.recognize_model), err)) {
        LOG_E("face", "%s", err.c_str());
        return kExitDevice;
    }
    std::printf("已登记“%s”：%zu 张（人脸库 %s）。正在运行的界面要重启才会用上\n", name.c_str(), faces.size(),
                cfg.face.database.c_str());
    return kExitOk;
}

int face_reembed(const config::AppConfig &cfg)
{
    std::string err;
    auto engine = face::FaceEngine::open(cfg.face, err);
    if (!engine) {
        LOG_E("face", "%s", err.c_str());
        return kExitDevice;
    }
    auto db = open_db(cfg);
    if (!db) {
        return kExitDevice;
    }
    std::vector<face::StoredFace> all;
    if (!db->faces(all, err)) {
        LOG_E("face", "%s", err.c_str());
        return kExitDevice;
    }
    const std::string model = face::model_key(cfg.face.recognize_model);
    media::JpegDecoder dec;
    int done = 0;
    int failed = 0;
    for (const auto &f : all) {
        media::Image img;
        // 小图已经对齐好了：关键点就在参考位置上，“对齐”只是原样拷贝
        face::Face ref;
        ref.landmarks = face::kArcFaceRef;
        std::vector<float> e;
        if (f.aligned.empty() || !dec.decode(f.aligned.data(), f.aligned.size(), 1 << 14, img, err) ||
            !engine->embed({img.pixels.data(), img.width, img.height, img.stride, face::PixelFormat::xrgb8888}, ref, e,
                           err) ||
            !db->update(f.id, e, model, err)) {
            std::printf("  %s 的第 %lld 号照片失败：%s\n", f.name.c_str(), static_cast<long long>(f.id),
                        f.aligned.empty() ? "没有存小图，只能重新登记" : err.c_str());
            ++failed;
            continue;
        }
        ++done;
    }
    std::printf("用 %s 重新算了 %d 张照片的特征，失败 %d 张\n", model.c_str(), done, failed);
    return failed == 0 ? kExitOk : kExitRejected;
}

#else

int face_enroll(const config::AppConfig & /*cfg*/, const std::string & /*name*/,
                const std::vector<std::string> & /*photos*/)
{
    std::printf("这次编译没有 RKNN，--enroll 用不了\n");
    return kExitNoHw;
}

int face_reembed(const config::AppConfig & /*cfg*/)
{
    std::printf("这次编译没有 RKNN，--face-reembed 用不了\n");
    return kExitNoHw;
}

#endif

int face_list(const config::AppConfig &cfg)
{
    auto db = open_db(cfg);
    if (!db) {
        return kExitDevice;
    }
    const std::string model = face::model_key(cfg.face.recognize_model);
    std::vector<face::PersonInfo> people;
    std::string err;
    if (!db->people(model, people, err)) {
        LOG_E("face", "%s", err.c_str());
        return kExitDevice;
    }
    std::printf("人脸库 %s（当前识别模型 %s）\n", cfg.face.database.c_str(), model.c_str());
    int total = 0;
    int stale = 0;
    for (const auto &p : people) {
        std::printf("  %-16s %d 张  登记于 %s%s\n", p.name.c_str(), p.faces, p.created.c_str(),
                    p.other_model > 0 ? "  （有照片要重新算特征）" : "");
        total += p.faces;
        stale += p.other_model;
    }
    std::printf("共 %zu 人，%d 张照片\n", people.size(), total);
    if (stale > 0) {
        std::printf("%d 张照片的特征是别的模型算的：运行 --face-reembed\n", stale);
    }
    return kExitOk;
}

int face_unenroll(const config::AppConfig &cfg, const std::string &name)
{
    auto db = open_db(cfg);
    if (!db) {
        return kExitDevice;
    }
    int removed = 0;
    std::string err;
    if (!db->remove(name, removed, err)) {
        LOG_E("face", "%s", err.c_str());
        return kExitDevice;
    }
    if (removed < 0) {
        std::printf("人脸库里没有“%s”\n", name.c_str());
        return kExitRejected;
    }
    std::printf("已删除“%s”和他的 %d 张照片。正在运行的界面要重启才会生效\n", name.c_str(), removed);
    return kExitOk;
}

}  // namespace rksec::app
