#include "app/record_run.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

#include "app/capture_run.hpp"
#include "app/detect_pipeline.hpp"
#include "app/hw_pipeline.hpp"
#include "app/proc_stats.hpp"
#include "app/recording.hpp"
#include "capture/capture_service.hpp"
#include "core/log.hpp"
#include "record/event_store.hpp"
#include "ui/latest_image.hpp"

namespace rksec::app {

namespace {
using Clock = std::chrono::steady_clock;
[[maybe_unused]] constexpr int kExitOk = 0;
[[maybe_unused]] constexpr int kExitNoHw = 2;
[[maybe_unused]] constexpr int kExitDevice = 4;

// 现在事件表里最大的编号（结束时只列出这次运行产生的）
std::int64_t last_event_id(const std::string &db)
{
    std::string err;
    auto store = record::EventStore::open(db, err);
    std::vector<record::EventRow> rows;
    if (!store || !store->list(rows, err, 1) || rows.empty()) {
        return 0;
    }
    return rows.front().id;
}
}  // namespace

int record_run(const config::AppConfig &cfg, int seconds)
{
    if (!HwPipeline::available()) {
        std::printf("这次编译没有 MPP 或 RGA，--record-run 用不了\n");
        return kExitNoHw;
    }
    install_stop_signals();
    std::string err;
    auto hw = HwPipeline::open(cfg, cfg.detect.enabled ? cfg.detect.workers : 0, err);
    if (!hw) {
        LOG_E("pipeline", "%s", err.c_str());
        return kExitDevice;
    }
    if (hw->workers() == 0 || !hw->recognizing()) {
        LOG_E("record", "录像靠“陌生人”触发，要检测和人脸识别都开着：%s %s", hw->detect_error().c_str(),
              hw->face_error().c_str());
        return kExitDevice;
    }
    const std::int64_t before = last_event_id(cfg.face.database);
    auto rec = Recording::open(cfg, true, err);
    if (!rec) {
        LOG_E("record", "录像和 RTSP 都起不来：%s", err.c_str());
        return kExitDevice;
    }
    LOG_I("video", "%s", rec->describe().c_str());
    record::EventRecorder *recorder = rec->recorder();
    stream::RtspServer *rtsp = rec->rtsp();

    DetectHooks hooks = hw->hooks();
    rec->attach(hooks);
    ui::LatestImage preview;
    DetectPipeline pipe{hw->options(cfg), hooks, preview};
    pipe.start();
    capture::CaptureService svc{capture::real_sys(), service_options(cfg),
                                [&pipe](FrameRef f) { pipe.push(std::move(f)); }};
    svc.start();
    LOG_I("record", "运行 %d 秒（Ctrl+C 提前结束）。走到镜头前：没登记的人会被录下来", seconds);

    const auto t0 = Clock::now();
    const double cpu0 = cpu_seconds();
    const long rss0 = rss_kb();
    auto next_report = t0 + std::chrono::seconds(5);
    while (!stop_requested() && Clock::now() - t0 < std::chrono::seconds(seconds)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));  // 模拟界面线程
        preview.take();
        if (auto r = pipe.take_result()) {
            rec->on_result(*r, &preview.front());
        }
        rec->tick();
        if (Clock::now() >= next_report) {
            next_report += std::chrono::seconds(5);
            const auto es = rec->encoder().stats();
            const auto rs = recorder != nullptr ? recorder->stats() : record::EventRecorder::Stats{};
            LOG_I("record",
                  "%s；事件 %llu 个，文件 %llu 个；RTSP 客户端 %d 个；编码 %llu 帧，丢 %llu；端到端 p50 %.1f ms",
                  recorder == nullptr ? "不录像" : (recorder->recording() ? "录像中" : "待命"),
                  static_cast<unsigned long long>(rs.events), static_cast<unsigned long long>(rs.files),
                  rtsp != nullptr ? rtsp->clients() : -1, static_cast<unsigned long long>(es.encoded),
                  static_cast<unsigned long long>(es.dropped), pipe.detect_latency.summary().p50_ms);
        }
    }
    svc.stop();
    pipe.stop();
    rec->stop();

    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    const auto es = rec->encoder().stats();
    const auto rs = recorder != nullptr ? recorder->stats() : record::EventRecorder::Stats{};
    const auto ps = pipe.stats();
    std::printf("运行       %.0f 秒\n", secs);
    std::printf("检测       结果 %llu 个；人脸识别 %llu 次，认出来 %llu，陌生人 %llu\n",
                static_cast<unsigned long long>(ps.results), static_cast<unsigned long long>(ps.faces.attempts),
                static_cast<unsigned long long>(ps.faces.matched), static_cast<unsigned long long>(ps.faces.unmatched));
    std::printf("%s", face_report(ps, cfg.face.match_threshold).c_str());
    std::printf("编码       %llu 帧，丢 %llu，失败 %llu；%s\n", static_cast<unsigned long long>(es.encoded),
                static_cast<unsigned long long>(es.dropped), static_cast<unsigned long long>(es.failed),
                rec->encoder().encode_time.report().c_str());
    std::printf("录像       事件 %llu 个，文件 %llu 个，写入 %llu 帧（开头不是关键帧丢掉 %llu）；写盘错误 %llu，"
                "写盘队列丢 %llu；启动时恢复 %llu 个，超配额删除 %llu 个\n",
                static_cast<unsigned long long>(rs.events), static_cast<unsigned long long>(rs.files),
                static_cast<unsigned long long>(rs.packets), static_cast<unsigned long long>(rs.skipped),
                static_cast<unsigned long long>(rs.write_errors), static_cast<unsigned long long>(rs.queue_dropped),
                static_cast<unsigned long long>(rs.recovered), static_cast<unsigned long long>(rs.deleted));
    if (rtsp != nullptr) {
        const auto ss = rtsp->stats();
        std::printf(
            "RTSP       %s：连过 %llu 次，流水线建过 %llu 次；推出 %llu 个包，没人看跳过 %llu，等关键帧扔 %llu，"
            "网络积压扔 %llu\n",
            rtsp->url().c_str(), static_cast<unsigned long long>(ss.clients_total),
            static_cast<unsigned long long>(ss.sessions), static_cast<unsigned long long>(ss.pushed),
            static_cast<unsigned long long>(ss.no_viewer), static_cast<unsigned long long>(ss.waiting_key),
            static_cast<unsigned long long>(ss.congested));
    }
    std::printf("%s\n", pipe.detect_latency.report().c_str());
    std::printf("CPU        平均 %.0f%%（100%% = 一个核满载）\n", secs > 0 ? (cpu_seconds() - cpu0) / secs * 100.0 : 0);
    std::printf("内存 RSS   %ld KB → %ld KB\n", rss0, rss_kb());

    auto store = record::EventStore::open(cfg.face.database, err);
    std::vector<record::EventRow> rows;
    if (store && store->list(rows, err, 100)) {
        std::printf("这次的事件：\n");
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
            if (it->id <= before) {
                continue;
            }
            std::printf("  #%lld %s ～ %s  %.1f 秒  %.1f MB  编号 %s  %s%s  %s\n", static_cast<long long>(it->id),
                        it->started.c_str(), it->ended.c_str(), static_cast<double>(it->duration_ms) / 1000.0,
                        static_cast<double>(it->bytes) / 1048576.0, it->tracks.c_str(), it->status.c_str(),
                        it->has_snapshot ? "（有截图）" : "", it->file.c_str());
        }
    }
    return kExitOk;
}

}  // namespace rksec::app
