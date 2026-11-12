#pragma once
#include "app/detect_pipeline.hpp"
#include "core/config.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
namespace rksec::stream {
class MqttPublisher;
}
namespace rksec::app {
// UI/主循环只投递有界消息、读取状态；GPIO/JPEG/SQLite 都在工作线程。
class AccessService {
  public:
    explicit AccessService(config::AppConfig cfg, stream::MqttPublisher *mqtt = nullptr);
    ~AccessService();
    AccessService(const AccessService &) = delete;
    AccessService &operator=(const AccessService &) = delete;
    void pump(DetectPipeline &pipe, const DetectResult *result, const media::Image *preview,
              PipelineClock::time_point preview_at);
    access::View view() const;
    void stop();

  private:
    struct Packet {
        std::optional<std::vector<int>> tracks;
        PipelineClock::time_point captured{};
        std::vector<access::FaceEvent> faces;
        std::shared_ptr<media::Image> preview;
        PipelineClock::time_point preview_at{};
    };
    void run();
    void publish(const access::View &view);
    const config::AppConfig cfg_;
    stream::MqttPublisher *const mqtt_;
    BoundedQueue<Packet> input_{8};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> pir_active_{false};
    std::atomic<std::uint64_t> session_{0};
    mutable std::mutex mutex_;
    access::View view_;
    std::thread worker_;
};
}  // namespace rksec::app
