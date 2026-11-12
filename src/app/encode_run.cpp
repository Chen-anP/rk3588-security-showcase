#include "app/encode_run.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

#include "app/capture_run.hpp"
#include "app/detect_pipeline.hpp"
#include "app/encode_stage.hpp"
#include "app/hw_pipeline.hpp"
#include "app/proc_stats.hpp"
#include "capture/capture_service.hpp"
#include "core/log.hpp"
#include "media/h264_stream.hpp"
#include "ui/latest_image.hpp"
#if defined(RKSEC_HAVE_MPP) && defined(RKSEC_HAVE_RGA)
#include "media/mpp_h264_encoder.hpp"
#endif

namespace rksec::app {

namespace {
using Clock = std::chrono::steady_clock;
[[maybe_unused]] constexpr int kExitOk = 0;
[[maybe_unused]] constexpr int kExitNoHw = 2;
[[maybe_unused]] constexpr int kExitDevice = 4;
}  // namespace

#if defined(RKSEC_HAVE_MPP) && defined(RKSEC_HAVE_RGA)

int encode_run(const config::AppConfig &cfg_in, int seconds, const std::string &out_path)
{
    install_stop_signals();
    // 只测编码：不做检测、不做人脸识别，这样数字不受 NPU 影响
    config::AppConfig cfg = cfg_in;
    cfg.detect.enabled = false;
    cfg.face.recognize = false;
    cfg.encode.enabled = true;

    std::string err;
    auto hw = HwPipeline::open(cfg, 0, err);
    if (!hw) {
        LOG_E("pipeline", "%s", err.c_str());
        return kExitDevice;
    }
    auto enc = std::make_unique<media::MppH264Encoder>();
    const media::MppH264Encoder *enc_view = enc.get();  // 停下以后读“补了几次头”
    media::MppH264Encoder::Options eo;
    eo.width = cfg.camera.width;
    eo.height = cfg.camera.height;
    eo.fps = cfg.encode.fps;
    eo.bitrate_kbps = cfg.encode.bitrate_kbps;
    eo.gop = cfg.encode.gop;
    if (!enc->open(eo, err)) {
        LOG_E("encode", "%s", err.c_str());
        return kExitDevice;
    }
    const std::vector<std::uint8_t> headers = enc->parameter_sets();
    std::printf("编码器     H.264 High@4.1 CBR %d kbps，%d fps，每 %d 帧一个关键帧，%dx%d\n", cfg.encode.bitrate_kbps,
                cfg.encode.fps, cfg.encode.gop, eo.width, eo.height);
    std::printf("SPS/PPS    %zu 字节\n", headers.size());

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        LOG_E("encode", "打不开输出文件 %s", out_path.c_str());
        return kExitDevice;
    }

    // 码流统计：只在编码线程里改，结束（stage.stop()）之后才读
    std::uint64_t packets = 0;
    std::uint64_t keyframes_with_headers = 0;
    std::uint64_t first_frame_keyframe = 0;
    std::int64_t last_key_pts = -1;
    double key_gap_sum_s = 0;
    std::uint64_t key_gaps = 0;
    EncodeStage stage{std::move(enc), [&](const media::EncodedPacket &p) {
                          if (packets++ == 0 && p.keyframe) {
                              first_frame_keyframe = 1;
                          }
                          if (p.keyframe) {
                              if (!media::extract_parameter_sets(p.data.data(), p.data.size()).empty()) {
                                  ++keyframes_with_headers;
                              }
                              if (last_key_pts >= 0) {
                                  key_gap_sum_s += static_cast<double>(p.pts_us - last_key_pts) / 1e6;
                                  ++key_gaps;
                              }
                              last_key_pts = p.pts_us;
                          }
                          out.write(reinterpret_cast<const char *>(p.data.data()),
                                    static_cast<std::streamsize>(p.data.size()));
                      }};
    stage.start();

    DetectHooks hooks = hw->hooks();
    hooks.encode = [&stage](const std::shared_ptr<const media::YuvView> &yuv, PipelineClock::time_point captured) {
        stage.submit(yuv, std::chrono::duration_cast<std::chrono::microseconds>(captured.time_since_epoch()).count());
    };
    ui::LatestImage preview;
    DetectPipeline pipe{hw->options(cfg), hooks, preview};
    pipe.start();
    capture::CaptureService svc{capture::real_sys(), service_options(cfg),
                                [&pipe](FrameRef f) { pipe.push(std::move(f)); }};
    svc.start();
    LOG_I("encode", "运行 %d 秒，写入 %s（Ctrl+C 提前结束）", seconds, out_path.c_str());

    const auto t0 = Clock::now();
    const double cpu0 = cpu_seconds();
    const long rss0 = rss_kb();
    auto next_report = t0 + std::chrono::seconds(5);
    std::uint64_t bytes_last = 0;
    auto last = t0;
    while (!stop_requested() && Clock::now() - t0 < std::chrono::seconds(seconds)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        preview.take();
        if (Clock::now() >= next_report) {
            next_report += std::chrono::seconds(5);
            const auto now = Clock::now();
            const double dt = std::chrono::duration<double>(now - last).count();
            const auto es = stage.stats();
            LOG_I("encode", "已编 %llu 帧，实时码率 %.0f kbps，丢 %llu 帧，编码 p50 %.1f ms p99 %.1f ms",
                  static_cast<unsigned long long>(es.encoded),
                  static_cast<double>(es.bytes - bytes_last) * 8.0 / 1000.0 / dt,
                  static_cast<unsigned long long>(es.dropped), stage.encode_time.summary().p50_ms,
                  stage.encode_time.summary().p99_ms);
            bytes_last = es.bytes;
            last = now;
        }
    }
    svc.stop();
    pipe.stop();
    stage.stop();
    out.close();

    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    const auto cs = svc.stats();
    const auto ps = pipe.stats();
    const auto es = stage.stats();
    std::printf("运行       %.0f 秒\n", secs);
    std::printf("摄像头     %llu 帧（%.1f fps），丢帧 %llu\n", static_cast<unsigned long long>(cs.frames),
                static_cast<double>(cs.frames) / secs, static_cast<unsigned long long>(cs.lost));
    std::printf("解码       %llu 帧成功，%llu 失败\n", static_cast<unsigned long long>(ps.decoded),
                static_cast<unsigned long long>(ps.decode_failed));
    std::printf("编码       送入 %llu，编完 %llu（%.1f 帧/秒），编码跟不上被挤掉 %llu，失败 %llu\n",
                static_cast<unsigned long long>(es.submitted), static_cast<unsigned long long>(es.encoded),
                static_cast<double>(es.encoded) / secs, static_cast<unsigned long long>(es.dropped),
                static_cast<unsigned long long>(es.failed));
    std::printf("码流       %.2f MB，平均 %.0f kbps（目标 %d kbps）\n", static_cast<double>(es.bytes) / 1048576.0,
                static_cast<double>(es.bytes) * 8.0 / 1000.0 / secs, cfg.encode.bitrate_kbps);
    std::printf("关键帧     %llu 个，平均间隔 %.2f 秒；每个关键帧都带 SPS/PPS：%s；第一个包就是关键帧：%s\n",
                static_cast<unsigned long long>(es.keyframes),
                key_gaps > 0 ? key_gap_sum_s / static_cast<double>(key_gaps) : 0.0,
                es.keyframes == 0 ? "（没有关键帧）" : (keyframes_with_headers == es.keyframes ? "是" : "否"),
                first_frame_keyframe != 0 ? "是" : "否");
    std::printf("           其中 %llu 个关键帧的 SPS/PPS 是程序补上的（MPP 没带）\n",
                static_cast<unsigned long long>(enc_view->headers_added()));
    std::printf("%s\n%s\n", stage.encode_time.report().c_str(), pipe.preview_latency.report().c_str());
    std::printf("CPU        平均 %.0f%%（100%% = 一个核满载）\n", secs > 0 ? (cpu_seconds() - cpu0) / secs * 100.0 : 0);
    std::printf("内存 RSS   %ld KB → %ld KB\n", rss0, rss_kb());
    std::printf("输出文件   %s\n", out_path.c_str());
    if (es.encoded == 0 || es.keyframes == 0) {
        LOG_E("encode", "一帧都没编出来（或者没有关键帧），看上面摄像头、解码、编码的日志");
        return kExitDevice;
    }
    return kExitOk;
}

#else

int encode_run(const config::AppConfig & /*cfg*/, int /*seconds*/, const std::string & /*out_path*/)
{
    std::printf("这次编译没有 MPP 或 RGA，--encode-run 用不了\n");
    return kExitNoHw;
}

#endif

}  // namespace rksec::app
