#include "app/decode_bench.hpp"

#include <turbojpeg.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/capture_run.hpp"
#include "app/proc_stats.hpp"
#include "capture/v4l2_camera.hpp"
#include "core/log.hpp"
#include "core/stage_timer.hpp"
#include "media/jpeg_decoder.hpp"
#include "media/yuv_frame.hpp"
#ifdef RKSEC_HAVE_MPP
#include "media/mpp_jpeg_decoder.hpp"
#endif

namespace rksec::app {

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kExitOk = 0;
constexpr int kExitDevice = 4;
#ifdef RKSEC_HAVE_MPP
constexpr int kExitBad = 1;
// 硬件和 CPU 的 IDCT 实现不同，亮度差一两级正常；平均差超过这个值说明解错了（错位、格式不对）
constexpr double kMaxMeanDiff = 2.0;
#endif

struct Result {
    double cpu_percent = 0;  // 解码期间进程 CPU（100% = 一个核）
};

// 计时跑一遍：fn 处理第 i 帧，返回是否成功
template <typename Fn> Result timed(const std::vector<std::vector<std::uint8_t>> &frames, StageStats &st, Fn fn)
{
    const double cpu0 = cpu_seconds();
    const auto t0 = Clock::now();
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto s = Clock::now();
        if (fn(i)) {
            st.record(Clock::now() - s);
        }
    }
    const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
    return Result{wall > 0 ? (cpu_seconds() - cpu0) / wall * 100.0 : 0};
}

bool capture_frames(const config::AppConfig &cfg, int n, std::vector<std::vector<std::uint8_t>> &out)
{
    capture::V4l2Camera cam{capture::real_sys(), service_options(cfg).camera};
    std::string err;
    if (!cam.open(err) || !cam.start(err)) {
        LOG_E("camera", "%s", err.c_str());
        return false;
    }
    int timeouts = 0;
    while (static_cast<int>(out.size()) < n) {
        auto r = cam.next(2000);
        if (r.status == capture::Status::error) {
            LOG_E("camera", "%s", r.error.c_str());
            return false;
        }
        if (r.status == capture::Status::timeout) {
            if (++timeouts >= 3) {
                LOG_E("camera", "6 秒没有新帧");
                return false;
            }
            continue;
        }
        timeouts = 0;
        const Frame &f = *r.frame;
        out.emplace_back(f.data, f.data + f.bytes);
    }
    return true;
}

}  // namespace

int decode_bench(const config::AppConfig &cfg, int frames, const std::string &save_path)
{
    std::vector<std::vector<std::uint8_t>> jpegs;
    jpegs.reserve(static_cast<std::size_t>(frames));
    LOG_I("bench", "从摄像头取 %d 帧……", frames);
    if (!capture_frames(cfg, frames, jpegs)) {
        return kExitDevice;
    }
    std::size_t total = 0;
    for (const auto &j : jpegs) {
        total += j.size();
    }
    std::printf("样本       %zu 帧 MJPG，平均 %zu 字节\n", jpegs.size(), total / jpegs.size());

    int rc = kExitOk;

    // ---------- CPU：libjpeg-turbo ----------
    {
        // 和硬件解码同样的工作：全尺寸、输出 YUV（不转 RGB）
        tjhandle tj = tjInitDecompress();
        std::vector<unsigned char> yuv;
        StageStats full{"CPU 解码 全尺寸 → YUV（和硬件同样的工作）"};
        const Result r1 = timed(jpegs, full, [&](std::size_t i) {
            int w = 0;
            int h = 0;
            int ss = 0;
            int cs = 0;
            const auto len = static_cast<unsigned long>(jpegs[i].size());
            if (tjDecompressHeader3(tj, jpegs[i].data(), len, &w, &h, &ss, &cs) != 0) {
                return false;
            }
            yuv.resize(tjBufSizeYUV2(w, 1, h, ss));
            return tjDecompressToYUV2(tj, jpegs[i].data(), len, yuv.data(), w, 1, h, TJFLAG_FASTDCT) == 0;
        });
        tjDestroy(tj);
        std::printf("%s，CPU %.0f%%\n", full.report().c_str(), r1.cpu_percent);

        // 第 4b 步预览用的方法：解码时缩小到 640 宽，输出 XRGB
        media::JpegDecoder dec;
        media::Image img;
        std::string err;
        StageStats half{"CPU 解码 缩小到 640 宽 → XRGB（4b 预览）"};
        const Result r2 = timed(
            jpegs, half, [&](std::size_t i) { return dec.decode(jpegs[i].data(), jpegs[i].size(), 640, img, err); });
        std::printf("%s，CPU %.0f%%\n", half.report().c_str(), r2.cpu_percent);
    }

#ifdef RKSEC_HAVE_MPP
    // ---------- MPP 硬件解码 ----------
    media::MppJpegDecoder mpp;
    std::string err;
    if (!mpp.open({}, err)) {
        LOG_E("mpp", "%s", err.c_str());
        return kExitDevice;
    }
    StageStats hw{"MPP 硬件解码 全尺寸"};
    media::MppJpegDecoder::Frame first;
    const Result r3 = timed(jpegs, hw, [&](std::size_t i) {
        auto f = mpp.decode(jpegs[i].data(), jpegs[i].size(), err);
        if (!f) {
            if (mpp.stats().failed <= 3) {
                LOG_W("mpp", "第 %zu 帧：%s", i, err.c_str());
            }
            return false;
        }
        if (i == 0) {
            first = f;  // 留着第一帧做检查（占着一块输出缓冲区，池子有 4 块，不影响后面）
        }
        return true;
    });
    const auto ms = mpp.stats();
    std::printf("%s，CPU %.0f%%；成功 %llu，失败 %llu，没有空缓冲区 %llu\n", hw.report().c_str(), r3.cpu_percent,
                static_cast<unsigned long long>(ms.ok), static_cast<unsigned long long>(ms.failed),
                static_cast<unsigned long long>(ms.no_buffer));

    if (first) {
        const media::YuvLayout &l = first->layout;
        std::printf("输出格式   %s %dx%d，每行 %d 字节，%d 行，fd %d\n", media::format_name(l.format), l.width,
                    l.height, l.hstride, l.vstride, first->fd);

        // 对比亮度：libjpeg-turbo 精确 IDCT 解出 Y 平面
        tjhandle tj = tjInitDecompress();
        int w = 0;
        int h = 0;
        int ss = 0;
        int cs = 0;
        tjDecompressHeader3(tj, jpegs[0].data(), static_cast<unsigned long>(jpegs[0].size()), &w, &h, &ss, &cs);
        std::vector<unsigned char> yuv(tjBufSizeYUV2(w, 1, h, ss));
        const bool tj_ok = tjDecompressToYUV2(tj, jpegs[0].data(), static_cast<unsigned long>(jpegs[0].size()),
                                              yuv.data(), w, 1, h, TJFLAG_ACCURATEDCT) == 0;
        tjDestroy(tj);
        if (!tj_ok || w != l.width || h != l.height) {
            std::printf("对比       libjpeg-turbo 解不出或尺寸不同（%dx%d），跳过\n", w, h);
        } else {
            long long sum = 0;
            int worst = 0;
            for (int y = 0; y < h; ++y) {
                const std::uint8_t *a = first->data + static_cast<std::size_t>(y) * static_cast<std::size_t>(l.hstride);
                const unsigned char *b = yuv.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
                for (int x = 0; x < w; ++x) {
                    const int d = std::abs(static_cast<int>(a[x]) - static_cast<int>(b[x]));
                    sum += d;
                    worst = d > worst ? d : worst;
                }
            }
            const double mean = static_cast<double>(sum) / (static_cast<double>(w) * h);
            const bool good = mean <= kMaxMeanDiff;
            std::printf("对比       硬件 vs CPU 亮度：平均差 %.2f 级，最大差 %d 级（满量程 255）——%s\n", mean, worst,
                        good ? "一致" : "差太多，硬件解码结果不对");
            if (!good) {
                rc = kExitBad;
            }
        }
        if (!save_path.empty()) {
            media::Image rgb;
            media::yuv_to_xrgb(*first, rgb);
            std::printf("已保存     %s（第 1 帧，硬件解码 → CPU 转 RGB）%s\n", save_path.c_str(),
                        media::save_ppm(rgb, save_path) ? "" : " —— 写文件失败");
        }
    }
#else
    std::printf("MPP        这次编译没有 MPP（不是板子，或者没装 librockchip-mpp-dev），只测了 CPU\n");
    (void)save_path;
#endif
    return rc;
}

}  // namespace rksec::app
