#include "app/ui_run.hpp"

#include <pthread.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/access_service.hpp"
#include "app/capture_run.hpp"
#include "app/detect_pipeline.hpp"
#include "app/hw_pipeline.hpp"
#include "app/pass_history.hpp"
#include "app/proc_stats.hpp"
#include "app/recording.hpp"
#include "core/bounded_queue.hpp"
#include "core/log.hpp"
#include "core/stage_timer.hpp"
#include "core/version.hpp"
#include "detect/coco_names.hpp"
#include "display/drm_display.hpp"
#include "media/jpeg_decoder.hpp"
#include "stream/mqtt_publisher.hpp"
#include "ui/latest_image.hpp"
#include "ui/lvgl_port.hpp"
#include "ui/router.hpp"
#include "ui/screens.hpp"
#include "ui/text.hpp"
#include "ui/theme.hpp"

namespace rksec::app {

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kExitDevice = 4;
constexpr auto kStatusEvery = std::chrono::seconds(1);
constexpr int kLogEverySteps = 10;                            // 每 10 次状态更新（10 秒）写一行日志
constexpr auto kBoxesStale = std::chrono::milliseconds(500);  // 这么久没有新的检测结果，框就不画了（检测停了）

double seconds_between(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double>(b - a).count();
}

// 解码线程：队列里取帧 → 解码缩小 → 放进信箱。队列关闭后退出
void preview_loop(BoundedQueue<FrameRef> &queue, ui::LatestImage &latest, StageStats &decode_time,
                  media::JpegDecoder::Stats &final_stats)
{
    pthread_setname_np(pthread_self(), "preview");
    media::JpegDecoder dec;
    std::string err;
    std::uint64_t failures_logged = 0;
    while (auto f = queue.pop()) {
        const Frame &frame = **f;
        const std::uint64_t seq = frame.sequence;
        const auto t0 = Clock::now();
        const bool ok = dec.decode(frame.data, frame.bytes, ui::theme::kPreviewW, latest.back(), err);
        decode_time.record(Clock::now() - t0);
        f.reset();  // 解码完立刻还缓冲区给驱动，不等显示；之后不能再用 frame
        if (ok) {
            latest.publish();
        } else if (failures_logged++ < 5) {
            LOG_W("preview", "第 %llu 帧解码失败：%s", static_cast<unsigned long long>(seq), err.c_str());
        }
    }
    final_stats = dec.stats();
}

// 跟踪结果（原画面坐标）→ 预览上的框。只画已确认的轨迹（连续 3 帧才确认，单帧误检不会闪一下）。
// 标签：编号 + 类别（人用中文，其他用英文名）+ 分数，比如“#3 人 87%”；
// 开了人脸识别（第 7c 步）：认出来的“#3 赵小明”（绿框），陌生人“#3 陌生人”（橙框），还没结论的照旧（蓝框）
std::vector<ui::OverlayBox> to_overlay(const DetectResult &r, int src_w, int src_h, bool recognizing)
{
    std::vector<ui::OverlayBox> out;
    const double sx = static_cast<double>(ui::theme::kPreviewW) / src_w;
    const double sy = static_cast<double>(ui::theme::kPreviewH) / src_h;
    for (std::size_t i = 0; i < r.tracks.size(); ++i) {
        const track::Track &t = r.tracks[i];
        const detect::Detection &d = t.box;
        ui::OverlayBox b;
        b.x = static_cast<int>(static_cast<double>(d.x1) * sx);
        b.y = static_cast<int>(static_cast<double>(d.y1) * sy);
        b.w = static_cast<int>(static_cast<double>(d.x2 - d.x1) * sx);
        b.h = static_cast<int>(static_cast<double>(d.y2 - d.y1) * sy);
        const face::Identity id = i < r.identities.size() ? r.identities[i] : face::Identity{};
        if (id.state == face::IdState::known) {
            b.label = "#" + std::to_string(t.id) + " " + id.name;
            b.color = ui::theme::kOk;
        } else if (id.state == face::IdState::stranger) {
            b.label = "#" + std::to_string(t.id) + " " + ui::text::kStranger;
            b.color = ui::theme::kWarn;
        } else {
            std::array<char, 48> buf{};
            std::snprintf(buf.data(), buf.size(), "#%d %s %.0f%%", t.id,
                          d.cls == 0 ? ui::text::kPerson : std::string{detect::coco_name(d.cls)}.c_str(),
                          static_cast<double>(d.score) * 100.0);
            b.label = buf.data();
            b.color = recognizing && d.cls == 0 ? ui::theme::kAccent : ui::theme::kOk;
        }
        out.push_back(std::move(b));
    }
    return out;
}

}  // namespace

int ui_run(const config::AppConfig &cfg)
{
    install_stop_signals();

    // ---------- 显示 ----------
    display::DrmDisplay drm;
    std::string err;
    if (!drm.open({cfg.display.device, cfg.display.connector}, err)) {
        LOG_E("display", "显示设备打不开：%s", err.c_str());
        return kExitDevice;
    }
    LOG_I("display", "%s", drm.summary().c_str());
    if (drm.width() != ui::theme::kScreenW || drm.height() != ui::theme::kScreenH) {
        LOG_W("display", "界面按 %dx%d 设计，当前屏幕 %dx%d，显示可能不完整", ui::theme::kScreenW, ui::theme::kScreenH,
              drm.width(), drm.height());
    }

    ui::lvgl_init();
    std::uint64_t present_failures = 0;
    auto ram = std::make_unique<ui::RamDisplay>(drm.width(), drm.height(),
                                                [&drm, &present_failures](const std::uint8_t *px, std::size_t stride) {
                                                    std::string e;
                                                    if (drm.present(px, stride, e)) {
                                                        return true;
                                                    }
                                                    if (present_failures++ < 5) {  // 前几次写日志，之后只计数，不刷屏
                                                        LOG_W("display", "显示一帧失败：%s", e.c_str());
                                                    }
                                                    return false;
                                                });

    // ---------- 触摸 ----------
    lv_indev_t *touch = nullptr;
    if (!cfg.display.touch.empty()) {
        touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, cfg.display.touch.c_str());
        if (touch == nullptr) {
            LOG_W("touch", "触摸屏打不开：%s（插好了吗？用户在 input 组吗？）界面照常显示，只是不能点",
                  cfg.display.touch.c_str());
        } else {
            LOG_I("touch", "触摸屏 %s", cfg.display.touch.c_str());
        }
    }

    // ---------- 界面 ----------
    ui::Router router;
    ui::DeviceInfo info;
    info.display = display::short_describe(drm.mode());
    info.camera = std::to_string(cfg.camera.width) + "x" + std::to_string(cfg.camera.height) + " MJPG";
    info.touch = cfg.display.touch;
    info.version = version::full();
    auto screens = std::make_unique<ui::Screens>(ram->display(), router, info);

    // ---------- 采集、解码、检测 ----------
    // 板子上：MPP 解码 + RGA + NPU 检测（第 6 步的流水线）；没有这些硬件库（或打不开）时退回第 4b 步的 CPU 解码，不检测
    ui::LatestImage latest;
    std::unique_ptr<HwPipeline> hw;
    std::unique_ptr<Recording> rec;  // 事件录像（第 8b 步）。在 pipe 前面声明：pipe 先析构
    std::unique_ptr<DetectPipeline> pipe;
    if (HwPipeline::available()) {
        const int want = cfg.detect.enabled ? cfg.detect.workers : 0;
        hw = HwPipeline::open(cfg, want, err);
        if (!hw) {
            LOG_W("pipeline", "硬件流水线打不开：%s；退回 CPU 解码，不检测", err.c_str());
        } else {
            if (want > 0 && hw->workers() == 0) {
                LOG_W("detect", "检测起不来：%s；只显示预览", hw->detect_error().c_str());
            } else if (want > 0) {
                LOG_I("detect", "推理线程 %d 个；%s", hw->workers(), hw->describe().c_str());
                if (!hw->recognizing()) {
                    LOG_W("face", "不做人脸识别：%s", hw->face_error().c_str());
                }
            } else {
                LOG_I("detect", "配置里关闭了检测（detect.enabled = false）");
            }
            DetectHooks hooks = hw->hooks();
            rec = Recording::open(cfg, hw->workers() > 0 && hw->recognizing(), err);
            if (rec) {
                LOG_I("video", "%s", rec->describe().c_str());
                rec->attach(hooks);
            } else {
                LOG_W("video", "不编码：%s", err.c_str());
            }
            pipe = std::make_unique<DetectPipeline>(hw->options(cfg), hooks, latest);
            pipe->start();
        }
    }
    BoundedQueue<FrameRef> queue{1};  // CPU 路径：只要最新一帧；解码慢了丢旧帧，旧帧马上还给驱动
    StageStats decode_time{"MJPG 解码（CPU，缩小到 640x360）", 1024};
    media::JpegDecoder::Stats dec_stats;
    std::thread preview;
    if (!pipe) {
        preview = std::thread([&] { preview_loop(queue, latest, decode_time, dec_stats); });
    }
    DetectPipeline *p = pipe.get();
    capture::CaptureService svc{capture::real_sys(), service_options(cfg), [&queue, p](FrameRef f) {
                                    if (p != nullptr) {
                                        p->push(std::move(f));
                                    } else {
                                        queue.push(std::move(f));
                                    }
                                }};
    svc.start();

    auto mqtt = stream::start_mqtt(cfg.mqtt);
    std::unique_ptr<AccessService> gate;
    if (cfg.access.enabled) {
        if (pipe && pipe->recognizing()) {
            try {
                gate = std::make_unique<AccessService>(cfg, mqtt.get());
            } catch (const std::exception &) {
                screens->show_access("门禁不可用");
                LOG_E("access", "门禁启动失败，监控继续运行");
            }
        } else {
            screens->show_access("识别不可用");
        }
    }
    PassHistory history{cfg.face.database};
    // ---------- 主循环（界面线程） ----------
    const auto t0 = Clock::now();
    const double cpu0 = cpu_seconds();
    const long rss0 = rss_kb();
    const auto screen_timeout = std::chrono::seconds(cfg.display.idle_timeout_seconds);
    bool auto_sleep = screen_timeout.count() != 0 && drm.supports_power_control();
    if (screen_timeout.count() != 0 && !auto_sleep) {
        LOG_W("display", "HDMI 不支持 DPMS，保持亮屏");
    }
    auto last_motion = t0;
    auto next_wake_retry = t0;
    bool wake_pending = false;
    auto last_preview = t0;
    auto last = t0;
    double cpu_last = cpu0;
    std::uint64_t frames_last = 0;
    std::uint64_t ui_frames_last = 0;
    std::uint64_t results_last = 0;
    auto next_status = t0;
    auto last_result = t0;
    bool boxes_shown = false;
    int steps = 0;
    LOG_I("ui", "界面启动（Ctrl+C 或 systemctl stop 结束）");

    while (!stop_requested()) {
        if (auto result = history.take()) {
            screens->show_passes(result->rows, result->error);
        }
        if (const media::Image *img = latest.take()) {
            screens->show_preview(*img);
            last_preview = Clock::now();
        }
        const auto now = Clock::now();
        if (p != nullptr) {
            if (auto r = p->take_result()) {
                screens->show_boxes(to_overlay(*r, cfg.camera.width, cfg.camera.height, p->recognizing()));
                if (rec) {
                    rec->on_result(*r, &latest.front());
                }
                if (gate) {
                    gate->pump(*p, &*r, &latest.front(), last_preview);
                }
                boxes_shown = !r->tracks.empty();
                last_result = now;
            } else {
                if (gate) {
                    gate->pump(*p, nullptr, &latest.front(), last_preview);
                }
                if (boxes_shown && now - last_result > kBoxesStale) {
                    screens->show_boxes({});
                    boxes_shown = false;
                }
            }
        }
        bool motion_available = false;
        if (gate) {
            const auto state = gate->view();
            motion_available = !state.pir_degraded;
            screens->show_access(access::status_text(state));
            if (state.pir_active) {
                last_motion = now;
                wake_pending = true;
            }
        }
        if (rec) {
            rec->tick();  // 没有新结果时也让录像的收尾计时往前走
        }
        if (now >= next_status) {
            next_status += kStatusEvery;
            const double dt = seconds_between(last, now);
            const auto cs = svc.stats();
            const double cpu = cpu_seconds();
            ui::UiStatus st;
            st.camera_ok = cs.streaming;
            st.camera_fps = dt > 0 ? static_cast<double>(cs.frames - frames_last) / dt : 0;
            st.decode_p50_ms = p != nullptr ? p->decode_time.summary().p50_ms : decode_time.summary().p50_ms;
            st.detecting = p != nullptr && p->detecting();
            st.detect_p50_ms = p != nullptr ? p->infer_time.summary().p50_ms : 0;
            st.lost = cs.lost;
            st.ui_fps = dt > 0 ? static_cast<double>(drm.stats().presented - ui_frames_last) / dt : 0;
            st.cpu_percent = dt > 0 ? (cpu - cpu_last) / dt * 100.0 : 0;
            if (!cs.streaming) {
                screens->clear_preview();
                boxes_shown = false;
            }
            screens->update_status(st);
            if (++steps % kLogEverySteps == 0) {
                LOG_I("ui", "摄像头 %s %.1f fps，丢帧 %llu；解码 p50 %.1f ms；界面 %.0f fps；CPU %.0f%%；RSS %ld KB",
                      cs.streaming ? "出帧" : "未连接", st.camera_fps, static_cast<unsigned long long>(cs.lost),
                      st.decode_p50_ms, st.ui_fps, st.cpu_percent, rss_kb());
                if (p != nullptr && p->detecting()) {
                    const auto ps = p->stats();
                    const double span =
                        static_cast<double>(kLogEverySteps) * std::chrono::duration<double>(kStatusEvery).count();
                    LOG_I("detect", "检测 %.1f 次/秒，跳过 %llu；推理 p50 %.1f ms；端到端 p50 %.1f ms p99 %.1f ms",
                          static_cast<double>(ps.results - results_last) / span,
                          static_cast<unsigned long long>(ps.skipped_busy), st.detect_p50_ms,
                          p->detect_latency.summary().p50_ms, p->detect_latency.summary().p99_ms);
                    results_last = ps.results;
                }
            }
            last = now;
            cpu_last = cpu;
            frames_last = cs.frames;
            ui_frames_last = drm.stats().presented;
        }

        const std::uint32_t idle = lv_timer_handler();  // 处理输入、动画，有变化就重画并翻页
        if (auto_sleep) {
            const auto screen_now = Clock::now();
            const auto touch_idle = std::chrono::milliseconds(lv_display_get_inactive_time(ram->display()));
            const bool touch_held = touch != nullptr && lv_indev_get_state(touch) == LV_INDEV_STATE_PRESSED;
            const bool can_wake = touch != nullptr || motion_available;
            if (drm.sleeping()) {
                wake_pending = wake_pending || !can_wake || touch_held || touch_idle < screen_timeout;
                if (wake_pending && screen_now >= next_wake_retry) {
                    if (drm.set_sleeping(false, err)) {
                        lv_display_trigger_activity(ram->display());
                        lv_obj_invalidate(lv_display_get_screen_active(ram->display()));
                        last_motion = screen_now;
                        LOG_I("display", "屏幕已唤醒");
                    } else {
                        next_wake_retry = screen_now + std::chrono::seconds(1);
                        LOG_W("display", "%s，将重试唤醒", err.c_str());
                    }
                }
            } else if (can_wake && !touch_held && touch_idle >= screen_timeout &&
                       screen_now - last_motion >= screen_timeout) {
                wake_pending = false;
                if (drm.set_sleeping(true, err)) {
                    next_wake_retry = screen_now;
                    LOG_I("display", "屏幕已息屏");
                } else {
                    auto_sleep = false;
                    LOG_W("display", "%s，保持亮屏", err.c_str());
                }
            }
        }
        const std::uint32_t wait = std::min<std::uint32_t>(idle, 5);  // 最多睡 5 ms，及时拿到新的预览帧
        std::this_thread::sleep_for(std::chrono::milliseconds(wait));
    }

    // ---------- 收尾：先停数据来源，再拆界面，最后还显示 ----------
    svc.stop();
    if (gate) {
        gate->stop();
    }
    if (p != nullptr) {
        p->stop();
        if (rec) {
            rec->stop();  // 剩下的帧编完，正在录的事件就地结束
        }
    } else {
        queue.close();
        preview.join();
    }
    drm.wait_idle();

    const double secs = seconds_between(t0, Clock::now());
    const double cpu_avg = secs > 0 ? (cpu_seconds() - cpu0) / secs * 100.0 : 0;
    const auto cs = svc.stats();
    const auto ds = drm.stats();
    const auto ls = latest.stats();
    const std::uint64_t ui_frames = ds.presented;

    if (touch != nullptr) {
        lv_evdev_delete(touch);
    }
    screens.reset();
    ram.reset();
    ui::lvgl_deinit();
    drm.close();  // 恢复控制台画面

    if (rec && rec->recorder() != nullptr) {
        const auto rs = rec->recorder()->stats();
        std::printf("录像       事件 %llu 个，文件 %llu 个（最后一个 %s）；写盘错误 %llu；超配额删除 %llu 个\n",
                    static_cast<unsigned long long>(rs.events), static_cast<unsigned long long>(rs.files),
                    rs.last_file.empty() ? "无" : rs.last_file.c_str(),
                    static_cast<unsigned long long>(rs.write_errors), static_cast<unsigned long long>(rs.deleted));
    }
    if (rec && rec->rtsp() != nullptr) {
        const auto ss = rec->rtsp()->stats();
        std::printf("RTSP       连过 %llu 次，推出 %llu 个包；等关键帧扔 %llu，网络积压扔 %llu\n",
                    static_cast<unsigned long long>(ss.clients_total), static_cast<unsigned long long>(ss.pushed),
                    static_cast<unsigned long long>(ss.waiting_key), static_cast<unsigned long long>(ss.congested));
    }
    std::printf("运行       %.0f 秒\n", secs);
    std::printf("界面       %llu 帧（平均 %.1f fps）；翻页需要等待 %llu 次，超时 %llu 次，显示失败 %llu 次\n",
                static_cast<unsigned long long>(ui_frames), secs > 0 ? static_cast<double>(ui_frames) / secs : 0.0,
                static_cast<unsigned long long>(ds.flip_waits), static_cast<unsigned long long>(ds.flip_timeouts),
                static_cast<unsigned long long>(present_failures));
    std::printf("摄像头     %llu 帧（平均 %.2f fps），丢帧 %llu，重连 %llu 次\n",
                static_cast<unsigned long long>(cs.frames), secs > 0 ? static_cast<double>(cs.frames) / secs : 0.0,
                static_cast<unsigned long long>(cs.lost),
                static_cast<unsigned long long>(cs.sessions > 0 ? cs.sessions - 1 : 0));
    if (p != nullptr) {
        const auto ps = p->stats();
        std::printf("流水线     解码 %llu（失败 %llu），输入被新帧挤掉 %llu；送去推理 %llu，推理线程都忙跳过 %llu\n",
                    static_cast<unsigned long long>(ps.decoded), static_cast<unsigned long long>(ps.decode_failed),
                    static_cast<unsigned long long>(ps.input_dropped), static_cast<unsigned long long>(ps.jobs),
                    static_cast<unsigned long long>(ps.skipped_busy));
        std::printf("检测       结果 %llu 个，推理失败 %llu；乱序 %llu 次；界面没来得及拿被覆盖 %llu 个\n",
                    static_cast<unsigned long long>(ps.results), static_cast<unsigned long long>(ps.infer_failed),
                    static_cast<unsigned long long>(ps.reordered),
                    static_cast<unsigned long long>(ps.results_overwritten));
        std::printf("跟踪       出现过 %llu 个编号，删除 %llu 个，被挡住后找回 %llu 次，结束时还有 %zu 个\n",
                    static_cast<unsigned long long>(ps.tracks.created),
                    static_cast<unsigned long long>(ps.tracks.removed),
                    static_cast<unsigned long long>(ps.tracks.recovered), ps.tracks.active);
        if (p->recognizing()) {
            const auto &f = ps.faces;
            const std::uint64_t face_failed = ps.face_input_failed + ps.face_infer_failed;
            std::printf("人脸识别   %llu 次：没找到脸 %llu，认出来 %llu，陌生人 %llu；采纳新结论 %llu 次；失败 %llu\n",
                        static_cast<unsigned long long>(f.attempts), static_cast<unsigned long long>(f.no_face),
                        static_cast<unsigned long long>(f.matched), static_cast<unsigned long long>(f.unmatched),
                        static_cast<unsigned long long>(f.adopted), static_cast<unsigned long long>(face_failed));
            std::printf("%s", face_report(ps, cfg.face.match_threshold).c_str());
        }
        for (const StageStats *s : {&p->decode_time, &p->preview_time, &p->npu_input_time, &p->infer_time,
                                    &p->preview_latency, &p->detect_latency, &p->face_input_time, &p->face_time}) {
            std::printf("%s\n", s->report().c_str());
        }
    } else {
        std::printf("队列       丢弃 %llu 帧（解码跟不上时丢旧帧）\n",
                    static_cast<unsigned long long>(queue.stats().dropped));
        std::printf("解码       成功 %llu（其中数据有问题 %llu），失败 %llu\n",
                    static_cast<unsigned long long>(dec_stats.ok), static_cast<unsigned long long>(dec_stats.warnings),
                    static_cast<unsigned long long>(dec_stats.failed));
        std::printf("%s\n", decode_time.report().c_str());
    }
    std::printf("预览       解出 %llu 帧，界面取走 %llu 帧，被新帧覆盖 %llu 帧\n",
                static_cast<unsigned long long>(ls.published), static_cast<unsigned long long>(ls.taken),
                static_cast<unsigned long long>(ls.overwritten));
    std::printf("CPU        平均 %.0f%%（100%% = 一个核满载）\n", cpu_avg);
    std::printf("内存 RSS   %ld KB → %ld KB\n", rss0, rss_kb());
    return 0;
}

}  // namespace rksec::app
