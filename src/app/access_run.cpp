#include "app/access_run.hpp"
#include "app/access_service.hpp"
#include "app/capture_run.hpp"
#include "app/hw_pipeline.hpp"
#include "app/proc_stats.hpp"
#include "app/recording.hpp"
#include "core/log.hpp"
#include "stream/mqtt_publisher.hpp"
#include <cstdio>
#include <thread>
namespace rksec::app {
int access_run(const config::AppConfig &cfg, int seconds)
{
    if (!cfg.detect.enabled || !cfg.face.recognize) {
        std::fprintf(stderr, "门禁需要检测与人脸识别\n");
        return 3;
    }
    install_stop_signals();
    std::string error;
    auto hw = HwPipeline::open(cfg, cfg.detect.workers, error);
    if (!hw || !hw->workers() || !hw->recognizing()) {
        LOG_E("access", "识别不可用，门禁不启动");
        return 4;
    }
    auto recording = Recording::open(cfg, true, error);
    auto hooks = hw->hooks();
    if (recording) {
        recording->attach(hooks);
    }
    ui::LatestImage preview;
    DetectPipeline pipe{hw->options(cfg), hooks, preview};
    auto mqtt = stream::start_mqtt(cfg.mqtt);
    AccessService gate{cfg, mqtt.get()};
    pipe.start();
    capture::CaptureService camera{capture::real_sys(), service_options(cfg),
                                   [&](FrameRef frame) { pipe.push(std::move(frame)); }};
    camera.start();
    const auto start = PipelineClock::now();
    auto preview_at = start;
    std::string previous;
    auto next_health = start + std::chrono::seconds(5);
    while (!stop_requested() && PipelineClock::now() - start < std::chrono::seconds(seconds)) {
        if (preview.take()) {
            preview_at = PipelineClock::now();
        }
        auto result = pipe.take_result();
        if (result && recording) {
            recording->on_result(*result, &preview.front());
        }
        gate.pump(pipe, result ? &*result : nullptr, &preview.front(), preview_at);
        if (recording) {
            recording->tick();
        }
        const auto status = access::status_text(gate.view());
        if (status != previous) {
            std::printf("[access] %s\n", status.c_str());
            std::fflush(stdout);
            previous = status;
        }
        if (PipelineClock::now() >= next_health) {
            next_health = PipelineClock::now() + std::chrono::seconds(5);
            const auto camera_state = camera.stats();
            const auto pipeline_state = pipe.stats();
            std::printf("[access-health] camera=%s frames=%llu detections=%llu face_attempts=%llu pir=%s\n",
                        camera_state.streaming ? "streaming" : "waiting",
                        static_cast<unsigned long long>(camera_state.frames),
                        static_cast<unsigned long long>(pipeline_state.results),
                        static_cast<unsigned long long>(pipeline_state.faces.attempts),
                        gate.view().pir_degraded ? "unavailable" : (gate.view().pir_active ? "active" : "idle"));
            std::printf("[access-health] face=%s score=%.3f quality=%s\n",
                        pipeline_state.last_face.found ? "found" : "none",
                        static_cast<double>(pipeline_state.last_face.score),
                        face::quality_text(pipeline_state.last_face.quality));
            if (!camera_state.streaming && !camera_state.last_error.empty()) {
                std::printf("[access-health] %s\n", camera_state.last_error.c_str());
            }
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    camera.stop();
    gate.stop();
    pipe.stop();
    if (recording) {
        recording->stop();
    }
    const auto state = gate.view();
    std::printf("门禁结束：通行记录成功 %llu，失败 %llu\n", static_cast<unsigned long long>(state.grants),
                static_cast<unsigned long long>(state.write_errors));
    return state.error.empty() && camera.stats().frames > 0 && pipe.stats().results > pipe.stats().infer_failed ? 0 : 4;
}
}  // namespace rksec::app
