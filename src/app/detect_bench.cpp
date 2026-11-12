#include "app/detect_bench.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <thread>
#include <vector>

#include "app/capture_run.hpp"
#include "app/detect_pipeline.hpp"
#include "app/hw_pipeline.hpp"
#include "app/proc_stats.hpp"
#include "capture/capture_service.hpp"
#include "core/log.hpp"
#include "core/stage_timer.hpp"
#include "detect/coco_names.hpp"
#include "detect/yolo_post.hpp"
#include "media/dma_buffer.hpp"
#include "media/draw.hpp"
#include "media/jpeg_decoder.hpp"
#include "media/letterbox.hpp"
#include "ui/latest_image.hpp"
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

// 一个跟踪编号现在的身份（日志用）
std::string identity_text(const face::Identity &id)
{
    switch (id.state) {
    case face::IdState::known:
        return id.name;
    case face::IdState::stranger:
        return "陌生人";
    case face::IdState::pending:
        break;
    }
    return "未定";
}
constexpr int kExitOk = 0;
[[maybe_unused]] constexpr int kExitBad = 1;
constexpr int kExitNoHw = 2;
constexpr int kExitDevice = 4;
[[maybe_unused]] constexpr std::uint8_t kPad = 114;
[[maybe_unused]] constexpr int kImageRuns = 20;  // --detect-image 重复推理几次算耗时

[[maybe_unused]] void print_dets(const std::vector<detect::Detection> &dets)
{
    for (const auto &d : dets) {
        std::printf("  %-14s %.3f  (%4.0f, %4.0f, %4.0f, %4.0f)\n", std::string{detect::coco_name(d.cls)}.c_str(),
                    static_cast<double>(d.score), static_cast<double>(d.x1), static_cast<double>(d.y1),
                    static_cast<double>(d.x2), static_cast<double>(d.y2));
    }
}

// 对比两种预处理的结果时：选一帧“有把握”的（有分数 ≥ kCmpPick 的框），只比较分数 ≥ kCmpKeep 的框。
// 分数贴着阈值（0.25）的框，两种预处理之间很小的像素差别就能让它一边有、一边没有——那不说明坐标换算错了
[[maybe_unused]] constexpr float kCmpPick = 0.5F;
[[maybe_unused]] constexpr float kCmpKeep = 0.4F;

[[maybe_unused]] std::vector<detect::Detection> confident(const std::vector<detect::Detection> &d)
{
    std::vector<detect::Detection> out;
    for (const auto &x : d) {
        if (x.score >= kCmpKeep) {
            out.push_back(x);
        }
    }
    return out;
}

// 两组框是否一致（只看分数 ≥ kCmpKeep 的）：个数相同，一一配对后同类、IoU ≥ 0.8、分数差 ≤ 0.05
[[maybe_unused]] bool same_dets(const std::vector<detect::Detection> &all_a,
                                const std::vector<detect::Detection> &all_b, double &worst_iou)
{
    const auto a = confident(all_a);
    const auto b = confident(all_b);
    worst_iou = 1;
    if (a.size() != b.size()) {
        return false;
    }
    std::vector<bool> used(b.size(), false);
    for (const auto &x : a) {
        float best = 0;
        std::size_t at = b.size();
        for (std::size_t k = 0; k < b.size(); ++k) {
            if (!used[k] && b[k].cls == x.cls && detect::iou(x, b[k]) > best) {
                best = detect::iou(x, b[k]);
                at = k;
            }
        }
        if (at == b.size() || best < 0.8F || std::abs(b[at].score - x.score) > 0.05F) {
            worst_iou = at == b.size() ? 0 : std::min(worst_iou, static_cast<double>(best));
            return false;
        }
        used[at] = true;
        worst_iou = std::min(worst_iou, static_cast<double>(best));
    }
    return true;
}

[[maybe_unused]] detect::PostOptions post_options(const config::AppConfig &cfg, bool all_classes)
{
    detect::PostOptions o;
    o.score_threshold = cfg.detect.score_threshold;
    o.nms_threshold = cfg.detect.nms_threshold;
    if (!all_classes) {
        for (const auto &n : cfg.detect.classes) {
            o.classes.push_back(detect::coco_id(n));
        }
    }
    return o;
}

}  // namespace

// ---------------------------------------------------------------- --detect-image

#ifdef RKSEC_HAVE_RKNN

int detect_image(const config::AppConfig &cfg, const std::string &path, const std::string &save_path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LOG_E("detect", "打不开图片 %s", path.c_str());
        return kExitDevice;
    }
    const std::vector<std::uint8_t> jpeg{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::string err;
    media::JpegDecoder dec;
    media::Image img;
    if (!dec.decode(jpeg.data(), jpeg.size(), 1 << 14, img, err)) {  // 最大宽度给得很大 = 不缩小
        LOG_E("detect", "图片解码失败：%s", err.c_str());
        return kExitDevice;
    }

    auto model = detect::RknnModel::load(cfg.detect.model, 0, err);
    if (!model || !detect::check_yolo11(*model, err)) {
        LOG_E("detect", "%s", err.c_str());
        return kExitDevice;
    }
    std::printf("模型       %s\n           %s\n", cfg.detect.model.c_str(), model->describe().c_str());
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

    StageStats t_run{"推理（rknn_run）"};
    StageStats t_post{"后处理（解码框 + NMS）"};
    std::vector<detect::Detection> dets;
    const detect::PostOptions opts = post_options(cfg, true);
    for (int i = 0; i < kImageRuns + 1; ++i) {  // 第一次含初始化，不计
        const auto a = Clock::now();
        if (!model->run(*input, err)) {
            LOG_E("detect", "%s", err.c_str());
            return kExitDevice;
        }
        const auto b = Clock::now();
        dets = detect::yolo11_decode(model->outputs(), mw, mh, opts, err);
        const auto c = Clock::now();
        if (!err.empty()) {
            LOG_E("detect", "%s", err.c_str());
            return kExitDevice;
        }
        if (i > 0) {
            t_run.record(b - a);
            t_post.record(c - b);
        }
    }
    detect::to_source(dets, lb, img.width, img.height);
    std::printf("%s\n%s\n", t_run.report().c_str(), t_post.report().c_str());
    std::printf("结果       %zu 个框（阈值 %.2f，NMS %.2f，全部类别），坐标是原图像素：\n", dets.size(),
                static_cast<double>(opts.score_threshold), static_cast<double>(opts.nms_threshold));
    print_dets(dets);

    if (!save_path.empty()) {
        for (const auto &d : dets) {
            media::draw_rect(img, static_cast<int>(d.x1), static_cast<int>(d.y1), static_cast<int>(d.x2),
                             static_cast<int>(d.y2), d.cls == 0 ? 0x27AE60U : 0x2F80EDU, 3);
        }
        std::printf("已保存     %s%s\n", save_path.c_str(), media::save_ppm(img, save_path) ? "" : " —— 写失败");
    }
    return kExitOk;
}

#else

int detect_image(const config::AppConfig & /*cfg*/, const std::string & /*path*/, const std::string & /*save_path*/)
{
    std::printf("这次编译没有 RKNN（找不到 rknn_api.h 或 librknnrt），--detect-image 用不了\n");
    return kExitNoHw;
}

#endif

// ---------------------------------------------------------------- --detect-bench

#if defined(RKSEC_HAVE_MPP) && defined(RKSEC_HAVE_RGA) && defined(RKSEC_HAVE_RKNN)

int detect_bench(const config::AppConfig &cfg, int frames)
{
    std::vector<std::vector<std::uint8_t>> jpegs;
    LOG_I("bench", "从摄像头取 %d 帧……", frames);
    if (!grab_mjpeg(cfg, frames, jpegs)) {
        return kExitDevice;
    }
    std::string err;
    media::MppJpegDecoder dec;
    if (!dec.open({}, err)) {
        LOG_E("mpp", "%s", err.c_str());
        return kExitDevice;
    }
    auto single = detect::RknnModel::load(cfg.detect.model, 0, err);
    auto joint = single ? detect::RknnModel::load(cfg.detect.model, detect::RknnModel::kAllCores, err) : nullptr;
    if (!single || !joint || !detect::check_yolo11(*single, err)) {
        LOG_E("detect", "%s", err.c_str());
        return kExitDevice;
    }
    std::printf("模型       %s\n", single->describe().c_str());
    const int mw = single->input_width();
    const int mh = single->input_height();
    const std::size_t in_bytes = static_cast<std::size_t>(mw) * static_cast<std::size_t>(mh) * 3;
    auto hw_in = media::DmaBuffer::allocate(in_bytes, err);
    auto cpu_in = hw_in ? media::DmaBuffer::allocate(in_bytes, err) : nullptr;
    if (!hw_in || !cpu_in) {
        LOG_E("dma", "%s", err.c_str());
        return kExitDevice;
    }
    hw_in->begin_cpu_write();
    std::memset(hw_in->data(), kPad, hw_in->size());
    hw_in->end_cpu_write();
    const media::RgbTarget hw_t{hw_in->fd(), mw, mh, mw, media::RgbFormat::rgb888};
    const detect::PostOptions opts = post_options(cfg, false);

    StageStats t_dec{"MPP 解码"};
    StageStats t_rga{"RGA letterbox"};
    StageStats t_run1{"推理：绑定 1 个核"};
    StageStats t_run3{"推理：3 个核联合"};
    StageStats t_post{"后处理"};
    StageStats t_total{"整帧（解码 + RGA + 推理 1 核 + 后处理）"};
    std::uint64_t failed = 0;
    std::uint64_t persons = 0;
    std::size_t most = 0;
    // 用来对比两种预处理的那一帧：第一个有把握检测到目标的帧（都没有就用第 0 帧）。
    // 两边都是 0 个框时对比不出任何东西——letterbox 的坐标换算错了也看不出来
    std::vector<detect::Detection> first_hw;
    std::size_t cmp = 0;
    bool cmp_found = false;
    media::Letterbox lb;
    int src_w = 0;
    int src_h = 0;

    const double cpu0 = cpu_seconds();
    const auto w0 = Clock::now();
    for (std::size_t i = 0; i < jpegs.size(); ++i) {
        const auto a = Clock::now();
        auto f = dec.decode(jpegs[i].data(), jpegs[i].size(), err);
        const auto b = Clock::now();
        if (!f) {
            ++failed;
            continue;
        }
        src_w = f->layout.width;
        src_h = f->layout.height;
        lb = media::fit_letterbox(src_w, src_h, mw, mh);
        const bool ok = media::rga_convert(*f, hw_t, {lb.left, lb.top, lb.width, lb.height}, err);
        f.reset();
        const auto c = Clock::now();
        if (!ok || !single->run(*hw_in, err)) {
            if (failed++ < 3) {
                LOG_W("bench", "第 %zu 帧：%s", i, err.c_str());
            }
            continue;
        }
        const auto d = Clock::now();
        auto dets = detect::yolo11_decode(single->outputs(), mw, mh, opts, err);
        const auto e = Clock::now();
        detect::to_source(dets, lb, src_w, src_h);
        t_dec.record(b - a);
        t_rga.record(c - b);
        t_run1.record(d - c);
        t_post.record(e - d);
        t_total.record(e - a);
        persons += dets.size();
        most = std::max(most, dets.size());
        const bool sure = std::any_of(dets.begin(), dets.end(), [](const auto &x) { return x.score >= kCmpPick; });
        if (!cmp_found && (sure || i == 0)) {
            first_hw = dets;
            cmp = i;
            cmp_found = sure;
        }
    }
    const double wall = std::chrono::duration<double>(Clock::now() - w0).count();
    const double cpu_ms = (cpu_seconds() - cpu0) * 1000.0 / static_cast<double>(jpegs.size());

    // 3 个核联合跑一个模型：同样的输入（最后一帧）重复推理
    for (std::size_t i = 0; i < jpegs.size(); ++i) {
        const auto a = Clock::now();
        if (!joint->run(*hw_in, err)) {
            LOG_W("bench", "3 核联合推理失败：%s", err.c_str());
            break;
        }
        t_run3.record(Clock::now() - a);
    }

    // 基线：CPU 解码（全尺寸）+ CPU letterbox
    StageStats t_cpu{"基线：CPU 解码 + CPU letterbox"};
    media::JpegDecoder cpu_dec;
    media::Image img;
    std::vector<detect::Detection> first_cpu;
    const double c0 = cpu_seconds();
    for (std::size_t i = 0; i < jpegs.size(); ++i) {
        const auto a = Clock::now();
        if (!cpu_dec.decode(jpegs[i].data(), jpegs[i].size(), 1 << 14, img, err)) {
            continue;
        }
        const media::Letterbox clb = media::fit_letterbox(img.width, img.height, mw, mh);
        cpu_in->begin_cpu_write();
        media::letterbox_to_rgb(img, clb, cpu_in->data(), mw, mh, mw, kPad);
        cpu_in->end_cpu_write();
        t_cpu.record(Clock::now() - a);
        if (i == cmp && single->run(*cpu_in, err)) {
            first_cpu = detect::yolo11_decode(single->outputs(), mw, mh, opts, err);
            detect::to_source(first_cpu, clb, img.width, img.height);
        }
    }
    const double cpu_base_ms = (cpu_seconds() - c0) * 1000.0 / static_cast<double>(jpegs.size());

    std::printf("%s\n%s\n%s\n%s\n%s\n%s\n", t_dec.report().c_str(), t_rga.report().c_str(), t_run1.report().c_str(),
                t_run3.report().c_str(), t_post.report().c_str(), t_total.report().c_str());
    std::printf("硬件路径   %.1f 秒 %zu 帧，每帧 CPU %.2f ms；失败 %llu\n", wall, jpegs.size(), cpu_ms,
                static_cast<unsigned long long>(failed));
    std::printf("%s，每帧 CPU %.2f ms（对比上面的 MPP 解码 + RGA）\n", t_cpu.report().c_str(), cpu_base_ms);
    std::printf("检测       类别 %zu 种，平均每帧 %.2f 个框，最多 %zu 个\n", opts.classes.size(),
                static_cast<double>(persons) / static_cast<double>(std::max<std::size_t>(t_run1.summary().total, 1)),
                most);

    double worst = 0;
    const bool same = same_dets(first_hw, first_cpu, worst);
    std::printf(
        "对比       第 %zu 帧（只比分数 ≥ %.1f 的框）：硬件预处理 %zu 个，CPU 预处理 %zu 个，最差 IoU %.2f——%s%s\n",
        cmp, static_cast<double>(kCmpKeep), confident(first_hw).size(), confident(first_cpu).size(), worst,
        same ? "一致" : "不一致",
        cmp_found ? "" : "（没有一帧有把握地检测到目标（分数 ≥ 0.5），这个对比没有意义：人站在画面里再跑）");
    if (!first_hw.empty()) {
        std::printf("           硬件预处理的框（原画面坐标）：\n");
        print_dets(first_hw);
        std::printf("           CPU 预处理的框：\n");
        print_dets(first_cpu);
    }
    return same ? kExitOk : kExitBad;
}

#else

int detect_bench(const config::AppConfig & /*cfg*/, int /*frames*/)
{
    std::printf("这次编译没有 MPP、RGA 或 RKNN，--detect-bench 用不了\n");
    return kExitNoHw;
}

#endif

// ---------------------------------------------------------------- --detect-run

int detect_run(const config::AppConfig &cfg, int seconds, int workers)
{
    if (!HwPipeline::available()) {
        std::printf("这次编译没有 MPP 或 RGA，--detect-run 用不了\n");
        return kExitNoHw;
    }
    install_stop_signals();
    const int want = workers > 0 ? workers : (cfg.detect.enabled ? cfg.detect.workers : 0);
    std::string err;
    auto hw = HwPipeline::open(cfg, want, err);
    if (!hw) {
        LOG_E("pipeline", "%s", err.c_str());
        return kExitDevice;
    }
    if (want > 0 && hw->workers() == 0) {
        LOG_E("detect", "检测起不来：%s", hw->detect_error().c_str());
        return kExitDevice;
    }
    LOG_I("detect", "%s", hw->describe().c_str());
    if (!hw->recognizing()) {
        LOG_W("face", "不做人脸识别：%s", hw->face_error().c_str());
    }

    ui::LatestImage preview;
    DetectPipeline pipe{hw->options(cfg), hw->hooks(), preview};
    pipe.start();
    capture::CaptureService svc{capture::real_sys(), service_options(cfg),
                                [&pipe](FrameRef f) { pipe.push(std::move(f)); }};
    svc.start();
    LOG_I("pipeline", "运行 %d 秒，推理线程 %d 个（Ctrl+C 提前结束）", seconds, hw->workers());

    const auto t0 = Clock::now();
    const double cpu0 = cpu_seconds();
    const long rss0 = rss_kb();
    std::uint64_t boxes = 0;
    std::uint64_t taken = 0;
    auto next_report = t0 + std::chrono::seconds(5);
    auto next_face = t0 + std::chrono::seconds(1);
    std::uint64_t frames_last = 0;
    std::uint64_t results_last = 0;
    std::uint64_t face_attempts_last = 0;
    std::optional<DetectResult> latest;
    auto last = t0;
    while (!stop_requested() && Clock::now() - t0 < std::chrono::seconds(seconds)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));  // 模拟界面线程每 20 ms 拿一次
        preview.take();
        if (auto r = pipe.take_result()) {
            boxes += r->dets.size();
            ++taken;
            latest = std::move(r);
        }
        if (pipe.recognizing() && Clock::now() >= next_face) {
            // 人脸识别每秒一行：现在画面里每个人是谁，最近一次识别的脸多大（走远走近看什么时候认不出来）
            next_face += std::chrono::seconds(1);
            const auto ps = pipe.stats();
            if (ps.faces.attempts != face_attempts_last && latest) {
                face_attempts_last = ps.faces.attempts;
                std::string who;
                for (std::size_t i = 0; i < latest->tracks.size(); ++i) {
                    who += " #" + std::to_string(latest->tracks[i].id) + "=" + identity_text(latest->identities[i]);
                }
                const auto &f = ps.last_face;
                if (f.found) {
                    LOG_I("face",
                          "人：%s；最近一次 #%d 脸宽 %.0f 像素（裁的块宽 %d），最像 %s %.2f（第二 %.2f），"
                          "转头 %+.2f 抬低头 %.2f 检测 %.2f",
                          who.c_str(), f.track, static_cast<double>(f.width), f.region_width,
                          f.name.empty() ? "（底库空）" : f.name.c_str(), static_cast<double>(f.score),
                          static_cast<double>(f.second), static_cast<double>(f.pose.yaw),
                          static_cast<double>(f.pose.pitch), static_cast<double>(f.det_score));
                } else if (f.quality != face::Quality::ok) {
                    LOG_I("face", "人：%s；最近一次 #%d 脸质量不够，不识别：%s（转头 %+.2f 抬低头 %.2f 检测 %.2f）",
                          who.c_str(), f.track, face::quality_text(f.quality), static_cast<double>(f.pose.yaw),
                          static_cast<double>(f.pose.pitch), static_cast<double>(f.det_score));
                } else if (f.faces > 0) {
                    LOG_I("face",
                          "人：%s；最近一次 #%d 块里有 %d 张脸，都不像是这个人的：最接近的一张离中线 %.2f（限 %.2f）、"
                          "在人框 %.2f 深处（限 %.2f）、宽是人框的 %.2f（至少 %.2f）",
                          who.c_str(), f.track, f.faces, static_cast<double>(f.pick.offset_x),
                          static_cast<double>(face::kMaxOffsetX), static_cast<double>(f.pick.depth_y),
                          static_cast<double>(face::kMaxDepthY), static_cast<double>(f.pick.width_ratio),
                          static_cast<double>(face::kMinWidthRatio));
                } else {
                    LOG_I("face", "人：%s；最近一次 #%d 没找到脸（裁的块宽 %d）", who.c_str(), f.track, f.region_width);
                }
            }
        }
        if (Clock::now() >= next_report) {
            next_report += std::chrono::seconds(5);
            const auto now = Clock::now();
            const double dt = std::chrono::duration<double>(now - last).count();
            const auto cs = svc.stats();
            const auto ps = pipe.stats();
            LOG_I("pipeline", "摄像头 %.1f fps；检测 %.1f 次/秒，跳过 %llu；端到端 p50 %.1f ms p99 %.1f ms",
                  static_cast<double>(cs.frames - frames_last) / dt,
                  static_cast<double>(ps.results - results_last) / dt, static_cast<unsigned long long>(ps.skipped_busy),
                  pipe.detect_latency.summary().p50_ms, pipe.detect_latency.summary().p99_ms);
            frames_last = cs.frames;
            results_last = ps.results;
            last = now;
        }
    }
    svc.stop();
    pipe.stop();

    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    const auto cs = svc.stats();
    const auto ps = pipe.stats();
    std::printf("运行       %.0f 秒，推理线程 %d 个\n", secs, pipe.workers());
    std::printf("摄像头     %llu 帧（%.1f fps），丢帧 %llu\n", static_cast<unsigned long long>(cs.frames),
                static_cast<double>(cs.frames) / secs, static_cast<unsigned long long>(cs.lost));
    std::printf("流水线     解码 %llu（失败 %llu），输入被新帧挤掉 %llu；送去推理 %llu，推理线程都忙跳过 %llu\n",
                static_cast<unsigned long long>(ps.decoded), static_cast<unsigned long long>(ps.decode_failed),
                static_cast<unsigned long long>(ps.input_dropped), static_cast<unsigned long long>(ps.jobs),
                static_cast<unsigned long long>(ps.skipped_busy));
    std::printf("检测       结果 %llu 个（%.1f 次/秒），推理失败 %llu；乱序 %llu 次，最多同时等 %zu 个\n",
                static_cast<unsigned long long>(ps.results), static_cast<double>(ps.results) / secs,
                static_cast<unsigned long long>(ps.infer_failed), static_cast<unsigned long long>(ps.reordered),
                ps.max_pending);
    std::string per;
    for (std::size_t w = 0; w < ps.per_worker.size(); ++w) {
        per += (w == 0 ? "" : "、") + std::string{"NPU"} + std::to_string(w) + " " + std::to_string(ps.per_worker[w]);
    }
    std::printf("每个线程   %s\n", per.empty() ? "（不检测）" : per.c_str());
    std::printf("拿到结果   %llu 次，平均每次 %.2f 个框\n", static_cast<unsigned long long>(taken),
                taken > 0 ? static_cast<double>(boxes) / static_cast<double>(taken) : 0.0);
    std::printf("跟踪       出现过 %llu 个编号，删除 %llu 个，被挡住后找回 %llu 次，结束时还有 %zu 个\n",
                static_cast<unsigned long long>(ps.tracks.created), static_cast<unsigned long long>(ps.tracks.removed),
                static_cast<unsigned long long>(ps.tracks.recovered), ps.tracks.active);
    if (pipe.recognizing()) {
        const auto &f = ps.faces;
        const std::uint64_t face_failed = ps.face_input_failed + ps.face_infer_failed;
        std::printf("人脸识别   %llu 次：没找到脸 %llu，认出来 %llu，陌生人 %llu；采纳新结论 %llu 次；失败 %llu\n",
                    static_cast<unsigned long long>(f.attempts), static_cast<unsigned long long>(f.no_face),
                    static_cast<unsigned long long>(f.matched), static_cast<unsigned long long>(f.unmatched),
                    static_cast<unsigned long long>(f.adopted), static_cast<unsigned long long>(face_failed));
        std::printf("           找到过的最小的脸宽 %.0f 像素，认出来过的最小的脸宽 %.0f 像素（原画面）\n",
                    static_cast<double>(ps.smallest_face), static_cast<double>(ps.smallest_matched_face));
        std::printf("%s", face_report(ps, cfg.face.match_threshold).c_str());
    }
    for (const StageStats *s : {&pipe.decode_time, &pipe.preview_time, &pipe.npu_input_time, &pipe.infer_time,
                                &pipe.preview_latency, &pipe.detect_latency, &pipe.face_input_time, &pipe.face_time}) {
        std::printf("%s\n", s->report().c_str());
    }
    std::printf("CPU        平均 %.0f%%（100%% = 一个核满载）\n", secs > 0 ? (cpu_seconds() - cpu0) / secs * 100.0 : 0);
    std::printf("内存 RSS   %ld KB → %ld KB\n", rss0, rss_kb());
    return kExitOk;
}

}  // namespace rksec::app
