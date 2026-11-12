#include "app/rga_bench.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "app/capture_run.hpp"
#include "app/proc_stats.hpp"
#include "core/log.hpp"
#include "core/stage_timer.hpp"
#include "media/jpeg_decoder.hpp"
#include "media/yuv_frame.hpp"
#if defined(RKSEC_HAVE_MPP) && defined(RKSEC_HAVE_RGA)
#include "media/dma_buffer.hpp"
#include "media/letterbox.hpp"
#include "media/mpp_jpeg_decoder.hpp"
#include "media/rga_converter.hpp"
#endif

namespace rksec::app {

#if defined(RKSEC_HAVE_MPP) && defined(RKSEC_HAVE_RGA)

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kExitOk = 0;
constexpr int kExitBad = 1;
constexpr int kExitDevice = 4;
constexpr int kModel = 640;         // YOLO 输入 640×640
constexpr std::uint8_t kPad = 114;  // letterbox 补边的灰度（和 rknn_model_zoo 一致）
constexpr int kPreviewW = 640;
constexpr int kPreviewH = 360;
constexpr double kMaxMeanDiff = 4.0;  // 和 CPU 参考结果的平均差（缩放算法不同，差几级正常）

// CPU 参考：YUV 全尺寸转 RGB，再按 2×2 求平均缩小一半（1280×720 → 640×360）
media::Image cpu_half(const media::YuvView &v)
{
    media::Image full;
    media::yuv_to_xrgb(v, full);
    media::Image half;
    half.width = full.width / 2;
    half.height = full.height / 2;
    half.stride = static_cast<std::size_t>(half.width) * 4;
    half.pixels.resize(half.stride * static_cast<std::size_t>(half.height));
    for (int y = 0; y < half.height; ++y) {
        for (int x = 0; x < half.width; ++x) {
            for (int c = 0; c < 4; ++c) {
                const auto at = [&](int yy, int xx) {
                    return full.pixels[static_cast<std::size_t>(yy) * full.stride + static_cast<std::size_t>(xx) * 4 +
                                       static_cast<std::size_t>(c)];
                };
                const int sum =
                    at(2 * y, 2 * x) + at(2 * y, 2 * x + 1) + at(2 * y + 1, 2 * x) + at(2 * y + 1, 2 * x + 1);
                half.pixels[static_cast<std::size_t>(y) * half.stride + static_cast<std::size_t>(x) * 4 +
                            static_cast<std::size_t>(c)] = static_cast<std::uint8_t>((sum + 2) / 4);
            }
        }
    }
    return half;
}

// RGB888 的一块区域 → Image（存图、比较用）
media::Image from_rgb888(const std::uint8_t *data, int stride_px, int x0, int y0, int w, int h)
{
    media::Image img;
    img.width = w;
    img.height = h;
    img.stride = static_cast<std::size_t>(w) * 4;
    img.pixels.resize(img.stride * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) {
        const std::uint8_t *s =
            data +
            (static_cast<std::size_t>(y0 + y) * static_cast<std::size_t>(stride_px) + static_cast<std::size_t>(x0)) * 3;
        std::uint8_t *d = img.pixels.data() + static_cast<std::size_t>(y) * img.stride;
        for (int x = 0; x < w; ++x, s += 3, d += 4) {
            d[0] = s[2];  // B
            d[1] = s[1];  // G
            d[2] = s[0];  // R
            d[3] = 0xFF;
        }
    }
    return img;
}

// 两张同尺寸 XRGB 图 R、G、B 的平均差和最大差
void diff(const media::Image &a, const media::Image &b, double &mean, int &worst)
{
    long long sum = 0;
    long long n = 0;
    worst = 0;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            for (int c = 0; c < 3; ++c) {
                const std::size_t ia = static_cast<std::size_t>(y) * a.stride + static_cast<std::size_t>(x * 4 + c);
                const std::size_t ib = static_cast<std::size_t>(y) * b.stride + static_cast<std::size_t>(x * 4 + c);
                const int d = std::abs(static_cast<int>(a.pixels[ia]) - static_cast<int>(b.pixels[ib]));
                sum += d;
                ++n;
                worst = d > worst ? d : worst;
            }
        }
    }
    mean = n > 0 ? static_cast<double>(sum) / static_cast<double>(n) : 0;
}

}  // namespace

int rga_bench(const config::AppConfig &cfg, int frames, const std::string &save_dir)
{
    std::vector<std::vector<std::uint8_t>> jpegs;
    LOG_I("bench", "从摄像头取 %d 帧……（librga %s）", frames, media::rga_version().c_str());
    if (!grab_mjpeg(cfg, frames, jpegs)) {
        return kExitDevice;
    }
    std::string err;
    media::MppJpegDecoder dec;
    if (!dec.open({}, err)) {
        LOG_E("mpp", "%s", err.c_str());
        return kExitDevice;
    }

    // 输出缓冲区：NPU 输入 640×640×3，预览 640×360×4
    const auto npu = media::DmaBuffer::allocate(static_cast<std::size_t>(kModel) * kModel * 3, err);
    const auto prev =
        npu ? media::DmaBuffer::allocate(static_cast<std::size_t>(kPreviewW) * kPreviewH * 4, err) : nullptr;
    if (!npu || !prev) {
        LOG_E("dma", "%s", err.c_str());
        return kExitDevice;
    }
    // letterbox 的灰边只填一次：RGA 每帧只写中间的画面区域
    npu->begin_cpu_write();
    std::memset(npu->data(), kPad, npu->size());
    npu->end_cpu_write();

    const media::RgbTarget npu_t{npu->fd(), kModel, kModel, kModel, media::RgbFormat::rgb888};
    const media::RgbTarget prev_t{prev->fd(), kPreviewW, kPreviewH, kPreviewW, media::RgbFormat::xrgb8888};

    StageStats t_dec{"MPP 解码"};
    StageStats t_npu{"RGA → NPU 输入 640×640 RGB（letterbox）"};
    StageStats t_prev{"RGA → 预览 640×360 XRGB"};
    StageStats t_path{"整条预览路径（MPP 解码 + RGA 预览）"};
    std::uint64_t failed = 0;
    media::MppJpegDecoder::Frame first;
    media::Letterbox lb;

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
        t_dec.record(b - a);
        lb = media::fit_letterbox(f->layout.width, f->layout.height, kModel, kModel);
        const bool ok1 = media::rga_convert(*f, npu_t, {lb.left, lb.top, lb.width, lb.height}, err);
        const auto c = Clock::now();
        const bool ok2 = ok1 && media::rga_convert(*f, prev_t, {0, 0, kPreviewW, kPreviewH}, err);
        const auto d = Clock::now();
        if (!ok1 || !ok2) {
            if (failed++ < 3) {
                LOG_W("rga", "第 %zu 帧：%s", i, err.c_str());
            }
            continue;
        }
        t_npu.record(c - b);
        t_prev.record(d - c);
        t_path.record((b - a) + (d - c));
        if (i == 0) {
            first = f;
        }
    }
    const double wall = std::chrono::duration<double>(Clock::now() - w0).count();
    const double cpu = wall > 0 ? (cpu_seconds() - cpu0) / wall * 100.0 : 0;
    const double cpu_ms = (cpu_seconds() - cpu0) * 1000.0 / static_cast<double>(jpegs.size());

    // 第 4b 步的方法做对比：CPU 解码时缩小到 640 宽
    StageStats t_cpu{"4b 方法：CPU 解码并缩小到 640 宽"};
    {
        media::JpegDecoder cpu_dec;
        media::Image img;
        const double c0 = cpu_seconds();
        for (const auto &j : jpegs) {
            const auto s = Clock::now();
            if (cpu_dec.decode(j.data(), j.size(), kPreviewW, img, err)) {
                t_cpu.record(Clock::now() - s);
            }
        }
        std::printf("%s，每帧 CPU %.2f ms\n", t_cpu.report().c_str(),
                    (cpu_seconds() - c0) * 1000.0 / static_cast<double>(jpegs.size()));
    }
    std::printf("%s\n%s\n%s\n%s\n", t_dec.report().c_str(), t_npu.report().c_str(), t_prev.report().c_str(),
                t_path.report().c_str());
    std::printf("硬件路径   循环期间 CPU %.0f%%，每帧 CPU %.2f ms（解码 + 两次 RGA）；失败 %llu\n", cpu, cpu_ms,
                static_cast<unsigned long long>(failed));
    std::printf("letterbox  %dx%d 放在 (%d, %d)，比例 %.4f × %.4f\n", lb.width, lb.height, lb.left, lb.top, lb.scale_x,
                lb.scale_y);

    int rc = kExitOk;
    if (!first) {
        std::printf("对比       没有成功的帧\n");
        return kExitBad;
    }
    // 重新转换第一帧（循环里的输出已经被后面的帧覆盖），再和 CPU 参考比较
    if (!media::rga_convert(*first, npu_t, {lb.left, lb.top, lb.width, lb.height}, err) ||
        !media::rga_convert(*first, prev_t, {0, 0, kPreviewW, kPreviewH}, err)) {
        std::printf("对比       重新转换第一帧失败：%s\n", err.c_str());
        return kExitBad;
    }
    const media::Image ref = cpu_half(*first);

    prev->begin_cpu_read();
    media::Image prev_img;
    prev_img.width = kPreviewW;
    prev_img.height = kPreviewH;
    prev_img.stride = static_cast<std::size_t>(kPreviewW) * 4;
    prev_img.pixels.assign(prev->data(), prev->data() + prev->size());
    prev->end_cpu_read();

    npu->begin_cpu_read();
    const media::Image npu_img = from_rgb888(npu->data(), kModel, lb.left, lb.top, lb.width, lb.height);
    const media::Image npu_all = from_rgb888(npu->data(), kModel, 0, 0, kModel, kModel);
    // 灰边有没有被改动：检查第一行和最后一行
    bool border_ok = true;
    for (int x = 0; x < kModel * 3 && border_ok; ++x) {
        border_ok = npu->data()[x] == kPad && npu->data()[npu->size() - 1 - static_cast<std::size_t>(x)] == kPad;
    }
    npu->end_cpu_read();

    double m1 = 0;
    double m2 = 0;
    int w1 = 0;
    int w2 = 0;
    diff(prev_img, ref, m1, w1);
    if (npu_img.width == ref.width && npu_img.height == ref.height) {
        diff(npu_img, ref, m2, w2);
    } else {
        m2 = 999;
    }
    const bool good = m1 <= kMaxMeanDiff && m2 <= kMaxMeanDiff && border_ok;
    std::printf("对比       预览 vs CPU：平均差 %.2f、最大 %d；NPU 输入 vs CPU：平均差 %.2f、最大 %d；灰边%s——%s\n", m1,
                w1, m2, w2, border_ok ? "完好" : "被改了", good ? "一致" : "不对");
    if (!good) {
        rc = kExitBad;
    }
    if (!save_dir.empty()) {
        const bool s1 = media::save_ppm(prev_img, save_dir + "/rga_preview.ppm");
        const bool s2 = media::save_ppm(npu_all, save_dir + "/rga_npu_input.ppm");
        const bool s3 = media::save_ppm(ref, save_dir + "/cpu_reference.ppm");
        std::printf("已保存     %s/rga_preview.ppm、rga_npu_input.ppm、cpu_reference.ppm%s\n", save_dir.c_str(),
                    s1 && s2 && s3 ? "" : " —— 有文件写失败");
    }
    return rc;
}

#else

int rga_bench(const config::AppConfig & /*cfg*/, int /*frames*/, const std::string & /*save_dir*/)
{
    std::printf("这次编译没有 MPP 或 RGA（不是板子，或者没装 librockchip-mpp-dev、librga-dev），--rga-bench 用不了\n");
    return 2;
}

#endif

}  // namespace rksec::app
